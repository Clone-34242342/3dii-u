"""Synthetic inputs for the Builder's dump, patch, save, and CRO readers."""
import ast
import hashlib
import io
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/3dsrecomp/tools'))
import dumps  # noqa: E402
import mods  # noqa: E402
import saves  # noqa: E402
from selftest import bps  # noqa: E402
from recomp import cro, gen  # noqa: E402


def program(names):
    """A plain NCCH with one byte of data per ExeFS entry and no RomFS."""
    head, ex, ef = bytearray(0x200), bytearray(0x800), bytearray(0x400)
    struct.pack_into('<III', head, 0x1A0, 5, 2, 1)
    head[0x160:0x180] = hashlib.sha256(ex[:0x400]).digest()
    for i, name in enumerate(names):
        struct.pack_into('<8sII', ef, i * 16, name, i, 1)
        ef[0x200 + i] = i
        ef[0x200 - 32 * (i + 1):0x200 - 32 * i] = hashlib.sha256(bytes([i])).digest()
    head[0x1C0:0x1E0] = hashlib.sha256(ef[:0x200]).digest()
    content = head + ex + ef
    return SimpleNamespace(program=lambda: dumps.FileSlice(io.BytesIO(content), 0, len(content)))


def romfs():
    """A root with a subdirectory and one data file."""
    header = struct.pack('<10I', 0x28, 0, 0, 0x28, 0x34, 0, 0, 0x5C, 0x24, 0x80)
    end = 0xFFFFFFFF
    dirs = struct.pack('<12I', 0, end, 0x18, 0, end, 0, 0, end, end, end, end, 2) + b'd\0\0\0'
    file = struct.pack('<IIQQII', 0, end, 0, 1, end, 2) + b'f\0\0\0'
    return bytearray(header + dirs + file + b'!')


