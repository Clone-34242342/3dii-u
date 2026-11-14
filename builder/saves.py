"""A game's save, moved between a 3DS and the Wii U through the SD card. The builder puts the files of a 3DS save (as Checkpoint, JKSM or an
emulator keep them) in sd:/3dii-u/<title id>/import, and the game takes them in as it next starts, its own save going to previous on the card
first (port/wiiu/save_storage.cpp, save_from_card). The other way, an empty export folder there asks the game for its save, which it copies
into it as it starts; the builder then copies it to the PC."""
import os
import re
import shutil
import time

ROOM = 8 << 20  # more than any 3DS game's save, and copied while the game starts within the watchdog's time (frontend_wiiu.cpp)


class SaveError(Exception):
    """what to tell the player"""


def card_folder(card, title_id):
    """the game's folder on the card, as the game names it (port/wiiu/frontend_wiiu.cpp, card_dir)"""
    return os.path.join(card.rstrip('\\/') + os.sep, '3dii-u', title_id.lower())


def junk(name):
    """what Windows and macOS leave in folders: a game that finds a file it doesn't know in its save formats it (Mario Kart 7 in Cemu)"""
    return name.lower() in ('desktop.ini', 'thumbs.db', '.ds_store') or name.startswith('._')


def files_in(folder):
    """(path relative to folder, size) of every file under it but junk"""
    return [(os.path.relpath(os.path.join(d, f), folder), os.path.getsize(os.path.join(d, f))) for d, _, names in os.walk(folder) for f in names
            if not junk(f)]


def the_save(folder):
    """The folder that holds the save's own files, from the one the player chose: an emulator's title folder holds it in data/00000001, and
    Checkpoint's folder for a game holds one dated folder per backup (20261114-050000), the newest taken. Any other is taken as it is."""
    inner = os.path.join(folder, 'data', '00000001')
    if os.path.isdir(inner):
        return inner
    names = [n for n in os.listdir(folder) if not junk(n)]
    if names and all(re.fullmatch(r'\d{8}-\d{6}', n) and os.path.isdir(os.path.join(folder, n)) for n in names):
        return os.path.join(folder, max(names))
    return folder


def put_in_place(new, target):
    """new, a complete copy, as target: what was there is moved aside first, then deleted, so a deletion cut short leaves nothing half there"""
    old = target + '_old'
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(target):
        os.rename(target, old)
    os.rename(new, target)
    shutil.rmtree(old, ignore_errors=True)


def copy_whole(source, target):
    """source's files as target, by way of target_new, compared before anything is replaced"""
    new = target + '_new'
    shutil.rmtree(new, ignore_errors=True)
    shutil.copytree(source, new, ignore=lambda _, names: [n for n in names if junk(n)])
    if sorted(files_in(new)) != sorted(files_in(source)):
        shutil.rmtree(new, ignore_errors=True)
        raise SaveError('The copy came out different from the save: the card may be failing. Nothing was replaced.')
    put_in_place(new, target)


def bring_in(card, title_id, chosen):
    """the save in the folder the player chose, onto the card for the game to take in: (the folder taken, the number of its files)"""
    source = the_save(chosen)
    found = files_in(source)
    if not found:
        raise SaveError('There are no files in that folder. Choose the folder with the save in it: Checkpoint\'s or JKSM\'s folder for the game, '
                        'or the emulator\'s folder of it.')
    if sum(size for _, size in found) > ROOM:
        raise SaveError('That is bigger than any save of a 3DS game: choose the folder of this game\'s save alone.')
    copy_whole(source, os.path.join(card_folder(card, title_id), 'import'))
    return source, len(found)


def exported(card, title_id):
    """whether the game has left its save on the card"""
    export = os.path.join(card_folder(card, title_id), 'export')
    return os.path.isdir(export) and bool(files_in(export))


def ask_for_it(card, title_id):
    """an empty export folder: the game copies its save into it as it next starts"""
    os.makedirs(os.path.join(card_folder(card, title_id), 'export'), exist_ok=True)


def take_out(card, title_id, dest):
    """The save the game left in export, copied into a new folder in dest named as Checkpoint names a backup (20261114-050000), so it can go
    straight into Checkpoint's folder for the game: that folder's path. The card's copy goes once it's here, so asking again gets a newer one."""
    export = os.path.join(card_folder(card, title_id), 'export')
    target = os.path.join(dest, time.strftime('%Y%m%d-%H%M%S'))
    if os.path.exists(target):
        raise SaveError('A folder of that name is already there: try again in a second.')
    copy_whole(export, target)
    try:
        os.rename(export, export + '_taken')
        shutil.rmtree(export + '_taken', ignore_errors=True)
    except OSError:  # still on the card: taken again next time, which does no harm
        pass
    return target
