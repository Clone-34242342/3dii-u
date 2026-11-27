"""SDK spelling can shorten, but its physical directory and missing-tool errors must stay the same."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parent / 'win/build3ds.py'


def load():
    spec = importlib.util.spec_from_file_location('sdk_path_build', SOURCE)
    module = importlib.util.module_from_spec(spec)
    # build3ds configures child tools on import; tests must not change the test runner's environment.
    with mock.patch.dict(os.environ):
        spec.loader.exec_module(module)
    return module


b = load()
LONG = 'C:/A long SDK installation directory/devkitpro'
SHORT = 'C:/ALONGS~1/DEVKIT~1'


class ShortName:
    def __init__(self, value=SHORT, query=None, result=None):
        self.value, self.query, self.result = value, query, result
        self.calls = []

    def __call__(self, path, output, capacity):
        self.calls.append((path, output, capacity))
        if output is None:
            return len(self.value) + 1 if self.query is None else self.query
        output.value = self.value[:capacity - 1]
        return len(self.value) if self.result is None else self.result


class SdkPath(unittest.TestCase):
    def shorten(self, api, same=True):
        dll = SimpleNamespace(kernel32=SimpleNamespace(GetShortPathNameW=api))
        with mock.patch.object(b.os, 'name', 'nt'), mock.patch.object(b.ctypes, 'windll', dll, create=True), \
                mock.patch.object(b.os.path, 'samefile', return_value=same):
            return b.short_sdk_root(LONG)

    def test_shorter_same_directory_selected(self):
        api = ShortName(value=SHORT.replace('/', '\\'))
        self.assertEqual(self.shorten(api), SHORT)
        self.assertEqual(len(api.calls), 2)
        self.assertIsNone(api.calls[0][1])
        self.assertEqual(api.calls[1][2], len(SHORT) + 1)

    def test_unchanged_or_longer_names_keep_original(self):
        for value in (LONG, LONG + '/longer'):
            with self.subTest(value=value):
                self.assertEqual(self.shorten(ShortName(value=value)), LONG)

    def test_different_physical_directory_keeps_original(self):
        self.assertEqual(self.shorten(ShortName(), same=False), LONG)

    def test_query_and_conversion_failures_keep_original(self):
        for api in (ShortName(query=0), ShortName(result=0), mock.Mock(side_effect=OSError('unavailable'))):
            with self.subTest(api=api):
                self.assertEqual(self.shorten(api), LONG)

    def test_truncated_or_growing_result_keeps_original(self):
        for used in (len(SHORT) + 1, len(SHORT) + 20):
            with self.subTest(used=used):
                self.assertEqual(self.shorten(ShortName(result=used)), LONG)

    def test_unavailable_api_keeps_original(self):
        with mock.patch.object(b.os, 'name', 'nt'), \
                mock.patch.object(b.ctypes, 'windll', SimpleNamespace(), create=True):
            self.assertEqual(b.short_sdk_root(LONG), LONG)

    def test_non_windows_does_not_call_api(self):
        api = mock.Mock(side_effect=AssertionError('Windows API must not be called'))
        dll = SimpleNamespace(kernel32=SimpleNamespace(GetShortPathNameW=api))
        with mock.patch.object(b.os, 'name', 'posix'), mock.patch.object(b.ctypes, 'windll', dll, create=True):
            self.assertEqual(b.short_sdk_root(LONG), LONG)
        api.assert_not_called()

    def test_root_is_shortened_before_bin_and_wut(self):
        api = ShortName()
        dll = SimpleNamespace(kernel32=SimpleNamespace(GetShortPathNameW=api))
        with mock.patch.dict(os.environ, THREEDII_DEVKITPRO=LONG), \
                mock.patch.object(b.os, 'name', 'nt'), mock.patch.object(b.ctypes, 'windll', dll, create=True), \
                mock.patch.object(b.os.path, 'samefile', return_value=True):
            module = load()
        self.assertEqual(module.DKP, SHORT)
        self.assertEqual(module.BIN, SHORT + '/devkitPPC/bin/powerpc-eabi-')
        self.assertEqual(module.WUT, SHORT + '/wut')

    @unittest.skipUnless(os.name == 'nt', 'real Windows names')
    def test_real_existing_name_still_identifies_same_files(self):
        with tempfile.TemporaryDirectory(prefix='3dii_sdk_long_name_') as temp:
            sdk = Path(temp) / 'SDK installation directory with spaces' / 'devkitpro'
            header = sdk / 'wut/include/test.h'
            header.parent.mkdir(parents=True)
            header.write_text('/* synthetic header */\n')
            original = sdk.as_posix()
            shorter = b.short_sdk_root(original)
            self.assertLessEqual(len(shorter), len(original))
            self.assertTrue(os.path.samefile(original, shorter))
            self.assertTrue(os.path.samefile(header, Path(shorter) / 'wut/include/test.h'))

    @unittest.skipUnless(os.name == 'nt', 'real Windows missing-tool error')
    def test_missing_sdk_preserves_original_path_and_tool_error(self):
        with tempfile.TemporaryDirectory(prefix='3dii_sdk_missing_') as temp:
            missing = (Path(temp) / 'missing SDK').as_posix()
            with mock.patch.dict(os.environ, THREEDII_DEVKITPRO=missing):
                module = load()
            self.assertEqual(module.DKP, missing)
            self.assertEqual(module.BIN, missing + '/devkitPPC/bin/powerpc-eabi-')
            self.assertEqual(module.WUT, missing + '/wut')
            with self.assertRaises(FileNotFoundError):
                module.run([module.BIN + 'gcc.exe', '--version'], capture_output=True)

    @unittest.skipUnless(os.name == 'nt', 'real Windows missing-tool error')
    def test_existing_sdk_folder_does_not_hide_missing_compiler(self):
        with tempfile.TemporaryDirectory(prefix='3dii_sdk_incomplete_') as temp:
            sdk = Path(temp) / 'Incomplete SDK installation directory'
            sdk.mkdir()
            with mock.patch.dict(os.environ, THREEDII_DEVKITPRO=sdk.as_posix()):
                module = load()
            self.assertTrue(os.path.samefile(module.DKP, sdk))
            with self.assertRaises(FileNotFoundError):
                module.run([module.BIN + 'gcc.exe', '--version'], capture_output=True)


if __name__ == '__main__':
    unittest.main()
