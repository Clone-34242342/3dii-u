"""Synthetic first-game extraction, analysis and PC trial pictures; no game data required."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent


def run(tool, *args):
    return subprocess.run([sys.executable, str(ROOT / 'tools' / tool), *map(str, args)],
                          capture_output=True, text=True, timeout=20)


def image():
    ncch = bytearray(0x2000)
    ncch[0x100:0x104] = b'NCCH'
    struct.pack_into('<I', ncch, 0x104, len(ncch) // 0x200)
    struct.pack_into('<Q', ncch, 0x118, 0x0004000000111100)
    struct.pack_into('<III', ncch, 0x210, 0x100000, 1, 4)
    struct.pack_into('<I', ncch, 0x21c, 0x1000)
    code = struct.pack('<I', 0xe12fff1e) + bytes(0xffc)
    exefs = bytearray(0x200)
    exefs[:8] = b'.code\0\0\0'
    struct.pack_into('<II', exefs, 8, 0, len(code))
    exefs[-32:] = hashlib.sha256(code).digest()
    ncch[0xa00:0xc00] = exefs
    ncch[0xc00:0x1c00] = code
    struct.pack_into('<III', ncch, 0x1a0, 5, 9, 1)
    struct.pack_into('<III', ncch, 0x1b0, 14, 2, 1)
    level3 = struct.pack('<10I', 0x28, 0x28, 4, 0x2c, 24, 0x44, 4, 0x48, 0, 0x48) + bytes(32)
    ncch[0x1c00:0x1c04] = b'IVFC'
    struct.pack_into('<I', ncch, 0x1c08, 32)
    struct.pack_into('<Q', ncch, 0x1c44, len(level3))
    struct.pack_into('<I', ncch, 0x1c4c, 8)
    ncch[0x1d00:0x1d00 + len(level3)] = level3
    ncch[0x160:0x180] = hashlib.sha256(ncch[0x200:0x600]).digest()
    ncch[0x1c0:0x1e0] = hashlib.sha256(exefs).digest()
    ncch[0x1e0:0x200] = hashlib.sha256(ncch[0x1c00:0x1e00]).digest()
    head = bytearray(0x200)
    head[0x100:0x104] = b'NCSD'
    struct.pack_into('<II', head, 0x120, 1, len(ncch) // 0x200)
    return head + ncch, level3


def main():
    with tempfile.TemporaryDirectory(prefix='3dii_trial_') as tmp:
        work = Path(tmp)
        dump, out = work / 'test.3ds', work / 'out'
        data, level3 = image()
        dump.write_bytes(data)
        out.mkdir()
        old = out / 'romfs.bin'
        old.write_bytes(b'previous image')
        alias = work / 'previous.romfs'
        os.link(old, alias)
        result = run('extract_game.py', dump, out)
        assert result.returncode == 0, result.stderr
        assert old.read_bytes() == level3
        assert alias.read_bytes() == b'previous image', 'Replacing an extraction must preserve hardlinked originals'
        result = run('analyze_game.py', out)
        assert result.returncode == 0 and 'romfs files: 0' in result.stdout, result.stderr
        old.write_bytes(data[0x1e00:0x2200])  # Legacy whole-IVFC extraction remains readable.
        result = run('analyze_game.py', out)
        assert result.returncode == 0 and 'romfs files: 0' in result.stdout, result.stderr
        previous = old.read_bytes()
        for damage in ('hash', 'level3', 'truncated'):
            broken = bytearray(data)
            if damage == 'hash':
                broken[0x1e08] ^= 1
            elif damage == 'level3':
                struct.pack_into('<Q', broken, 0x1e44, 0x10000)
                broken[0x3e0:0x400] = hashlib.sha256(broken[0x1e00:0x2000]).digest()
            else:
                broken = broken[:-0x100]
            dump.write_bytes(broken)
            result = run('extract_game.py', dump, out)
            assert result.returncode != 0, damage
            assert old.read_bytes() == previous and not (out / 'romfs.bin.part').exists(), damage
        shots = work / 'shots'
        shots.mkdir()
        for screen, width, colour in (('top', 400, b'\xff\0\0'), ('bottom', 320, b'\0\0\xff')):
            (shots / f'{screen}1.ppm').write_bytes(f'P6\n{width} 240\n255\n'.encode() + colour * width * 240)
        sheet = work / 'sheet.png'
        result = run('trial_sheet.py', shots, sheet, 1)
        assert result.returncode == 0, result.stderr
        png = sheet.read_bytes()
        width, height = struct.unpack_from('>II', png, 16)
        assert (width, height) == (204, 244)
        offset, compressed = 8, b''
        while offset < len(png):
            length = struct.unpack_from('>I', png, offset)[0]
            if png[offset + 4:offset + 8] == b'IDAT':
                compressed += png[offset + 8:offset + 8 + length]
            offset += length + 12
        raw = zlib.decompress(compressed)
        pixel = lambda x, y: raw[y * (width * 3 + 1) + 1 + x * 3: y * (width * 3 + 1) + 4 + x * 3]
        assert pixel(2, 2) == b'\xff\0\0' and pixel(22, 122) == b'\0\0\xff'
        assert run('trial_sheet.py', shots, work / 'empty.png', 1, 2).returncode != 0
    print('trial tools: level-3/legacy reads, bounds/hash failures, hardlink preservation and screen pixels ok')


if __name__ == '__main__':
    main()
