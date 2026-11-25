"""A smaller C generation drops only its obsolete objects and forces the PC executable to relink."""
from pathlib import Path
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / 'win'))
import build3ds as b


class GeneratedObjects(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='3dii_objects_')).resolve()
        work = patch.object(b, 'WORK', str(self.root))
        work.start()
        self.addCleanup(work.stop)

    def tearDown(self):
        assert self.root.parent == Path(tempfile.gettempdir()).resolve() and self.root.name.startswith('3dii_objects_')
        shutil.rmtree(self.root)

    def test_obsolete_chunks_and_dependencies_only(self):
        keep = {'code_000.o', 'code_000.o.d', 'notes.o', 'code_notes.o', 'code_deadbeef.o', 'code_001.original', 'code_001.o.backup'}
        drop = {'code_001.o', 'code_001.o.d', 'code_1000.o', 'table.o', 'gs_native.o'}
        for name in keep | drop:
            (self.root / name).write_bytes(b'preserve contents unless obsolete generated object')
        self.assertTrue(b.prune_generated_objects(str(self.root), [('code_000', None)]))
        self.assertEqual({p.name for p in self.root.iterdir()}, keep)
        self.assertFalse(b.prune_generated_objects(str(self.root), [('code_000', None)]))

    def test_deletion_failure_is_not_hidden(self):
        (self.root / 'code_001.o').write_bytes(b'old')
        with patch.object(b.os, 'remove', side_effect=PermissionError('object locked')):
            with self.assertRaisesRegex(PermissionError, 'object locked'):
                b.prune_generated_objects(str(self.root), [])
        self.assertEqual((self.root / 'code_001.o').read_bytes(), b'old')

    def test_host_relinks_when_only_obsolete_object_was_removed(self):
        game = self.root / 'game';gen = game / 'gen_abi';gen.mkdir(parents=True)
        (game / 'code.bin').write_bytes(b'fixture')
        (game / 'info.json').write_text('{}')
        for name in ('code_000.c', 'table.c', 'guest.h'):
            (gen / name).write_text('')
        rt = self.root / 'runtime'
        rt.mkdir()
        runtime_source = b.ROOT + '/runtime_fixture.cpp'
        (rt / b.obj_name(runtime_source)).write_bytes(b'rt')
        removed_runtime = b.ROOT + '/removed_runtime.cpp'
        (rt / b.obj_name(removed_runtime)).write_bytes(b'obsolete runtime')
        active_runtime = [runtime_source, removed_runtime]
        (rt / 'unrelated.o').write_bytes(b'unrelated runtime object')
        obj = self.root / 'pc_fixture_gen_abi';obj.mkdir()
        for name in ('code_000.o', 'table.o', 'code_001.o', 'notes.o', 'code_deadbeef.o'):
            (obj / name).write_bytes(b'object')
        exe = obj / 'fixture.exe';exe.write_bytes(b'last executable')
        os.utime(exe, (2_000_000_000, 2_000_000_000))
        calls = []
        fail_link = [False]
        def run(args, **kw):
            calls.append(args)
            if fail_link[0] and '-o' in args:
                return SimpleNamespace(returncode=1, stdout='error: injected locked executable')
            return SimpleNamespace(returncode=0, stdout='')
        args = SimpleNamespace(gen='gen_abi', jobs=1)
        with contextlib.ExitStack() as stack:
            for name, value in [('WORK', str(self.root)), ('host_runtime', lambda _: str(rt)),
                                ('recompiler_sources', lambda: []), ('functions_list', lambda _: str(gen / 'table.c')),
                                ('runtime_sources', lambda host=False: list(active_runtime)),
                                ('compile_all', lambda *a, **kw: None), ('run', run)]:
                stack.enter_context(patch.object(b, name, value))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            b.host_build(SimpleNamespace(dir=str(game), name='fixture'), args)
            self.assertTrue(any('-o' in cmd and Path(cmd[cmd.index('-o') + 1]) == exe for cmd in calls), 'stale executable was reused')
            self.assertNotIn('code_001.o', (obj / 'link.rsp').read_text())
            self.assertNotIn('notes.o', (obj / 'link.rsp').read_text())
            self.assertNotIn('code_deadbeef.o', (obj / 'link.rsp').read_text())
            self.assertNotIn('unrelated.o', (obj / 'link.rsp').read_text())
            self.assertTrue((obj / 'notes.o').is_file() and (obj / 'code_deadbeef.o').is_file())
            calls.clear()
            b.host_build(SimpleNamespace(dir=str(game), name='fixture'), args)
            self.assertEqual(calls, [], 'unchanged build relinked unnecessarily')
            active_runtime.remove(removed_runtime)
            fail_link[0] = True
            with self.assertRaises(SystemExit):
                b.host_build(SimpleNamespace(dir=str(game), name='fixture'), args)
            self.assertIn(b.obj_name(removed_runtime), (obj/'link.rsp').read_text(), 'failed link committed new input membership')
            self.assertEqual(exe.read_bytes(), b'last executable')
            calls.clear()
            fail_link[0] = False
            b.host_build(SimpleNamespace(dir=str(game), name='fixture'), args)
            self.assertTrue(any('-o' in cmd and Path(cmd[cmd.index('-o') + 1]) == exe for cmd in calls), 'runtime membership change reused old executable')
            self.assertNotIn(b.obj_name(removed_runtime), (obj/'link.rsp').read_text())
            self.assertTrue((rt/b.obj_name(removed_runtime)).is_file(), 'unrelated objects must be retained on disk')

    def test_reparse_directory_and_ancestor_preserve_outside_files(self):
        outside = self.root / 'outside'
        outside.mkdir()
        sentinel = outside / 'code_001.o'
        sentinel.write_bytes(b'outside bytes must survive')
        work = self.root / 'work'
        work.mkdir()
        alias = work / 'pc_alias'
        if sys.platform == 'win32':
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(outside)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            alias.symlink_to(outside, target_is_directory=True)
        try:
            with patch.object(b, 'WORK', str(work)):
                for path in (alias, alias / 'nested'):
                    with self.assertRaises(ValueError):
                        b.prune_generated_objects(str(path), [])
                # Even an alias used as WORK itself cannot be traversed before writes or pruning.
                with patch.object(b, 'WORK', str(alias)):
                    with self.assertRaises(ValueError):
                        b.check_cache_dir(str(alias))
            self.assertEqual(sentinel.read_bytes(), b'outside bytes must survive')
        finally:
            if sys.platform == 'win32':
                alias.rmdir()
            else:
                alias.unlink()

    def test_shared_cache_file_cannot_be_overwritten(self):
        protected = self.root / 'protected'
        protected.mkdir()
        sentinel = protected / 'original.tik'
        sentinel.write_bytes(b'original synthetic ticket')
        objects = self.root / 'pc_fixture'
        objects.mkdir()
        os.link(sentinel, objects / 'code_000.o')
        with self.assertRaisesRegex(ValueError, 'shared'):
            b.prune_generated_objects(str(objects), [('code_000', None)])
        self.assertEqual(sentinel.read_bytes(), b'original synthetic ticket')

    def test_ppc_link_uses_current_sources_and_optional_geometry_only(self):
        gen = self.root / 'gen_abi'
        gen.mkdir()
        for name in ('code_000.c', 'table.c'):
            (gen / name).write_text('')
        code = self.root / 'ppc_fixture'
        code.mkdir()
        for name in ('code_000.o', 'table.o', 'notes.o', 'code_deadbeef.o', 'gs_native.o', 'gs_native.c'):
            (code / name).write_bytes(b'cached object')
        rt = self.root / 'runtime'
        rt.mkdir()
        runtime_source = b.ROOT + '/runtime_fixture.cpp'
        (rt / b.obj_name(runtime_source)).write_bytes(b'runtime')
        (rt / 'unrelated.o').write_bytes(b'unrelated runtime object')
        out = self.root / 'out'
        game = SimpleNamespace(name='fixture', gen_dir=str(gen), out=str(out), rpx=str(out/'fixture.rpx'), nm=str(out/'fixture.nm'))
        def run(argv, **kwargs):
            if '-o' in argv:
                target = Path(argv[argv.index('-o') + 1])
                if not target.is_absolute():
                    target = Path(kwargs.get('cwd', b.WORK)) / target
                target.write_bytes(b'linked output')
            if argv[0].endswith('elf2rpl.exe'):
                target = Path(argv[-1])
                if not target.is_absolute():
                    target = Path(kwargs.get('cwd', b.WORK)) / target
                target.write_bytes(b'rpx')
            return SimpleNamespace(returncode=0, stdout='header\nsize\n')
        with patch.object(b, 'run', side_effect=run), patch.object(b, 'runtime_sources', return_value=[runtime_source]), contextlib.redirect_stdout(io.StringIO()):
            b.link_game(game, str(rt), str(code))
            response = (self.root/'link/fixture_code.rsp').read_text()
            self.assertIn('code_000.o', response)
            self.assertIn('table.o', response)
            self.assertIn('gs_native.o', response)
            self.assertNotIn('notes.o', response)
            self.assertNotIn('code_deadbeef.o', response)
            self.assertTrue((code/'notes.o').exists())
            self.assertNotIn('unrelated.o', (self.root/'link/fixture.rsp').read_text())
            (code/'gs_native.o').unlink()
            b.link_game(game, str(rt), str(code))
            self.assertNotIn('gs_native.o', (self.root/'link/fixture_code.rsp').read_text())

    def test_runtime_source_lists_match_each_platform(self):
        runtime = self.root/'rt_source'
        port = self.root/'port'
        names = ('core.cpp', 'profile.cpp', 'main.cpp', 'frontend.cpp', 'host_sdl.cpp', 'watch.cpp',
                 'services/service.cpp', 'gpu/draw.cpp', 'audio/mixer.cpp')
        for name in names:
            path = runtime/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('')
        for name in ('wiiu/camera.cpp', 'pc/native.cpp', 'pc/win/compat.cpp'):
            path = port/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('')
        shared = {str(runtime/name).replace('\\', '/') for name in ('core.cpp', 'services/service.cpp', 'gpu/draw.cpp', 'audio/mixer.cpp')}
        with patch.object(b, 'RT', str(runtime)), patch.object(b, 'PORT', str(port)):
            self.assertEqual(set(b.runtime_sources()), shared | {str(port/'wiiu/camera.cpp').replace('\\', '/')})
            self.assertEqual(set(b.runtime_sources(host=True)), shared | {str(runtime/'profile.cpp').replace('\\', '/'),
                str(port/'pc/native.cpp').replace('\\', '/'), str(port/'pc/win/compat.cpp').replace('\\', '/')})


if __name__ == '__main__':
    unittest.main()
