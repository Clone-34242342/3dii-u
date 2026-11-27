"""Exercise the real converter's paths and failure preservation without keys or a game."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SOURCE = Path(os.environ.get('CIA_CONVERTER_SOURCE', HERE / 'cia_to_decrypted_3ds.py'))
spec = importlib.util.spec_from_file_location('converter_under_test', SOURCE)
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)


class Conversion(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cia-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'Street Fighter Ⅳ.cia'
        self.source.write_bytes(b'original CIA, never modified')
        self.source_hash = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.outdir = self.root / '出力'
        self.outdir.mkdir()
        self.output = self.outdir / 'Game Ⅳ.3ds'
        self.output.write_bytes(b'existing output')
        self.mode = 'ok'
        self.command = None

    def run_makerom(self, command, **kwargs):
        self.command = command
        cwd = Path(kwargs.get('cwd', Path.cwd()))
        source = Path(command[2])
        target = Path(command[4])
        # Reproduce the shipped executable's silent success with an unrepresentable argument.
        if not str(source).isascii() or not str(target).isascii():
            return subprocess.CompletedProcess(command, 0, '', 'cannot open Unicode argument')
        self.assertEqual((cwd / source).read_bytes(), self.source.read_bytes())
        self.assertFalse(source.is_absolute())
        self.assertFalse(target.is_absolute())
        if self.mode == 'exception':
            raise OSError('controlled process launch failure')
        if self.mode != 'missing':
            data = b'G' * (0x100 if self.mode == 'short' else 0x200)
            (cwd / target).write_bytes(data)
        return subprocess.CompletedProcess(command, 7 if self.mode == 'failure' else 0, '', 'diagnostic')

    def convert(self, *, decrypt=True, verify=True, source=None, output=None, copy_failure=False):
        def decrypted(path):
            return Path(path).is_file() and decrypt
        copy = patch.object(conv.shutil, 'copyfile', side_effect=OSError('copy failed')) if copy_failure else contextlib.nullcontext()
        with contextlib.redirect_stdout(io.StringIO()), copy, \
                patch.object(conv.subprocess, 'run', side_effect=self.run_makerom), \
                patch.object(conv, 'decrypt_in_place', side_effect=decrypted), \
                patch.object(conv, 'verify', return_value=verify):
            result = conv.convert_and_decrypt(str(source or self.source),
                str(output or self.output), makerom_path=str(self.root / 'makerom.exe'))
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), self.source_hash)
        self.assertEqual(list(self.outdir.glob('ciatocci-*')), [])
        return result

    def test_unicode_arguments_and_parent_directory(self):
        self.assertTrue(self.convert())
        self.assertEqual(self.output.read_bytes(), b'G' * 0x200)

    def test_ascii_input(self):
        ascii_source = self.root / 'input.cia'
        ascii_source.write_bytes(self.source.read_bytes())
        self.assertTrue(self.convert(source=ascii_source))

    def test_copy_failure_keeps_existing_output(self):
        self.assertFalse(self.convert(copy_failure=True))
        self.assertEqual(self.output.read_bytes(), b'existing output')

    def test_external_tool_failure_keeps_existing_output(self):
        self.mode = 'failure'
        self.assertFalse(self.convert())
        self.assertEqual(self.output.read_bytes(), b'existing output')

    def test_false_success_and_short_output_keep_existing_output(self):
        for mode in ('missing', 'short'):
            with self.subTest(mode=mode):
                self.mode = mode
                self.assertFalse(self.convert())
                self.assertEqual(self.output.read_bytes(), b'existing output')

    def test_decryption_and_verification_failure_keep_existing_output(self):
        for decrypt, verify in ((False, True), (True, False)):
            with self.subTest(decrypt=decrypt, verify=verify):
                self.assertFalse(self.convert(decrypt=decrypt, verify=verify))
                self.assertEqual(self.output.read_bytes(), b'existing output')

    def test_launch_failure_keeps_existing_output(self):
        self.mode = 'exception'
        self.assertFalse(self.convert())
        self.assertEqual(self.output.read_bytes(), b'existing output')

    def test_same_source_path_is_rejected(self):
        self.assertFalse(self.convert(output=self.source))
        self.assertIsNone(self.command)

    def test_hardlinked_source_is_rejected(self):
        self.output.unlink()
        os.link(self.source, self.output)
        self.assertFalse(self.convert())
        self.assertIsNone(self.command)


if __name__ == '__main__':
    unittest.main()
