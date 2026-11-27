"""Builder failure injection with synthetic inputs and real lock contention in a temporary workspace."""
from contextlib import ExitStack, contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from types import MethodType, ModuleType, SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT / d) for d in ('builder', 'tools', 'tools/win')]
import app
import build3ds
import check_ports as ports
import dvlb_to_log
import make_app
import pipeline
import ship_card
import updater
from test_pipeline_outputs import fake_tmd


def wup(folder, marker, title_id=0x000500003d000863):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'title.tmd').write_bytes(fake_tmd(marker, title_id))
    (folder / '00000000.app').write_bytes(marker)


def run_args(root):
    return ['pipeline.py', '--game', 'synthetic.3ds', '--otp', 'unused', '--out', str(root / 'output'), '--keep']


@contextmanager
def build_environment(root, argv):
    engine, build = root / 'engine', root / 'build'
    with mock.patch.multiple(pipeline, ROOT=str(engine), WORK=str(build), BUILD=str(build),
                             WORK_GAMES=str(build / 'builder_work'), ARCHIVES=str(build / 'archives')), \
         mock.patch.object(sys, 'argv', argv), mock.patch.object(sys, 'platform', 'win32'), \
         mock.patch.object(pipeline.user_files, 'check_otp', return_value=None):
        yield engine, build


def lock_process(root, holding, separate_build=False):
    with build_environment(root, run_args(root)), mock.patch.object(pipeline.dumps, 'Dump') as read:
        def reached(_):
            if holding:
                (root / 'ready').touch()
                threading.Event().wait(20)
            raise SystemExit(23)
        read.side_effect = reached
        with mock.patch.object(pipeline, 'BUILD', str(root / 'large-build') if separate_build else pipeline.BUILD):
            pipeline.main()


def wait_for(path, process):
    for _ in range(100):
        if path.exists() or process.poll() is not None:
            break
        time.sleep(.05)


def publish_process(root, first):
    real_check = pipeline.wup_safety.check_title_replacement
    title_id = 0x000500003d000863 if first else 0x000500003d000864
    end = 'Same_Name' if first or os.name != 'nt' else 'SAME_NAME'
    target = root / 'fake-card' / 'install' / end

    def pause_after_validation(path, check_id):
        real_check(path, check_id)
        if first:
            (root / 'validated').touch()
            for _ in range(200):
                if (root / 'resume').exists():
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('publisher test timed out')

    try:
        with build3ds.objcache.workspace_lock(root / f'workspace-{title_id:x}'), \
             mock.patch.object(pipeline.wup_safety, 'check_title_replacement', side_effect=pause_after_validation), \
             pipeline.staged_output(str(target)) as new:
            wup(Path(new), b'first pack' if first else b'second pack', title_id)
    except (OSError, ValueError) as e:
        print(e)
        raise SystemExit(1)


def ship_process(root, pause_at):
    source = root / 'source' / 'Game'
    card = root / 'fake-card'
    target = card / 'install' / 'Game'
    isdir, remove = os.path.isdir, shutil.rmtree

    def pause():
        (root / 'ship-paused').touch()
        for _ in range(200):
            if (root / 'ship-resume').exists():
                return
            time.sleep(.05)
        raise RuntimeError('ship test timed out')

    def check_directory(path):
        result = isdir(path)
        if pause_at == 'absence' and Path(path) == target and not result:
            pause()
        return result

    def remove_directory(path, *args, **kwargs):
        if pause_at == 'cleanup' and Path(path) == source:
            pause()
        return remove(path, *args, **kwargs)

    with mock.patch.object(ship_card, 'PREVIOUS', str(root / 'previous')), \
         mock.patch.object(ship_card.os.path, 'isdir', side_effect=check_directory), \
         mock.patch.object(ship_card.shutil, 'rmtree', side_effect=remove_directory):
        result = ship_card.copy(str(source), str(card))
    print(result)
    if 'on the card' not in result:
        raise SystemExit(1)


def publish_same_title(target):
    try:
        with pipeline.staged_output(str(target)) as new:
            wup(Path(new), b'competing pack')
    except (OSError, ValueError) as e:
        print(e)
        raise SystemExit(1)


