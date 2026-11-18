"""Self-check for finding SD cards (gui.sd_cards), with kernel32 faked so no real drive is asked anything: a card reader's FAT32 disk counts
as a card, an internal, NTFS or unreadable one doesn't, every disk opened is closed, and the builder picks a card by itself only where it
can't be some other disk. python builder/test_cards.py"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import gui  # noqa: E402

USB, SATA, SD, NVME = 7, 11, 12, 17
REMOVABLE, FIXED = gui.DRIVE_REMOVABLE, gui.DRIVE_FIXED


class Kernel32:
    """drives: {letter: (drive type, file system, bus, or None for a disk that won't open)}"""
    def __init__(self, drives):
        self.drives, self.opened, self.closed = drives, {}, []

    def GetLogicalDrives(self):
        return sum(1 << ord(letter) - 65 for letter in self.drives)

    def GetDriveTypeW(self, root):
        return self.drives[root[0]][0]

    def GetVolumeInformationW(self, root, *rest):
        rest[5].value = self.drives[root[0]][1]  # lpFileSystemNameBuffer
        return 1

    def CreateFileW(self, path, *rest):
        bus = self.drives[path[4]][2]  # \\.\X:
        if bus is None:
            return ctypes.c_void_p(-1).value
        self.opened[100 + len(self.opened)] = bus
        return 99 + len(self.opened)

    def DeviceIoControl(self, disk, code, query, size, descriptor, room, got, overlapped):
        assert code == gui.IOCTL_STORAGE_QUERY_PROPERTY and size == 12 and room >= 32
        descriptor[28:32] = self.opened[disk].to_bytes(4, 'little')  # STORAGE_DEVICE_DESCRIPTOR.BusType
        got._obj.value = 36
        return 1

    def CloseHandle(self, disk):
        self.closed.append(disk)
        return 1


class Window:
    """what App.refresh_cards uses of the app"""
    def __init__(self, card=''):
        self.st, self.shown = {'card': card}, None

    def set(self, cards):
        self.shown = cards


real = gui.WIN, gui.kernel32, gui.wiiu_card
set_up = set()
gui.WIN, gui.wiiu_card = True, lambda path: path in set_up
try:
    k32 = Kernel32({'C': (FIXED, 'NTFS', NVME), 'P': (REMOVABLE, 'FAT32', USB), 'Q': (FIXED, 'FAT32', USB), 'R': (FIXED, 'FAT32', SD),
                    'S': (FIXED, 'NTFS', USB), 'T': (FIXED, 'FAT32', SATA), 'U': (FIXED, 'FAT32', None), 'V': (FIXED, 'exFAT', USB)})
    gui.kernel32 = lambda: k32
    assert gui.sd_cards() == [('P:\\', True), ('Q:\\', False), ('R:\\', False)], gui.sd_cards()
    assert sorted(k32.closed) == sorted(k32.opened), 'a disk was left open'

    # a card reader's disk set up for a Wii U comes first, and is chosen; one that isn't is listed but never chosen by itself
    set_up.add('R:\\')
    w = Window()
    gui.App.refresh_cards(w)
    assert [c['path'] for c in w.shown] == ['R:\\', 'P:\\', 'Q:\\'] and w.st['card'] == 'R:\\', (w.shown, w.st)
    assert w.shown[0]['label'].endswith('set up for a Wii U') and w.shown[2]['label'].endswith('card reader'), w.shown
    set_up.clear()
    k32.drives = {'Q': (FIXED, 'FAT32', USB)}  # a FAT32 USB disk alone: maybe a hard drive, so the player chooses it
    w = Window()
    gui.App.refresh_cards(w)
    assert [c['path'] for c in w.shown] == ['Q:\\'] and w.st['card'] == '', (w.shown, w.st)
    w = Window('Q:\\')  # chosen before, it stays chosen
    gui.App.refresh_cards(w)
    assert w.st['card'] == 'Q:\\', w.st
finally:
    gui.WIN, gui.kernel32, gui.wiiu_card = real
print('cards ok')
