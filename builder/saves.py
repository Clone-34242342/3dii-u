"""A 3DS save packed into a game as it is built: the files of a save, as Checkpoint, JKSM or an emulator keep them, go into the title as
save_import, and the game takes them in on the console once it has made a save of its own (port/wiiu/save_storage.cpp, save_from_content).
No SD card: the save travels inside the game."""
import os
import re
import shutil
import stat
import tempfile

ROOM = 8 << 20  # more than any 3DS game's save, and copied while the game starts within the watchdog's time (frontend_wiiu.cpp)


class SaveError(Exception):
    """what to tell the player"""


def junk(name):
    """what Windows and macOS leave in folders: a game that finds a file it doesn't know in its save formats it (Mario Kart 7 in Cemu)"""
    return name.lower() in ('desktop.ini', 'thumbs.db', '.ds_store') or name.startswith('._')


def save_stat(path):
    info = os.lstat(path)
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise SaveError(f'Save folders cannot contain links or reparse points: {path}')
    return info


def save_directory(path):
    folder = root = os.path.abspath(path)
    parents = [root]
    while os.path.dirname(root) != root:
        root = os.path.dirname(root)
        parents.append(root)
    for directory in reversed(parents):
        if not stat.S_ISDIR(save_stat(directory).st_mode):
            raise SaveError(f'This save path is not a directory: {directory}')
    return folder


def files_in(folder):
    """(path relative to folder, size) of every file under it but junk"""
    return [(name, info.st_size) for name, info in file_stats(folder)]


def file_stats(folder):
    folder = os.path.abspath(folder)
    found, todo = [], [folder]
    while todo:
        with os.scandir(save_directory(todo.pop())) as entries:
            for entry in entries:
                info = save_stat(entry.path)
                if junk(entry.name):
                    continue
                if stat.S_ISDIR(info.st_mode):
                    todo.append(entry.path)
                elif stat.S_ISREG(info.st_mode):
                    found.append((os.path.relpath(entry.path, folder), info))
                else:
                    raise SaveError(f'This save entry is not a regular file: {entry.path}')
    return found


def the_save(folder):
    """The folder that holds the save's own files, from the one the player chose: an emulator's title folder holds it in data/00000001, and
    Checkpoint's folder for a game holds one dated folder per backup (20261114-050000, optionally followed by a label), the newest taken.
    Any other is taken as it is."""
    folder = save_directory(folder)
    data = os.path.join(folder, 'data')
    inner = os.path.join(data, '00000001')
    try:
        if stat.S_ISDIR(save_stat(data).st_mode) and stat.S_ISDIR(save_stat(inner).st_mode):
            return inner
    except FileNotFoundError:
        pass
    names = [n for n in os.listdir(folder) if not junk(n)]
    if names and all(re.fullmatch(r'\d{8}-\d{6}(?:\D.*)?', n) and stat.S_ISDIR(save_stat(os.path.join(folder, n)).st_mode) for n in names):
        return os.path.join(folder, max(names))
    return folder


def checked(chosen):
    """the folder with the save's own files in the one the player chose, and how many there are; SaveError if it can't be a save"""
    source, found = checked_files(chosen)
    return source, len(found)


def checked_files(chosen):
    source = the_save(chosen)
    found = file_stats(source)
    if not found:
        raise SaveError('There are no files in that folder. Choose the folder with the save in it: Checkpoint\'s or JKSM\'s folder for the game, '
                        'or the emulator\'s folder of it.')
    if sum(info.st_size for _, info in found) > ROOM:
        raise SaveError('That is bigger than any save of a 3DS game: choose the folder of this game\'s save alone.')
    return source, found


def pack(chosen, content):
    """the save's files into a title's content folder, as save_import: the folder they went to"""
    source, found = checked_files(chosen)
    target = os.path.join(content, 'save_import')
    os.makedirs(content, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.save-', dir=content) as work:
        staged = os.path.join(work, 'save_import')
        for name, expected in found:
            original = os.path.join(source, name)
            save_directory(os.path.dirname(original))
            info = save_stat(original)
            size = expected.st_size
            if not os.path.samestat(info, expected) or info.st_size != size:
                raise SaveError(f'The save changed while it was being copied: {original}')
            with open(original, 'rb') as f:
                if not os.path.samestat(info, os.fstat(f.fileno())):
                    raise SaveError(f'The save changed while it was being copied: {original}')
                payload = f.read(size + 1)
            if len(payload) != size:
                raise SaveError(f'The save changed while it was being copied: {original}')
            copied = os.path.join(staged, name)
            os.makedirs(os.path.dirname(copied), exist_ok=True)
            with open(copied, 'wb') as f:
                f.write(payload)
        if os.path.lexists(target):
            shutil.rmtree(target)
        os.replace(staged, target)
    return target
