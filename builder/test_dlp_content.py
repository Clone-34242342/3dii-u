"""Synthetic DLP archive preservation, not inner-CIA validation or multiplayer support."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import dumps
import pipeline
from selftest import ncch


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


if __name__ == '__main__':
    unittest.main()
