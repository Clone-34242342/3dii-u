#!/usr/bin/env python3
"""check_ports.mark_built against made-up work folders in a temporary folder: test_mark_built.py"""
import os
import shutil
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as cp  # noqa: E402

d = tempfile.mkdtemp().replace('\\', '/')
cp.ROOT = d
for slug, program in (('g_one_v1', '0004000000111100'), ('g_two_v1', '0004000000222200'), ('g_old', '0004000000111100')):
    os.makedirs(f'{d}/builder_work/{slug}/out')
    with open(f'{d}/builder_work/{slug}/info.json', 'w') as f:
        f.write('{"program_id": "%s"}' % program)
os.utime(d + '/builder_work/g_old/out', (1000, 1000))
name = lambda slug: open(f'{d}/builder_work/{slug}/check_name.txt').read() if os.path.exists(f'{d}/builder_work/{slug}/check_name.txt') else None
# two games built side by side: both folders are newer than either build's start
cp.mark_built('one', 2000, 'Game One (0004000000111100): Not tested, update v1\n@stage Decrypting\n')
assert (name('g_one_v1'), name('g_two_v1'), name('g_old')) == ('one', None, None), "only its own game's folder, and not one from an older build"
cp.mark_built('two', 2000, 'Game Two (0004000000222200): Not tested\n')
assert (name('g_one_v1'), name('g_two_v1')) == ('one', 'two'), "the other build leaves the first one's mark"
cp.mark_built('three', 2000, 'no title line: the build failed before it\n')
assert (name('g_one_v1'), name('g_two_v1')) == ('one', 'two'), 'nothing marked without the game named'

title = Path(d) / 'builder_work/g_one_v1/out/title'
(title / 'code').mkdir(parents=True)
(title / 'code/app.xml').write_text('<app><title_id>000500003d001111</title_id></app>')
(title / 'code/game.rpx').write_bytes(b'rpx')
(title / 'content').mkdir()
romfs = title / 'content/romfs.bin'
romfs.write_bytes(b'fixture')
cp.mark_built('one', 2000, 'Game One (0004000000111100): Not tested\n')
assert cp.built_from_dump('one').rpx == (title / 'code/game.rpx').as_posix()
romfs.write_bytes(b'torn')
assert cp.built_from_dump('one') is None, 'a shortened payload cannot pass as a playable cached title'
romfs.unlink()
assert cp.built_from_dump('one') is None, 'a deleted payload cannot pass as a playable cached title'
romfs.write_bytes(b'fixture')
assert cp.built_from_dump('one') is not None

cold = Path(d) / 'builder_work/g_one_v1_cold'
shutil.copytree(title, cold / 'out/title')
shutil.copyfile(title.parent.parent / 'info.json', cold / 'info.json')
cp.mark_built('one_cold', 2000, 'Game One (0004000000111100): Not tested\n')
assert name('g_one_v1') == 'one', 'marking a cold build must never overwrite the warm build marker'
assert cp.built_from_dump('one_cold').out == (cold / 'out').as_posix()
(cold / 'check_name.txt').write_text('one')
assert cp.built_from_dump('one').out == title.parent.as_posix(), 'a warm check cannot use a stale cold marker'

def check_cold_route(extra, expected):
    entry = {'game': 'synthetic.3ds', **extra}
    t = cp.Target('nsmb2_cold', '3d0007ae', cold.as_posix(), d)
    with (patch.object(sys, 'argv', ['check_ports.py', 'nsmb2', '--cold']),
          patch.object(cp, 'sources', return_value={'games': {'nsmb2': entry}, 'otp': 'unused'}),
          patch.object(cp, 'build_port') as warm,
          patch.object(cp, 'build_from_dump', return_value=t) as build,
          patch.object(cp, 'built_from_dump', return_value=None),
          patch.object(cp, 'judge', return_value=([], [])) as judge):
        try:
            cp.main()
        except SystemExit as e:
            assert e.code == 0
        warm.assert_not_called()
        build.assert_called_once_with('nsmb2_cold', entry, 'unused')
        assert judge.call_args.args[0].route == expected

check_cold_route({}, 'nsmb2')
check_cold_route({'route': 'nsmb2_run'}, 'nsmb2_run')
shutil.rmtree(d)
print('mark_built: ok')
