"""Self-check for app.py's compiled-cache reset and its output's encoding, and for make_app.py's release tag and its check that a Complete game
has its confirmed shader list: python builder/test_app.py"""
import importlib.util
import os
import shutil
import tempfile
import types
from unittest import mock

spec = importlib.util.spec_from_file_location('app', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.py'))
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)

base = tempfile.mkdtemp()
engine, work = os.path.join(base, 'engine'), os.path.join(base, 'work')
cache = os.path.join(work, 'build_win_builder')


def compiled():
    os.makedirs(cache, exist_ok=True)
    open(os.path.join(cache, 'x.o'), 'w').close()


def stamp(s):
    with open(os.path.join(engine, 'stamp.txt'), 'w') as f:
        f.write(s)


try:
    os.makedirs(engine)
    compiled()
    app.fresh_cache(engine, work)
    assert os.path.exists(os.path.join(cache, 'x.o')), 'an engine without a stamp leaves the cache alone'
    stamp('1')
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'a new engine starts the cache over'
    compiled()
    app.fresh_cache(engine, work)
    assert os.path.exists(os.path.join(cache, 'x.o')), 'the same engine keeps it'
    stamp('2')
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'another engine starts it over again'
    compiled()
    stamp('3')
    with mock.patch.object(shutil, 'rmtree', return_value=None):  # Windows can leave locked objects behind
        try:
            app.fresh_cache(engine, work)
        except OSError:
            pass
        else:
            raise AssertionError('a failed cache cleanup allowed the build to use old objects')
    with open(os.path.join(work, 'engine_stamp.txt')) as f:
        assert f.read() == '2', 'a failed cache cleanup advanced the engine stamp'
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'cleanup was not retried after the lock was released'
    other = os.path.join(base, 'other-build-folder')
    other_cache = os.path.join(other, 'build_win_builder')
    os.makedirs(other_cache)
    open(os.path.join(other_cache, 'old.o'), 'w').close()
    with open(os.path.join(other, 'engine_stamp.txt'), 'w') as f:
        f.write('older engine')
    with mock.patch.dict(os.environ, {'THREEDII_ROOT': engine, 'THREEDII_WORK': work, 'THREEDII_BUILD': other}), \
            mock.patch.object(app.sys, 'argv', ['Builder.exe', '--pipeline', '--game', 'unused']):
        app.use_engine(engine)
    assert not os.path.exists(other_cache), 'a pipeline inherited the engine but reused another build folder\'s stale objects'
    print('fresh_cache ok')
    # what a build prints reaches the window as UTF-8 whatever the PC's code page: a name with ★ or ™ used to end the build
    import subprocess
    import sys
    script = os.path.join(base, 'say.py')
    with open(script, 'w', encoding='utf-8') as f:
        f.write('print("Kumamon\\u2605 SUPER STREET FIGHTER\\u2122 \\u30df\\u30a4")\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}
    r = subprocess.run([sys.executable, '-X', 'utf8=0', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.py'), script], capture_output=True, env=env)
    assert r.returncode == 0 and r.stdout.decode('utf-8').strip() == 'Kumamon★ SUPER STREET FIGHTER™ ミイ', (r.returncode, r.stdout, r.stderr)
    print('output encoding ok')
    # a build to publish is given its release's tag, and only one of this version's
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_app
    tag = 'builder-1.0-beta-2026-11-13'
    assert make_app.release_tag(['make_app.py'], '1.0') == '' and make_app.release_tag(['make_app.py', '--release', tag], '1.0') == tag
    for bad in (['--release'], ['--release', 'deps-1'], ['--release', 'builder-2.0-beta-2026-11-13'], ['--release', '1.0']):
        try:
            make_app.release_tag(['make_app.py'] + bad, '1.0')
        except SystemExit:
            continue
        raise AssertionError(f'{bad} was taken as a release tag')
    print('release tag ok')
    # Collection must not prefer an unrelated tool's DLLs over Windows' own ICU.
    # A failed collector must also restore PATH for the remaining build tools.
    class Collected(Exception):
        pass

    def collect(args):
        assert os.environ['PATH'].split(os.pathsep)[0] == os.path.join(os.environ['WINDIR'], 'System32')
        raise Collected

    installer = types.ModuleType('PyInstaller')
    installer.__main__ = types.ModuleType('PyInstaller.__main__')
    installer.__main__.run = collect
    with mock.patch.dict(sys.modules, {'PyInstaller': installer, 'PyInstaller.__main__': installer.__main__,
                                      'gui': types.SimpleNamespace(VERSION='1.0')}), \
            mock.patch.dict(os.environ, {'PATH': 'unrelated-tools', 'WINDIR': os.environ.get('WINDIR', 'C:/Windows')}), \
            mock.patch.object(make_app, 'make_icon'), mock.patch.object(make_app, 'version_resource'), \
            mock.patch.object(make_app.os, 'makedirs'):
        try:
            make_app.main()
        except Collected:
            pass
        else:
            raise AssertionError('did not reach the collector')
        assert os.environ['PATH'] == 'unrelated-tools'
    print('packaging DLL search order ok')
    # a game called Complete must have its confirmed shader list before the app is made; the list's notes and other games don't count
    games = os.path.join(base, 'games')
    compat = {'_about': 'notes', '_cemu': [{'status': 'Cemu'}], '1': {'slug': 'done', 'status': 'Complete'}, '2': {'slug': 'plays', 'status': 'Plays'}}
    assert make_app.unconfirmed(compat, games) == ['done']
    listed = os.path.join(games, 'done', 'shader_logs', 'confirmed.bin')
    os.makedirs(os.path.dirname(listed))
    open(listed, 'wb').close()
    assert make_app.unconfirmed(compat, games) == ['done'], 'an empty list is no list'
    states = b'\x09\x00\x00\x00state one' + b'\x09\x00\x00\x00state two'
    with open(listed, 'wb') as f:
        f.write(states)
    assert make_app.unconfirmed(compat, games) == []
    make_app.bundle_logs(os.path.join(games, 'done'), os.path.join(base, 'shipped'))  # and the app ships it as it is
    with open(os.path.join(base, 'shipped', 'shader_logs', 'confirmed.bin'), 'rb') as f:
        assert f.read() == states
    print('confirmed lists ok')
finally:
    shutil.rmtree(base)
