"""Dependency installation with synthetic archives and mocked downloads; no network or real tools."""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

spec = importlib.util.spec_from_file_location('dependency_setup', Path(__file__).with_name('setup.py'))
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)

PPC = 'third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc'
AAC = 'toolchain/aac_ppc'
FILES = {p: b'archive binary' for p in setup.DEPS if p not in (PPC, AAC)}
FILES.update({PPC + '/' + p: b'archive library/header' for p in ('lib/libcafeglsl.a', 'include/cafeglsl/CafeGLSLCompiler.h', 'include/cafeglsl/cafe_gx2.h', 'include/cafeglsl/gfd.h')})
FILES.update({AAC + '/' + p: b'archive library/header' for p in ('lib/libavcodec.a', 'lib/libavutil.a', 'include/libavcodec/avcodec.h', 'include/libavutil/avutil.h')})
FILES[setup.NOTICE] = b'synthetic notices'


def archive(entries=()):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        for name, data in [*FILES.items(), *entries]:
            if isinstance(name, str) and '\\' in name:
                raw = zipfile.ZipInfo(name)
                raw.filename = name  # ZipInfo normally normalizes these on Windows when writing.
                name = raw
            z.writestr(name, data)
    return out.getvalue()


class DependencyInstall(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix='setup-deps-test-')).resolve()
        self.root = self.temp / 'workspace'
        self.root.mkdir()
        self.patch = mock.patch.object(setup, 'ROOT', str(self.root))
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        assert self.temp.parent == Path(tempfile.gettempdir()).resolve()
        assert self.temp.name.startswith('setup-deps-test-')
        shutil.rmtree(self.temp)

    def installed(self):
        for name in FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'newer locally installed ' + name.encode())

    def state(self):
        return {p.relative_to(self.root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns, p.stat().st_ino, p.stat().st_nlink)
                for p in self.root.rglob('*') if p.is_file()}

    def fetch(self, data=None, expected_hash=None):
        data = archive() if data is None else data
        def download(argv, **kwargs):
            assert argv[:3] == ['gh', 'release', 'download']
            (Path(argv[argv.index('--dir') + 1]) / setup.DEPS_ZIP).write_bytes(data)
            return True
        with mock.patch.object(setup.shutil, 'which', return_value='synthetic-gh'), \
                mock.patch.object(setup.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)), \
                mock.patch.object(setup, 'run', side_effect=download), \
                mock.patch.object(setup, 'DEPS_SHA256', expected_hash or hashlib.sha256(data).hexdigest()):
            return setup.fetch_deps()

    def test_missing_makerom_does_not_replace_candidate_compiler(self):
        self.installed()
        (self.root / 'builder/bin/makerom.exe').unlink()
        before = self.state()
        self.assertTrue(self.fetch())
        compiler = 'third_party/cafeglsl/glslcompiler.exe'
        self.assertEqual(self.state()[compiler], before[compiler])
        for name, state in before.items():
            self.assertEqual(self.state()[name], state, name)
        self.assertEqual((self.root / 'builder/bin/makerom.exe').read_bytes(), FILES['builder/bin/makerom.exe'])

    def test_empty_workspace_installs_declared_groups(self):
        self.assertTrue(self.fetch())
        self.assertEqual(set(self.state()), set(FILES))
        self.assertEqual(setup.deps_missing(), [])

    def test_partial_group_is_missing_and_filled_without_replacing_present_files(self):
        self.installed()
        missing = self.root / PPC / 'lib/libcafeglsl.a'
        missing.unlink()
        self.assertIn(PPC, setup.deps_missing())
        before = self.state()
        self.assertTrue(self.fetch())
        for name, state in before.items():
            self.assertEqual(self.state()[name], state)
        self.assertEqual(missing.read_bytes(), FILES[PPC + '/lib/libcafeglsl.a'])
        self.assertEqual(setup.deps_missing(), [])

    def test_empty_required_file_is_not_silently_accepted_or_replaced(self):
        self.installed()
        (self.root / PPC / 'lib/libcafeglsl.a').write_bytes(b'')
        before = self.state()
        self.assertIn(PPC, setup.deps_missing())
        self.assertFalse(self.fetch())
        self.assertEqual(self.state(), before)

    def test_unsafe_archive_members_fail_before_publication(self):
        for bad in ('../outside', '/absolute', 'C:/absolute', 'builder/bin/makerom.exe/child',
                    'builder/bin/other.exe', 'toolchain/aac_ppc/../escape', 'toolchain/aac_ppc//empty', 'toolchain/aac_ppc//',
                    'toolchain/aac_ppc/./dot', 'toolchain/aac_ppc/file:stream', 'toolchain/aac_ppc/trailing.',
                    'toolchain/aac_ppc/space ', 'toolchain\\aac_ppc\\file', 'toolchain/aac_ppc/CON'):
            with self.subTest(bad=bad):
                self.assertFalse(self.fetch(archive([(bad, b'bad')])))
                self.assertEqual(self.state(), {})
        link = zipfile.ZipInfo(AAC + '/alias')
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.assertFalse(self.fetch(archive([(link, b'../outside')])))
        self.assertEqual(self.state(), {})

    def test_duplicate_paths_and_file_parent_conflicts_fail_before_publication(self):
        for entries in ([(AAC + '/LIB/libavcodec.a', b'duplicate')],
                        [(AAC + '/extra', b'file'), (AAC + '/extra/child', b'child')]):
            with self.subTest(entries=entries):
                self.assertFalse(self.fetch(archive(entries)))
                self.assertEqual(self.state(), {})

    def test_complete_group_does_not_gain_optional_archive_files(self):
        self.installed()
        (self.root / 'builder/bin/makerom.exe').unlink()
        extra = PPC + '/optional-file'
        self.assertTrue(self.fetch(archive([(extra, b'optional')])))
        self.assertFalse((self.root / extra).exists())

    def test_corrupt_selected_payload_fails_before_publication(self):
        data = archive()
        self.assertIn(b'archive binary', data)  # uncompressed synthetic ZIP
        data = data.replace(b'archive binary', b'corrupt binary', 1)
        self.assertFalse(self.fetch(data))  # signed/hash-matched envelope still needs CRC checks
        self.assertEqual(self.state(), {})

    def test_archive_missing_required_file_fails_before_publication(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            for name, content in FILES.items():
                if name != PPC + '/lib/libcafeglsl.a':
                    z.writestr(name, content)
        self.assertFalse(self.fetch(data.getvalue()))
        self.assertEqual(self.state(), {})

    def test_hardlinked_target_is_rejected_unchanged(self):
        self.installed()
        target = self.root / 'builder/bin/makerom.exe'
        target.unlink()
        protected = self.temp / 'synthetic-ticket'
        protected.write_bytes(b'protect these synthetic bytes')
        os.link(protected, target)
        before = self.state()
        ticket = (protected.read_bytes(), protected.stat().st_mtime_ns, protected.stat().st_nlink)
        self.assertFalse(self.fetch())
        self.assertEqual(self.state(), before)
        self.assertEqual((protected.read_bytes(), protected.stat().st_mtime_ns, protected.stat().st_nlink), ticket)

    @unittest.skipUnless(os.name == 'nt', 'real Windows junction')
    def test_junction_destination_is_rejected_before_publication(self):
        self.installed()
        target = self.root / 'builder/bin'
        (target / 'makerom.exe').unlink()
        target.rmdir()
        outside = self.temp / 'protected'
        outside.mkdir()
        ticket = outside / 'makerom.exe'
        ticket.write_bytes(b'synthetic ticket')
        r = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(target), str(outside)], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        try:
            before = ticket.stat()
            self.assertFalse(self.fetch())
            self.assertEqual(ticket.read_bytes(), b'synthetic ticket')
            self.assertEqual(ticket.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(ticket.stat().st_nlink, before.st_nlink)
        finally:
            target.rmdir()  # removes the junction, never its target

    def test_hash_mismatch_does_not_mutate_workspace(self):
        self.installed()
        (self.root / 'builder/bin/makerom.exe').unlink()
        before = self.state()
        self.assertFalse(self.fetch(expected_hash='0' * 64))
        self.assertEqual(self.state(), before)


if __name__ == '__main__':
    unittest.main()
