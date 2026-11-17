"""Failure injection for replacing a built WUP or its SD-card copy; all files are synthetic."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from contextlib import ExitStack
from unittest import mock

import pipeline


class OutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='3diiu-output-test-')
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / 'source' / 'Game'
        self.target = self.root / 'output' / 'Game'
        for folder, data in ((self.source, b'new'), (self.target, b'old')):
            folder.mkdir(parents=True)
            (folder / 'title.tmd').write_bytes(data)

    def tearDown(self):
        assert self.source.resolve().is_relative_to(self.root) and self.target.resolve().is_relative_to(self.root)
        self.temp.cleanup()

    def old_intact(self):
        self.assertTrue(self.target.is_dir(), 'the last good output was removed')
        self.assertEqual((self.target / 'title.tmd').read_bytes(), b'old')

    def test_copy_rename_failure(self):
        rename = os.rename

        def refuse_new(src, dst):
            if Path(dst) == self.target and Path(src).name != 'previous':
                raise PermissionError('injected destination lock')
            return rename(src, dst)

        with mock.patch.object(pipeline.os, 'rename', side_effect=refuse_new):
            with self.assertRaises(PermissionError):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        self.old_intact()

    def test_failed_pack(self):
        def broken(argv, **kwargs):
            out = Path(argv[-1])
            out.mkdir(exist_ok=True)
            (out / 'title.tmd').write_bytes(b'incomplete')
            return subprocess.CompletedProcess(argv, 1, 'injected pack failure')

        with mock.patch.object(pipeline.subprocess, 'run', side_effect=broken):
            with self.assertRaises(SystemExit):
                pipeline.pack_wup('unused', 'unused', str(self.target))
        self.old_intact()

    def test_cancelled_pack(self):
        with mock.patch.object(pipeline.subprocess, 'run', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                pipeline.pack_wup('unused', 'unused', str(self.target))
        self.old_intact()

    def test_copy_success(self):
        pipeline.copy_to(str(self.source), str(self.target.parent))
        self.assertEqual((self.target / 'title.tmd').read_bytes(), b'new')
        self.assertEqual(list(self.target.parent.iterdir()), [self.target])

    def test_copy_verification_failure(self):
        with mock.patch.object(pipeline.filecmp, 'cmpfiles', return_value=([], ['title.tmd'], [])):
            with self.assertRaises(SystemExit):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        self.old_intact()
        self.assertEqual(list(self.target.parent.iterdir()), [self.target])

    def test_copy_failed_rollback_keeps_backup(self):
        rename = os.rename

        def refuse_destination(src, dst):
            if Path(dst) == self.target:
                raise PermissionError('injected destination lock including rollback')
            return rename(src, dst)

        with mock.patch.object(pipeline.os, 'rename', side_effect=refuse_destination):
            with self.assertRaisesRegex(OSError, 'safe at'):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        backups = list(self.target.parent.glob('.3diiu-*/previous/title.tmd'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b'old')

    def test_hard_stop_during_swap_keeps_backup(self):
        script = '''
import os, sys
import pipeline
rename = os.rename
def stop_after_backup(src, dst):
    rename(src, dst)
    if os.path.basename(dst) == 'previous':
        os._exit(23)
pipeline.os.rename = stop_after_backup
pipeline.copy_to(sys.argv[1], sys.argv[2])
'''
        r = subprocess.run([pipeline.sys.executable, '-c', script, str(self.source), str(self.target.parent)],
                           cwd=Path(__file__).parent, capture_output=True, text=True)
        self.assertEqual(r.returncode, 23, r.stderr)
        backups = list(self.target.parent.glob('.3diiu-*/previous/title.tmd'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b'old')
        self.assertIn(str(backups[0].parent), r.stdout)

    def test_pack_success(self):
        def packed(argv, **kwargs):
            out = Path(argv[-1])
            out.mkdir()
            (out / 'title.tmd').write_bytes(b'new')
            return subprocess.CompletedProcess(argv, 0, 'packed')

        with mock.patch.object(pipeline.subprocess, 'run', side_effect=packed):
            pipeline.pack_wup('unused', 'unused', str(self.target))
        self.assertEqual((self.target / 'title.tmd').read_bytes(), b'new')
        self.assertEqual(list(self.target.parent.iterdir()), [self.target])

    def test_failed_retry_preserves_wup_left_by_card_failure(self):
        engine, build = self.root / 'engine', self.root / 'build'
        fake_builder = engine / 'tools' / 'win' / 'build3ds.py'
        fake_builder.parent.mkdir(parents=True)
        (fake_builder.parent / 'gcc.exe').touch()
        fake_builder.write_text('''
import os, threading
from types import SimpleNamespace
BIN = os.path.dirname(__file__) + os.sep
MEASURE = MEASURE_CONSOLE = False
CRO_BASE = '08000000'
context = threading.local()
def Game(name, directory):
    return SimpleNamespace(dir=directory, mode='abi', extra_functions=directory + '/functions.txt')
def prepare_vertex_logs(*args): pass
def build_shadergen(*args): return 'unused'
def build_code(*args): pass
def build_shaders(*args): pass
''')

        class Game:
            path, kind, size = 'made-up.3ds', 'game', 0
            title_id, unique = 0x0004000000086300, 0x00086300
            product, name = 'CTR-P-TEST', 'Retry test'
            encrypted = lambda self: False
            dlp_source = lambda self: None
            read_names = lambda self: None
            f = mock.Mock()

        class ExtractionFailed(Exception):
            pass

        def extract(game, folder, progress):
            (Path(folder) / 'code.bin').write_bytes(b'synthetic code')

        def run(argv, **kwargs):
            if Path(argv[1]).name == 'pack_game_wup.py':
                out = Path(argv[-1])
                out.mkdir()
                (out / 'title.tmd').write_bytes(b'completed WUP')
            return subprocess.CompletedProcess(argv, 0, '')

        with ExitStack() as stack:
            for owner, name, value in ((pipeline, 'ROOT', str(engine)), (pipeline, 'BUILD', str(build)),
                                      (pipeline, 'WORK_GAMES', str(build / 'builder_work')), (pipeline, 'ARCHIVES', str(build / 'archives')),
                                      (pipeline.sys, 'platform', 'win32'),
                                      (pipeline.sys, 'argv', ['pipeline.py', '--game', 'made-up.3ds', '--otp', 'unused', '--card', str(self.root / 'card')])):
                stack.enter_context(mock.patch.object(owner, name, value))
            for owner, name, value in ((pipeline, 'profile_for', None), (pipeline.user_files, 'check_otp', None),
                                      (pipeline.dumps, 'Dump', Game()), (pipeline.dumps, 'have_system_archives', True),
                                      (pipeline.dumps, 'archives_named', []), (pipeline.shaderlogs, 'restore', (0, 0)),
                                      (pipeline.shutil, 'disk_usage', mock.Mock(free=1 << 40))):
                stack.enter_context(mock.patch.object(owner, name, return_value=value))
            stack.enter_context(mock.patch.object(pipeline, 'make_title'))
            stack.enter_context(mock.patch.object(pipeline.mods, 'apply'))
            extraction = stack.enter_context(mock.patch.object(pipeline.dumps, 'extract_program', side_effect=extract))
            stack.enter_context(mock.patch.object(pipeline.subprocess, 'run', side_effect=run))
            stack.enter_context(mock.patch.object(pipeline, 'copy_to', side_effect=OSError('injected card failure')))
            with self.assertRaises(SystemExit):
                pipeline.main()
            completed = list(build.rglob('title.tmd'))
            self.assertEqual(len(completed), 1)
            extraction.side_effect = ExtractionFailed
            with self.assertRaises(ExtractionFailed):
                pipeline.main()
            self.assertTrue(completed[0].is_file(), 'retry discarded the completed WUP before producing its replacement')
            self.assertEqual(completed[0].read_bytes(), b'completed WUP')


if __name__ == '__main__':
    unittest.main()
