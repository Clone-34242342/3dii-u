"""Keep generated titles and their output folders away from existing console titles and NAND data."""
from pathlib import Path
import os
import re

# Installed releases keep these IDs; new games use the 0x3D prefix.
LEGACY_IDS = {0x10334453, 0x10334455, 0x10334467, 0x10334477, 0x10334478, 0x10334479,
              0x10334480, 0x10334483, 0x10334484, 0x10334485, 0x10334486}
NAND_DIRS = {'nand', 'slc', 'slccmpt', 'mlc', 'mlc01', 'storage_slc', 'storage_slccmpt',
             'storage_mlc01', 'storage_usb01', 'ticket', 'tickets'}


def check_title_id(title_id):
    low = title_id & 0xffffffff
    if title_id >> 32 not in (0x00050000, 0x0005000e) or (low >> 24 != 0x3d and low not in LEGACY_IDS):
        raise ValueError('Refusing a title ID outside 3Dii U: existing console tickets must not be replaced.')


def check_output(path, source=None, replace=False):
    target = Path(path).resolve()
    if any(part.casefold() in NAND_DIRS for part in target.parts):
        raise ValueError('Choose an output folder outside NAND and ticket storage.')
    if source is not None:
        original = Path(source).resolve()
        if target == original or target in original.parents or original in target.parents:
            raise ValueError('The WUP output and source title must be separate folders.')
    if replace and os.path.lexists(path):
        def reparse(entry):
            return entry.is_symlink() or bool(getattr(entry.lstat(), 'st_file_attributes', 0) & 0x400)
        original = Path(path)
        if reparse(original) or not original.is_dir():
            raise ValueError('The existing WUP output must be an ordinary folder.')
        for entry in original.iterdir():
            known = entry.name in ('title.tik', 'title.tmd', 'title.cert') or re.fullmatch(r'[0-9A-Fa-f]{8}\.(app|h3)', entry.name)
            if reparse(entry) or not entry.is_file() or entry.lstat().st_nlink != 1 or not known:
                raise ValueError('Refusing to replace a folder containing unknown files or shared file links.')