class Dump(SimpleNamespace):
    __hash__ = object.__hash__


@contextmanager
def fake_build(root, product='CTR-P-TEST', name='Synthetic Game', args=(), code='pass', shaders='pass', measure=False):
    with build_environment(root, run_args(root) + list(args)) as (engine, build), ExitStack() as stack:
        script = engine / 'tools/win/build3ds.py'
        script.parent.mkdir(parents=True)
        script.with_name('gcc.exe').touch()
        script.write_text(f'''
import os, threading
from types import SimpleNamespace
BIN = os.path.dirname(__file__) + os.sep
MEASURE = {measure!r}
MEASURE_CONSOLE = False
CRO_BASE = '08000000'
context = threading.local()
def Game(name, directory):
    return SimpleNamespace(dir=directory, mode='abi', extra_functions=directory + '/functions.txt', upstream_functions=None)
def prepare_vertex_logs(*args): pass
def build_shadergen(*args): return 'unused'
def build_code(*args): {code}
def build_shaders(*args): {shaders}
''')
        game = Dump(path='synthetic.3ds', kind='game', size=0, title_id=0x0004000000086300, unique=0x00086300,
                    product=product, name=name, encrypted=lambda: False, read_names=lambda: None, f=mock.Mock())
        for owner, attr, result in (
                (pipeline, 'profile_for', None), (pipeline.dumps, 'Dump', game),
                (pipeline.dumps, 'have_system_archives', True), (pipeline.dumps, 'archives_named', []),
                (pipeline.dumps, 'copy_dlp', None), (pipeline.mods, 'apply', None), (pipeline, 'make_title', None),
                (pipeline.shaderlogs, 'restore', (0, 0)), (pipeline.shutil, 'disk_usage', mock.Mock(free=1 << 40)),
                (pipeline.subprocess, 'run', subprocess.CompletedProcess([], 0, ''))):
            stack.enter_context(mock.patch.object(owner, attr, return_value=result))
        stack.enter_context(mock.patch.object(pipeline.dumps, 'extract_program',
                            side_effect=lambda dump, folder, progress: (Path(folder) / 'code.bin').touch()))
        stack.enter_context(mock.patch.object(pipeline, 'pack_wup',
                            side_effect=lambda otp, title, dest: wup(Path(dest), b'completed')))
        yield engine, build


class BuilderLifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='3diiu-lifecycle-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.local_data = self.root / 'local-appdata'
        self.enterContext(mock.patch.dict(os.environ, LOCALAPPDATA=str(self.local_data)))

    def race_ship_and_publish(self, pause_at, competing_target):
        shipper = subprocess.Popen([sys.executable, __file__, '--ship', str(self.root), pause_at],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            wait_for(self.root / 'ship-paused', shipper)
            self.assertTrue((self.root / 'ship-paused').exists())
            contender = [sys.executable, __file__, '--publish-same-title', str(competing_target)]
            return subprocess.run(contender, capture_output=True, text=True, timeout=10)
        finally:
            (self.root / 'ship-resume').touch()
            output, _ = shipper.communicate(timeout=15)
            self.assertEqual(shipper.returncode, 0, output)

    def test_product_component_cannot_remove_workspace_parent(self):
        with fake_build(self.root, product='CTR-P-/../', name='') as (_, build):
            work = build / 'builder_work'
            (work / 'g_').mkdir(parents=True)
            sentinel = work / 'untouched.txt'
            sentinel.write_bytes(b'synthetic sentinel')
            remove = shutil.rmtree

            def remove_resolved(folder, **kwargs):
                resolved = Path(folder).resolve()
                self.assertTrue(resolved.is_relative_to(self.root), resolved)
                return remove(resolved, **kwargs)

            try:
                with mock.patch.object(pipeline.shutil, 'rmtree', side_effect=remove_resolved):
                    pipeline.main()
            finally:
                self.assertTrue(sentinel.is_file(), 'malformed product made extraction remove the build parent')
            self.assertEqual(sentinel.read_bytes(), b'synthetic sentinel')
            self.assertTrue((work / 'g_0004000000086300' / 'code.bin').is_file())
            folder, = (self.root / 'output').iterdir()
            self.assertEqual(folder.name, 'Game_0004000000086300')

    def test_workspace_lock_precedes_refresh_and_releases_on_exit(self):
        def child(mode):
            return [sys.executable, __file__, mode, str(self.root)]
        holder = subprocess.Popen(child('--hold'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(self.root / 'ready', holder)
            self.assertTrue((self.root / 'ready').exists())
            engine, build = self.root / 'engine', self.root / 'build'
            engine.mkdir(exist_ok=True)
            (engine / 'stamp.txt').write_text('new')
            cache = build / 'build_win_builder'
            cache.mkdir(parents=True)
            sentinel = cache / 'existing.o'
            sentinel.touch()
            denied = subprocess.run(child('--probe'), capture_output=True, text=True, timeout=15)
            self.assertEqual(denied.returncode, 1, denied.stdout + denied.stderr)
            self.assertIn('Another build is using', denied.stdout)
            self.assertTrue(sentinel.is_file())
            script = self.root / 'script.py'
            script.write_text('print("frozen script dispatched")')
            embedded = subprocess.run(child('--embedded-child'), capture_output=True, text=True, timeout=10,
                                      env={**os.environ, 'THREEDII_ROOT': str(engine), 'THREEDII_WORK': str(build), 'THREEDII_BUILD': str(build)})
            self.assertEqual(embedded.returncode, 0, embedded.stdout + embedded.stderr)
            self.assertIn('frozen script dispatched', embedded.stdout)
            self.assertTrue(sentinel.is_file(), 'a frozen helper refreshed the parent build cache')
        finally:
            holder.kill()
            holder.wait(timeout=10)
        next_build = subprocess.run(child('--probe'), capture_output=True, text=True, timeout=15)
        self.assertEqual(next_build.returncode, 23, next_build.stdout + next_build.stderr)
        self.assertFalse(sentinel.exists(), 'the next process did not refresh its stale cache')

    def test_workspace_lock_rejects_hardlink(self):
        inert = self.root / 'sentinel'
        inert.write_bytes(b'unchanged')
        os.link(inert, self.root / '.3diiu-build.lock')
        with self.assertRaisesRegex(ValueError, 'shared hard links'):
            with build3ds.objcache.workspace_lock(self.root):
                self.fail('opened a lock through a shared hard link')
        self.assertEqual(inert.read_bytes(), b'unchanged')

    def test_gui_archive_import_uses_work_lock_with_separate_build_folder(self):
        import gui

        holder = subprocess.Popen([sys.executable, __file__, '--hold-other-build', str(self.root)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        archive = self.root / 'build' / 'archives' / 'synthetic.romfs'
        work = str(self.root / 'build')
        window = SimpleNamespace(st={'keys': '', 'system': ''}, system_locked=False, tell=mock.Mock(), refresh_files=mock.Mock())
        window.import_system = MethodType(gui.App.import_system, window)
        if hasattr(gui.App, 'import_system_locked'):
            window.import_system_locked = MethodType(gui.App.import_system_locked, window)

        def import_archive(source, directory, temporary):
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(b'fake archive')
            return ['synthetic']

        try:
            wait_for(self.root / 'ready', holder)
            self.assertTrue((self.root / 'ready').exists())
            with mock.patch.multiple(pipeline, WORK=work, BUILD=str(self.root / 'large-build'), ARCHIVES=str(archive.parent)), \
                 mock.patch.object(gui.QFileDialog, 'getExistingDirectory', return_value=str(self.root / 'archive-source')), \
                 mock.patch.object(gui.decrypt, 'have_keys', return_value=False) as read_keys, \
                 mock.patch.object(gui, 'save_settings'), \
                 mock.patch.object(gui.dumps, 'import_system_archives', side_effect=import_archive) as imported:
                gui.App.chooseSystem(window)
                self.assertFalse(archive.exists(), 'a second window overwrote the shared archive cache during the build')
                read_keys.assert_not_called()
                imported.assert_not_called()
                self.assertIn('Another build is using', window.tell.call_args.args[1])
                holder.kill()
                holder.wait(timeout=10)
                gui.App.chooseSystem(window)
                imported.assert_called_once()
                self.assertEqual(archive.read_bytes(), b'fake archive')
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.wait(timeout=10)

    def test_wup_publication_serializes_independent_workspaces(self):
        target = self.root / 'fake-card' / 'install' / 'Same_Name'
        def child(which):
            return [sys.executable, __file__, '--publish', str(self.root), which]

        first = subprocess.Popen(child('first'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            wait_for(self.root / 'validated', first)
            self.assertTrue((self.root / 'validated').exists())
            second = subprocess.run(child('second'), capture_output=True, text=True, timeout=10)
            self.assertFalse(target.exists(), 'an independent publisher installed its different title after the first publisher validated')
            self.assertEqual(second.returncode, 1, second.stdout + second.stderr)
            self.assertIn('Another build is using', second.stdout)
        finally:
            (self.root / 'resume').touch()
            out, _ = first.communicate(timeout=15)
            self.assertEqual(first.returncode, 0, out)
        self.assertEqual((target / '00000000.app').read_bytes(), b'first pack')
        retry = subprocess.run(child('second'), capture_output=True, text=True, timeout=10)
        self.assertEqual(retry.returncode, 1, retry.stdout + retry.stderr)
        self.assertIn('A different Wii U title', retry.stdout)
        self.assertEqual((target / '00000000.app').read_bytes(), b'first pack')

    def test_app_junctions_keep_engine_identity(self):
        w = self.root / 'work'
        a, b = self.root / 'v1' / 'engine', self.root / 'v2' / 'engine'
        a.mkdir(parents=True)
        b.mkdir(parents=True)
        with mock.patch.dict(os.environ):
            os.environ.pop('THREEDII_ROOT', None)
            one, two = app.short_path(str(a), str(w)), app.short_path(str(b), str(w))
        self.assertTrue(os.path.samefile(one, a))
        self.assertTrue(os.path.samefile(two, b))
        self.assertNotEqual(one, two)

    def test_ship_copy_and_backup_restore_after_card_swap_failure(self):
        source, card = self.root / 'completed' / 'Game', self.root / 'fake-card'
        target, prior = card / 'install' / 'Game', self.root / 'previous' / 'Game'
        for folder, data in ((source, b'new'), (target, b'card old'), (prior, b'PC older')):
            wup(folder, data)
        rename = os.rename

        def swap(src, dst):
            if Path(dst) == target and Path(src).name == 'new':
                raise PermissionError('injected card publication failure')
            return rename(src, dst)

        with mock.patch.object(ship_card, 'PREVIOUS', str(prior.parent)):
            with mock.patch.object(ship_card.filecmp, 'cmpfiles', return_value=([], ['00000000.app'], [])):
                failure = ship_card.copy(str(source), str(card))
            self.assertEqual((target / '00000000.app').read_bytes(), b'card old')
            self.assertEqual((prior / '00000000.app').read_bytes(), b'PC older')
            self.assertIn('FAILED', failure)

            with mock.patch.object(os, 'rename', side_effect=swap):
                failure = ship_card.copy(str(source), str(card))
            self.assertIn('injected card publication failure', failure)
            self.assertEqual((target / '00000000.app').read_bytes(), b'card old')
            self.assertEqual((prior / '00000000.app').read_bytes(), b'PC older')
            self.assertEqual((source / '00000000.app').read_bytes(), b'new')

            self.assertIn('on the card', ship_card.copy(str(source), str(card)))
        self.assertEqual((target / '00000000.app').read_bytes(), b'new')
        self.assertEqual((prior / '00000000.app').read_bytes(), b'card old')
        self.assertFalse(source.exists())

    def test_ship_locks_card_before_deciding_whether_to_keep_previous(self):
        card = self.root / 'fake-card'
        target = card / 'install' / 'Game'
        previous = self.root / 'previous' / 'Game'
        wup(self.root / 'source' / 'Game', b'shipped build')
        attempted = self.race_ship_and_publish('absence', target)
        self.assertEqual((target / '00000000.app').read_bytes(), b'shipped build')
        self.assertEqual(attempted.returncode, 1,
                         f'competing same-ID build was installed and then replaced, previous retained={previous.exists()}\n{attempted.stdout}')
        self.assertIn('Another build is using', attempted.stdout)
        other_source = self.root / 'other-source' / 'Game'
        wup(other_source, b'competing pack')
        with mock.patch.object(ship_card, 'PREVIOUS', str(previous.parent)):
            self.assertIn('on the card', ship_card.copy(str(other_source), str(card)))
        self.assertEqual((target / '00000000.app').read_bytes(), b'competing pack')
        self.assertEqual((previous / '00000000.app').read_bytes(), b'shipped build')

    def test_ship_holds_source_lock_through_cleanup(self):
        source = self.root / 'source' / 'Game'
        wup(source, b'shipped build')
        attempted = self.race_ship_and_publish('cleanup', source)
        self.assertEqual(attempted.returncode, 1,
                         f'competing source was published during cleanup, source remains={source.exists()}\n{attempted.stdout}')
        self.assertIn('Another build is using', attempted.stdout)
        self.assertFalse(source.exists())
        retry = subprocess.run([sys.executable, __file__, '--publish-same-title', str(source)],
                               capture_output=True, text=True, timeout=10)
        self.assertEqual(retry.returncode, 0, retry.stdout + retry.stderr)
        self.assertEqual((source / '00000000.app').read_bytes(), b'competing pack')
        self.assertEqual((self.root / 'fake-card' / 'install' / 'Game' / '00000000.app').read_bytes(), b'shipped build')

    def test_collision_keeps_different_wii_u_title(self):
        source, target = self.root / 'source' / 'Same_Name', self.root / 'out' / 'Same_Name'
        wup(source, b'new')
        wup(target, b'other title', 0x000500003d000864)
        with self.assertRaises(SystemExit):
            pipeline.copy_to(str(source), str(target.parent))
        self.assertEqual((target / '00000000.app').read_bytes(), b'other title')

    def test_failed_cemu_runner_cannot_reuse_previous_attempt(self):
        t = ports.Target('game', 'ff', str(self.root / 'out'), str(self.root / 'game'))
        code = Path(t.out, 'title', 'code')
        code.mkdir(parents=True)
        (code / 'made-up.rpx').touch()
        template = self.root / 'fake-cemu'
        template.mkdir()
        (template / 'CemuMario.exe').touch()
        runs = []
        evidence = '[fs] every file call goes straight\n[glsl] compiler thread ready\n[quit] sound system shut down\n'

        def runner(*argv):
            captures = Path(argv[argv.index('-Out') + 1])
            profile = Path(argv[argv.index('-CemuDir') + 1])
            self.assertNotEqual(profile, template)
            runs.append(captures)
            if len(runs) == 2:
                return SimpleNamespace(returncode=7, stdout='injected runner failure')
            (profile / f'portable/mlc01/usr/save/00050000/{t.low}/user/common/log.txt').write_text(evidence)
            (profile / 'portable/log.txt').write_text('fresh Cemu log')
            captures.mkdir(parents=True, exist_ok=True)
            (captures / 'tv_00.png').write_bytes(b'fake good capture')
            return SimpleNamespace(returncode=0, stdout='closed')

        with mock.patch.object(ports, 'CEMU', template.as_posix()), \
             mock.patch.object(ports, 'run', side_effect=runner), \
             mock.patch.object(ports, 'pictures', return_value=(None, None)) as compare:
            faults, _ = ports.judge(t, 60, [])
        self.assertEqual(compare.call_count, 1)
        self.assertNotEqual(runs[0], runs[1])
        self.assertTrue(any('injected runner failure' in f for f in faults), faults)

    def test_shader_discovery_and_variants_fail_closed(self):
        with mock.patch.object(build3ds, 'WORK', str(self.root / 'work')):
            g = SimpleNamespace(dir=self.root.as_posix(), name='synthetic')
            with self.subTest(stage='vertex discovery'), mock.patch.object(build3ds, 'run', return_value=SimpleNamespace(returncode=5, stdout='failed')):
                with self.assertRaisesRegex(SystemExit, 'dvlb_to_log failed'):
                    build3ds.prepare_vertex_logs(g, 'unused')
            with self.subTest(stage='fragment variants'), mock.patch.object(build3ds, 'prepare_vertex_logs'), \
                 mock.patch.object(build3ds, 'run', side_effect=[SimpleNamespace(returncode=0, stdout=''), SimpleNamespace(returncode=6, stdout='failed')]):
                with self.assertRaisesRegex(SystemExit, 'fs_variants failed'):
                    build3ds.prepare_logs(g, 'unused')
        (self.root / 'romfs.bin').touch()
        with self.subTest(stage='nested command-list scan'), \
             mock.patch.object(dvlb_to_log.subprocess, 'run', return_value=SimpleNamespace(returncode=7, stdout='failed')):
            with self.assertRaisesRegex(SystemExit, 'command-list discovery failed'):
                dvlb_to_log.listed(str(self.root), 'unused')

    def test_generated_c_tracks_code_metadata_and_romfs(self):
        g = SimpleNamespace(dir=self.root.as_posix(), gen_dir=(self.root / 'gen').as_posix())
        table = Path(g.gen_dir, 'table.c')
        table.parent.mkdir()
        names = ('functions.txt', 'code.bin', 'info.json', 'romfs.bin', 'update_romfs.bin')
        stamp = time.time() + 10
        for name in names:
            path = self.root / name
            path.touch()
            os.utime(path, (stamp - 2, stamp - 2))
        table.touch()
        os.utime(table, (stamp, stamp))
        with mock.patch.object(build3ds, 'functions_list', return_value=str(self.root / names[0])):
            self.assertFalse(build3ds.need_gen(g))
            for name in names[1:]:
                with self.subTest(name=name):
                    path = self.root / name
                    os.utime(path, (stamp + 2, stamp + 2))
                    self.assertTrue(build3ds.need_gen(g))
                    os.utime(path, (stamp - 2, stamp - 2))
            Path(self.root / 'update_romfs.bin').unlink()
            os.utime(self.root, (stamp + 2, stamp + 2))
            self.assertTrue(build3ds.need_gen(g))

    def test_failed_code_waits_for_started_shader_worker(self):
        state = ModuleType('lifecycle_test_events')
        state.ready, state.release, state.failed, finished = (threading.Event() for _ in range(4))
        outcomes = []

        def build_and_report():
            try:
                pipeline.main()
            except BaseException as e:
                outcomes.append(e)
            finally:
                finished.set()

        code = 'import lifecycle_test_events as e; e.ready.wait(3); e.failed.set(); raise SystemExit("code broke")'
        shaders = 'import lifecycle_test_events as e; e.ready.set(); e.release.wait(5)'
        with mock.patch.dict(sys.modules, {state.__name__: state}), fake_build(self.root, code=code, shaders=shaders):
            builder = threading.Thread(target=build_and_report)
            builder.start()
            try:
                self.assertTrue(state.failed.wait(3), outcomes)
                self.assertFalse(finished.wait(.1), 'the pipeline exited while its shader worker still ran')
            finally:
                state.release.set()
                builder.join(5)
        self.assertEqual(len(outcomes), 1)
        self.assertIsInstance(outcomes[0], SystemExit)

    def test_zip_failure_preserves_the_old_and_unrelated_zip(self):
        out = self.root / 'dist'
        app_folder = out / make_app.NAME
        (app_folder / '_internal').mkdir(parents=True)
        (app_folder / (make_app.NAME + '.exe')).write_bytes(b'fake app')
        here = self.root / 'builder'
        here.mkdir()
        for folder, names in ((here, ('compat.json', 'app_readme.txt', 'app_notices.txt')),
                              (self.root, ('LICENSE', 'THIRD_PARTY_LICENSES.md'))):
            for name in names:
                (folder / name).write_text('{}')
        version = '0.0'
        old = out / f'{make_app.NAME} {version} beta.zip'
        unrelated = out / 'another-release.zip'
        old.write_bytes(b'old release')
        unrelated.write_bytes(b'other release')

        def install_engine(path):
            Path(path).mkdir(exist_ok=True)
            Path(path, 'stamp.txt').write_text('synthetic')
            return 0

        package = ModuleType('PyInstaller')
        package.__main__ = SimpleNamespace(run=lambda args: None)
        with mock.patch.dict(sys.modules, {'PyInstaller': package, 'PyInstaller.__main__': package.__main__,
                                           'gui': SimpleNamespace(VERSION=version)}), \
             mock.patch.multiple(make_app, HERE=str(here), ROOT=str(self.root), OUT=str(out), WORK=str(self.root / 'work'),
                                 bundle_engine=install_engine, make_icon=mock.Mock(), version_resource=mock.Mock(), prune=lambda _: 0), \
             mock.patch.object(sys, 'argv', ['make_app.py']):
            with mock.patch.object(make_app.shutil, 'make_archive', side_effect=OSError('no space')):
                with self.assertRaisesRegex(OSError, 'no space'):
                    make_app.main()
            self.assertEqual(old.read_bytes(), b'old release')
            self.assertEqual(unrelated.read_bytes(), b'other release')
            make_app.main()
        self.assertTrue(old.read_bytes().startswith(b'PK'))
        self.assertEqual(unrelated.read_bytes(), b'other release')

    def test_old_updater_child_cannot_fail_new_trying_release(self):
        old_tag, new_tag = ('builder-1.0-beta-2026-11-15', 'builder-1.0-beta-2026-11-16')
        with mock.patch.object(updater, 'home', return_value=str(self.root)):
            def record(tag):
                exe = self.root / tag / updater.TOP / updater.EXE
                exe.parent.mkdir(parents=True)
                exe.touch()
                with updater.record_lock() as held:
                    self.assertTrue(held)
                    self.assertTrue(updater.write_record({'tag': tag, 'exe': str(exe.relative_to(self.root))}, 'trying'))
                return str(exe)

            old_exe = record(old_tag)

            def failed(seconds):
                record(new_tag)
                return 3

            with mock.patch.object(updater.subprocess, 'Popen', return_value=SimpleNamespace(wait=failed)):
                self.assertFalse(updater.hand_over(old_exe, []))
            self.assertEqual(updater.read_record()['state'], 'trying')
            self.assertEqual(updater.read_record()['tag'], new_tag)
            self.assertEqual(updater.failed_tags(), [])

    def test_explicit_out_survives_card_publication(self):
        card = self.root / 'fake-card'
        install = card / 'install'
        install.mkdir(parents=True)
        old_probe = install / '.3diiu_check'
        old_probe.write_bytes(b'existing file')
        with fake_build(self.root, args=('--card', str(card))):
            pipeline.main()
        for base in (self.root / 'output', install):
            self.assertEqual((base / 'Synthetic_Game' / '00000000.app').read_bytes(), b'completed')
        self.assertEqual(old_probe.read_bytes(), b'existing file')
        self.assertFalse(list(install.glob('.3diiu-check-*')))
        self.assertEqual({p.name for p in install.iterdir()}, {'Synthetic_Game', old_probe.name})

    def test_unreadable_download_play_still_builds_the_main_game(self):
        error = pipeline.decrypt.DecryptError('pyctr could not decode from ascii')
        with fake_build(self.root), mock.patch('builtins.print') as log, \
             mock.patch.object(pipeline.dumps, 'copy_dlp', side_effect=error):
            pipeline.main()
        log.assert_any_call(f'Warning: skipping unreadable optional Download Play data. {error}', flush=True)
        self.assertEqual((self.root / 'output/Synthetic_Game/00000000.app').read_bytes(), b'completed')

    def test_system_archive_decryption_error_names_the_input(self):
        error = pipeline.decrypt.DecryptError('pyctr could not decode from ascii')
        with fake_build(self.root, args=('--emulator', str(self.root))), mock.patch('builtins.print') as log, \
             mock.patch.object(pipeline.dumps, 'import_system_archives', side_effect=error):
            with self.assertRaises(SystemExit):
                pipeline.main()
        log.assert_any_call(f'@error Could not decrypt the selected 3DS system files. {error}', flush=True)
        self.assertFalse((self.root / 'output/Synthetic_Game').exists())

    def test_download_play_child_rejects_console_packaging(self):
        with fake_build(self.root, args=('--download-play-child', '0')), \
             mock.patch.dict(os.environ, THREEDII_MEASURE='console'), \
             mock.patch.object(pipeline, 'pack_wup') as pack:
            with self.assertRaises(SystemExit):
                pipeline.main()
        pack.assert_not_called()
        self.assertFalse((self.root / 'output').exists())

    def test_download_play_child_build_uses_separate_title(self):
        def extracted(parent, title_id, folder, tmp, keys):
            self.assertEqual(title_id, 0x0004000100086302)
            (Path(folder) / 'code.bin').write_bytes(b'child program')

        with fake_build(self.root, args=('--download-play-child', '2'), measure=True), \
             mock.patch.dict(os.environ, THREEDII_MEASURE='1'), \
             mock.patch.object(pipeline, 'extract_dlp_child', side_effect=extracted) as extract, \
             mock.patch.object(pipeline, 'make_title') as title, mock.patch.object(pipeline, 'pack_wup') as pack:
            pipeline.main()
        extract.assert_called_once()
        pack.assert_not_called()
        folder, out, info = title.call_args.args[:3]
        self.assertEqual(info['title_id'], '000500003E086302')
        self.assertEqual(Path(out).parent.name, 'dlp_0004000100086302')
        self.assertEqual((Path(folder) / 'code.bin').read_bytes(), b'child program')

    def test_publication_lock_is_local_and_releases_on_process_exit(self):
        target = self.root / 'fake-card' / 'install' / 'Game'
        ready = self.root / 'publication-ready'
        locks = self.local_data / '3Dii U Builder' / 'publication_locks'
        holder = subprocess.Popen([sys.executable, __file__, '--hold-publication', str(self.root)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(ready, holder)
            self.assertTrue(ready.is_file())
            lock, = locks.glob('*.lock')
            self.assertFalse(target.parent.exists())
            for _ in range(2):
                with self.assertRaisesRegex(OSError, 'Another build is using'):
                    with pipeline.wup_safety.publication_lock(target):
                        self.fail('an active publisher lost its lock')
                self.assertTrue(lock.is_file())
        finally:
            holder.kill()
            holder.wait(timeout=10)
        with pipeline.wup_safety.publication_lock(target):
            self.assertTrue(lock.is_file())
        self.assertFalse(target.parent.exists())


if __name__ == '__main__':
    if sys.argv[1:2] in (['--hold'], ['--probe'], ['--hold-other-build']):
        lock_process(Path(sys.argv[2]), sys.argv[1] != '--probe', sys.argv[1] == '--hold-other-build')
    elif sys.argv[1:2] == ['--publish']:
        publish_process(Path(sys.argv[2]), sys.argv[3] == 'first')
    elif sys.argv[1:2] == ['--ship']:
        ship_process(Path(sys.argv[2]), sys.argv[3])
    elif sys.argv[1:2] == ['--publish-same-title']:
        publish_same_title(Path(sys.argv[2]))
    elif sys.argv[1:2] == ['--hold-publication']:
        root = Path(sys.argv[2])
        with pipeline.wup_safety.publication_lock(root / 'fake-card' / 'install' / 'Game'):
            (root / 'publication-ready').touch()
            threading.Event().wait(20)
    elif sys.argv[1:2] == ['--embedded-child']:
        root = Path(sys.argv[2])
        with mock.patch.multiple(app, FROZEN=True, find_root=lambda: str(root / 'engine'), hand_over=lambda: None, quiet_children=lambda: None), \
             mock.patch.object(sys, 'argv', ['Builder.exe', '-P', str(root / 'script.py')]):
            app.main()
    else:
        unittest.main()
