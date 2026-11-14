"""Self-check for saves.py in a folder standing in for an SD card: which folder of a pick holds the save (the newest of Checkpoint's backups, an
emulator's data/00000001), the files left out, the import the game takes in, a copy that comes out short, the request for the game's save and
the copy back.
Run: python builder/test_saves.py"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import saves  # noqa: E402


def put(path, data=b'x'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def read(path):
    with open(path, 'rb') as f:
        return f.read()


with tempfile.TemporaryDirectory() as tmp:
    card, tid = os.path.join(tmp, 'card'), '0005000010086300'
    game = saves.card_folder(card, tid.upper())
    assert game == os.path.join(card, '3dii-u', tid)

    # Checkpoint's folder for a game: its newest dated backup is taken, files and folders as they are, and what Windows leaves there isn't
    backups = os.path.join(tmp, 'Checkpoint', '0x86300 Animal Crossing')
    put(os.path.join(backups, '20261114-050000', 'garden_plus.dat'), b'new')
    put(os.path.join(backups, '20261114-050000', 'sub', 'more.bin'), b'deep')
    put(os.path.join(backups, '20261114-050000', 'Thumbs.db'), b'junk')
    put(os.path.join(backups, '20250101-120000', 'garden_plus.dat'), b'old')  # written later, dated earlier
    put(os.path.join(backups, 'desktop.ini'), b'junk')
    taken, n = saves.bring_in(card, tid, backups)
    assert taken == os.path.join(backups, '20261114-050000') and n == 2
    assert read(os.path.join(game, 'import', 'garden_plus.dat')) == b'new' and read(os.path.join(game, 'import', 'sub', 'more.bin')) == b'deep'
    assert not os.path.exists(os.path.join(game, 'import', 'Thumbs.db')) and sorted(os.listdir(game)) == ['import']

    # a save of nothing but folders, or a folder of the player's own backups, is taken as it is: no guessing which
    folders = os.path.join(tmp, 'folders')
    put(os.path.join(folders, 'replay', 'a.bin'), b'r')
    put(os.path.join(folders, 'my backup', 'b.bin'), b'm')
    assert saves.the_save(folders) == folders and saves.bring_in(card, tid, folders)[1] == 2

    # an emulator's folder of the game: the save is in data/00000001, and a second import replaces the first whole
    emulator = os.path.join(tmp, 'sdmc', 'title', '00040000', '00086300')
    put(os.path.join(emulator, 'data', '00000001', 'garden_plus.dat'), b'emu')
    assert saves.bring_in(card, tid, emulator)[1] == 1
    assert sorted(os.listdir(os.path.join(game, 'import'))) == ['garden_plus.dat'] and read(os.path.join(game, 'import', 'garden_plus.dat')) == b'emu'
    assert sorted(os.listdir(game)) == ['import']

    # the save's own files with a folder among them, taken as they are; an empty folder is turned down
    plain = os.path.join(tmp, 'plain')
    put(os.path.join(plain, 'main'), b'p')
    put(os.path.join(plain, 'sub', 'x'), b'q')
    assert saves.the_save(plain) == plain and saves.bring_in(card, tid, plain)[1] == 2
    os.makedirs(os.path.join(tmp, 'empty'))
    try:
        saves.bring_in(card, tid, os.path.join(tmp, 'empty'))
        raise AssertionError('an empty folder was taken')
    except saves.SaveError:
        pass

    # a copy that comes out short replaces nothing
    real = shutil.copytree
    def short(src, dst, *a, **k):  # copytree calls itself for each folder in it
        real(src, dst, *a, **k)
        if os.path.exists(os.path.join(dst, 'main')):
            os.remove(os.path.join(dst, 'main'))
    shutil.copytree = short
    try:
        saves.bring_in(card, tid, plain)
        raise AssertionError('a short copy was taken')
    except saves.SaveError:
        pass
    finally:
        shutil.copytree = real
    assert read(os.path.join(game, 'import', 'main')) == b'p' and sorted(os.listdir(game)) == ['import']

    # the other way: asked for, left by the game, copied into a new dated folder, and the card's copy gone
    assert not saves.exported(card, tid)
    saves.ask_for_it(card, tid)
    assert os.path.isdir(os.path.join(game, 'export')) and not saves.exported(card, tid)
    put(os.path.join(game, 'export', 'garden_plus.dat'), b'wiiu')
    assert saves.exported(card, tid)
    pc = os.path.join(tmp, 'pc')
    put(os.path.join(pc, 'keep.txt'), b'mine')
    where = saves.take_out(card, tid, pc)
    assert os.path.dirname(where) == pc and len(os.path.basename(where)) == 15 and read(os.path.join(where, 'garden_plus.dat')) == b'wiiu'
    assert read(os.path.join(pc, 'keep.txt')) == b'mine' and sorted(os.listdir(game)) == ['import']
print('saves: ok')
