"""Synthetic DLP archive preservation, not inner-CIA validation or multiplayer support."""
import hashlib
from io import BytesIO
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import dumps
from selftest import ncch
import pipeline


TITLE = 0x0004000000012300


def card(child=None):
    header, main = bytearray(0x200), bytearray(0x200)
    header[0x100:0x104] = b'NCSD'
    struct.pack_into('<II', header, 0x120, 1, 1)
    main[0x100:0x104] = b'NCCH'
    struct.pack_into('<I', main, 0x104, 1)
    struct.pack_into('<Q', main, 0x118, TITLE)
    if child is not None:
        struct.pack_into('<II', header, 0x130, 2, len(child) // 0x200)
    return header + main + (child or b'')


class DlpContentTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='3dii-dlp-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'synthetic.3ds'
        self.output = self.root / 'dlp_child.romfs'
        self.body = b'synthetic opaque RomFS envelope payload' * 20

    def copy(self, data):
        self.image.write_bytes(data)
        dump = dumps.Dump(str(self.image))
        try:
            return dumps.copy_dlp(dump, str(self.root))
        finally:
            dump.f.close()

    def test_present_and_absent(self):
        self.assertFalse(self.copy(card()))
        self.assertFalse(self.output.exists())
        self.assertTrue(self.copy(card(ncch(self.body))))
        self.assertEqual(self.output.read_bytes(), self.body)

    def test_cia_uses_content_index_not_row(self):
        child = ncch(self.body)
        self.image.write_bytes(bytes(0x200) + child)
        with self.image.open('rb') as f:
            dump = dumps.Dump.__new__(dumps.Dump)
            dump.f, dump.path = f, str(self.image)
            dump.cia = mock.Mock(contents=[(7, 2, 0, len(child), 0x200), (8, 1, 0, 0, None)])
            self.assertTrue(dumps.copy_dlp(dump, str(self.root)))
        self.assertEqual(self.output.read_bytes(), self.body)

    def test_encrypted_secondary_content_uses_existing_decryptor(self):
        encrypted = bytearray(ncch(self.body))
        encrypted[0x100:0x104] = b'xxxx'

        def decrypt(path, index, output):
            self.assertEqual(index, 2)
            self.assertEqual(path, str(self.image))
            Path(output).write_bytes(ncch(self.body))

        with mock.patch.object(dumps.decrypt, '_full_decrypted', side_effect=decrypt):
            self.assertTrue(self.copy(card(encrypted)))
        self.assertEqual(self.output.read_bytes(), self.body)
        self.assertFalse(list(self.root.glob('.dlp-*')))

    def test_bad_envelopes_preserve_last_output(self):
        good = card(ncch(self.body))
        bad_partition = bytearray(good)
        struct.pack_into('<I', bad_partition, 0x130, 0x10000000)
        bad_section = bytearray(good)
        struct.pack_into('<I', bad_section, 0x400 + 0x1B4, 0x10000000)
        bad_hash = bytearray(good)
        bad_hash[0x400 + 0x1E0] ^= 1
        bad_level3 = bytearray(good)
        struct.pack_into('<Q', bad_level3, 0x600 + 0x44, 1 << 40)
        bad_level3[0x400 + 0x1E0:0x600] = hashlib.sha256(bad_level3[0x600:0x800]).digest()
        for data in (good[:-1], bad_partition, bad_section, bad_hash, bad_level3):
            with self.subTest(size=len(data)):
                self.output.write_bytes(b'last good archive')
                with self.assertRaises(dumps.DumpError):
                    self.copy(data)
                self.assertEqual(self.output.read_bytes(), b'last good archive')
                self.assertFalse(Path(str(self.output) + '.part').exists())
                self.assertFalse(list(self.root.glob('.dlp-*')))

    def test_short_read_during_copy_preserves_output(self):
        original = dumps.FileSlice.pread

        def shortened(src, pos, length):
            data = original(src, pos, length)
            return data[:-1] if pos == 0x1200 else data

        self.output.write_bytes(b'last good archive')
        with mock.patch.object(dumps.FileSlice, 'pread', shortened), self.assertRaises(dumps.DumpError):
            self.copy(card(ncch(self.body)))
        self.assertEqual(self.output.read_bytes(), b'last good archive')
        self.assertFalse(Path(str(self.output) + '.part').exists())

    def test_title_includes_only_preserved_archive(self):
        self.copy(card(ncch(self.body)))
        out = self.root / 'build'
        (out / 'title/content').mkdir(parents=True)
        info = dict(slug='test', title_id=TITLE, name='Synthetic', product='TEST')
        with mock.patch.object(pipeline, 'tool', return_value=mock.Mock()):
            pipeline.make_title(str(self.root), str(out), info)
        self.assertEqual((out / 'title/content/dlp_child.romfs').read_bytes(), self.body)

    def test_child_staging_requires_matching_cia_and_program_ids(self):
        child_id = 0x0004000100012302
        archive = self.root / 'outer.romfs'
        tree = pipeline.mods.Dir('')
        tree.files.append([f'{child_id:016x}.cia', (0, len(self.body))])
        pipeline.mods.write_romfs(tree, BytesIO(self.body), str(archive))
        self.image.write_bytes(card(ncch(archive.read_bytes())))
        parent = dumps.Dump(str(self.image))
        self.addCleanup(parent.f.close)

        for cia_id, ncch_id, program_id in ((child_id, child_id, child_id), (TITLE, child_id, child_id),
                                           (child_id, TITLE, child_id), (child_id, child_id, TITLE)):
            with self.subTest(cia_id=cia_id, ncch_id=ncch_id, program_id=program_id):
                def open_child(path):
                    self.assertEqual(Path(path).read_bytes(), self.body)
                    program = mock.Mock()
                    program.pread.return_value = ncch_id.to_bytes(8, 'little')
                    return SimpleNamespace(f=open(path, 'rb'), cia=True, title_id=cia_id, encrypted=lambda: False,
                                           program=lambda: program)

                with mock.patch.object(dumps, 'Dump', side_effect=open_child), \
                     mock.patch.object(dumps, 'extract_program', return_value={'program_id': f'{program_id:016x}'}) as extract:
                    if cia_id == ncch_id == program_id == child_id:
                        pipeline.extract_dlp_child(parent, child_id, str(self.root), str(self.root), None)
                        extract.assert_called_once()
                    else:
                        with self.assertRaisesRegex(dumps.DumpError, 'wrong (title|program) ID'):
                            pipeline.extract_dlp_child(parent, child_id, str(self.root), str(self.root), None)
                        self.assertEqual(extract.call_count, int(cia_id == ncch_id == child_id))


if __name__ == '__main__':
    unittest.main()