class TestInputValidation(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)

    def test_exefs_names(self):
        cases = (([b'../guard'], 'invalid'), ([b'g\\ard'], 'invalid'), ([b'x:stream'], 'invalid'),
                 ([b'NUL'], 'invalid'), ([b'abc\0x'], 'invalid'), ([b'...'], 'invalid'),
                 ([b'icon', b'.ICON'], 'duplicate'), ([b'.code'], 'duplicate'))
        for i, (names, why) in enumerate(cases):
            with self.subTest(names=names):
                out = self.root / str(i)
                out.mkdir()
                outside = self.root / 'guard.bin'
                outside.write_bytes(b'guard')
                with patch.object(dumps, 'open', side_effect=AssertionError('unsafe ExeFS reached the writer'), create=True):
                    with self.assertRaisesRegex(dumps.DumpError, f'{why} file name'):
                        dumps.extract_program(program([b'.code', *names]), out)
                self.assertEqual(outside.read_bytes(), b'guard')
                self.assertEqual(list(out.iterdir()), [])
        good = self.root / 'good'
        dumps.extract_program(program([b'.code', b'.icon', b'logo-v2']), good)
        self.assertEqual((good / 'code.bin').read_bytes(), b'\0')
        self.assertEqual((good / 'exefs/icon.bin').read_bytes(), b'\1')
        self.assertEqual((good / 'exefs/logo-v2.bin').read_bytes(), b'\2')

    def test_update_validates_before_replacing_base(self):
        code = self.root / 'code.bin'
        code.write_bytes(b'base')
        with self.assertRaises(dumps.DumpError):
            dumps.add_update(None, program([]), self.root)
        self.assertEqual(code.read_bytes(), b'base')

    def test_save_links_and_budget(self):
        src, external, content = (self.root / n for n in ('save', 'external', 'content'))
        for directory in (src, external, content / 'save_import'):
            directory.mkdir(parents=True)
        (src / 'main').write_bytes(b'own')
        outside = external / 'other'
        outside.write_bytes(b'external')
        previous = content / 'save_import/stale'
        previous.write_bytes(b'previous')
        link = src / 'attached'
        if os.name == 'nt':
            from _winapi import CreateJunction
            CreateJunction(str(external), str(link))
            remove = link.rmdir
        else:
            link.symlink_to(external, target_is_directory=True)
            remove = link.unlink
        try:
            with self.assertRaises(saves.SaveError):
                saves.pack(src, content)
            with self.assertRaises(saves.SaveError):
                saves.checked(link)
            self.assertEqual(outside.read_bytes(), b'external')
            self.assertEqual(previous.read_bytes(), b'previous')
            self.assertFalse((content / 'save_import/attached/other').exists())
        finally:
            remove()
        (src / 'more').write_bytes(b'abc')
        with patch.object(saves, 'ROOM', 5), self.assertRaisesRegex(saves.SaveError, 'bigger'):
            saves.pack(src, content)
        self.assertEqual(previous.read_bytes(), b'previous')
        with patch.object(saves, 'ROOM', 6):
            saves.pack(src, content)
        self.assertEqual(sorted(p.name for p in previous.parent.iterdir()), ['main', 'more'])

    def test_save_copies_the_validated_files(self):
        src, content = self.root / 'save', self.root / 'content'
        src.mkdir()
        main = src / 'main'
        main.write_bytes(b'save')
        scan = saves.checked_files

        def scan_with_new_file(chosen):
            result = scan(chosen)
            (src / 'extra').write_bytes(b'other')
            return result

        with patch.object(saves, 'checked_files', side_effect=scan_with_new_file):
            packed = Path(saves.pack(src, content))
        self.assertEqual([p.name for p in packed.iterdir()], ['main'])

        def scan_with_replacement(chosen):
            result = scan(chosen)
            other = src / 'other'
            other.write_bytes(b'else')
            other.replace(main)
            return result

        with patch.object(saves, 'checked_files', side_effect=scan_with_replacement):
            with self.assertRaisesRegex(saves.SaveError, 'changed'):
                saves.pack(src, content)
        self.assertEqual((packed / 'main').read_bytes(), b'save')

    def test_compressed_module_changes_code(self):
        mod = self.root / 'romfs'
        mod.mkdir()
        (mod / 'module.CRO.P').touch()
        self.assertTrue(mods.Mod(str(self.root), 0x0004000000123400).changes_code())

    def test_romfs_links_and_bounds(self):
        good = self.root / 'valid.romfs'
        good.write_bytes(romfs())
        self.assertEqual(cro.romfs_files(good, lambda _: True), {'/f': b'!'})
        self.assertEqual(mods.read_romfs(io.BytesIO(romfs())).dirs[0].name, 'd')
        empty = self.root / 'empty.romfs'
        mods.write_romfs(mods.Dir(''), None, str(empty))
        self.assertEqual(cro.romfs_files(empty, lambda _: True), {})
        self.assertEqual(mods.read_romfs(io.BytesIO(empty.read_bytes())).files, [])
        cases = ((0x5C + 4, 0), (0x28 + 0x18 + 4, 0x18), (0x28 + 8, 0),
                 (0x28 + 12, 0x40), (0x5C + 28, 0x100), (0x5C + 16, 0xFFFFFFFF), (0x10, 0xFFFFFFFF))
        unpack = struct.unpack_from
        for offset, value in cases:
            payload = romfs()
            struct.pack_into('<I', payload, offset, value)
            path = self.root / 'invalid.romfs'
            path.write_bytes(payload)
            for read, error in ((lambda: mods.read_romfs(io.BytesIO(payload)), mods.ModError),
                                (lambda: cro.romfs_files(path, lambda _: True), ValueError)):
                calls = 0

                def bounded(*args):
                    nonlocal calls
                    calls += 1
                    self.assertLess(calls, 128, 'RomFS traversal did not terminate')
                    return unpack(*args)

                with self.subTest(offset=offset, value=value, error=error), patch.object(struct, 'unpack_from', side_effect=bounded):
                    with self.assertRaises(error):
                        read()

    def test_cro_name_literal(self):
        raw = b'mod"\\\t\n\xffAF??/\x017'
        module = bytearray(0x180)
        module[0x80:0x84] = b'CRO0'
        struct.pack_into('<I', module, 0xC0, 0x140)
        module[0x140:0x140 + len(raw)] = raw
        cros = cro.CroSet({'/m.cro': module})
        emitter = gen.Emitter(SimpleNamespace(img=SimpleNamespace(cros=cros, segments=[])), self.root)
        emitter.flag_opt = False
        emitter.run([])
        generated = (self.root / 'table.c').read_bytes()
        literal = re.search(rb'  \{("(?:[^"\\?\x00-\x1f\x7f-\xff]|\\[0-7]{3})*"), 0x[0-9a-f]+u,', generated)
        self.assertIsNotNone(literal, generated)
        self.assertEqual(ast.literal_eval((b'b' + literal[1]).decode('ascii')), raw)

    def test_code_output_limits(self):
        def guard(ctor):
            def checked(value=b''):
                if isinstance(value, int):
                    self.assertLess(value, 1 << 20, 'oversized allocation attempted')
                return ctor(value)
            return checked

        packed = struct.pack('<II', 0x08000008, 0x80000000)
        ips = b'IPS32' + struct.pack('>IH', 0x80000000, 1) + b'xEEOF'
        bps_body = b'BPS1\x81\x00\x00\x00\x00\x88\x80' + struct.pack('<II', zlib.crc32(b'a'), 0)
        oversized_bps = bps_body + struct.pack('<I', zlib.crc32(bps_body))
        for module, name, ctor, operation, error in (
            (dumps, 'bytearray', bytearray, lambda: dumps.blz_decompress(packed), dumps.DumpError),
            (mods, 'bytes', bytes, lambda: mods.apply_ips(b'a', ips), mods.ModError),
            (mods, 'bytearray', bytearray, lambda: mods.apply_bps(b'a', oversized_bps), mods.ModError),
        ):
            with self.subTest(module=module.__name__, name=name), patch.object(module, name, guard(ctor), create=True):
                with self.assertRaisesRegex(error, 'guest code region'):
                    operation()
        valid = b'prefix-' + b'\x00\xf0abc\x10' + struct.pack('<II', 0x0800000E, 7)
        self.assertEqual(dumps.blz_decompress(valid), b'prefix-' + b'abc' * 7)
        self.assertEqual(mods.apply_bps(b'a', bps(b'a', b'ab')), b'ab')
        body = bytearray(bps(b'a', b'ab')[:-4])
        body[5] = 0x81  # this says one output byte, but its action supplies two
        with self.assertRaisesRegex(mods.ModError, 'declared output size'):
            mods.apply_bps(b'a', body + struct.pack('<I', zlib.crc32(body)))

    def test_blz_zero_expansion(self):
        for stored in (b'codeDATA' + bytes(8), b'codeDATA' + bytes(4)):
            with self.subTest(stored=stored):
                self.assertEqual(dumps.blz_decompress(stored), stored)
                with patch.object(dumps, 'CODE_ROOM', len(stored) - 1):
                    with self.assertRaisesRegex(dumps.DumpError, 'guest code region'):
                        dumps.blz_decompress(stored)
        # A ten-byte footer with two padding bytes and six encoded bytes, expanding to 16 bytes.
        encoded = b'\x00\xa0abc\x10\xff\xff' + struct.pack('<II', 0x0A000010, 0)
        self.assertEqual(dumps.blz_decompress(encoded), (b'abc' * 6)[-16:])


if __name__ == '__main__':
    unittest.main()
