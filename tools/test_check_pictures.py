#!/usr/bin/env python3
"""tools/check_ports.py's picture check, on made-up pictures: test_check_pictures.py. Needs PySide6, as the check does."""
import os
import sys
import tempfile
from itertools import count
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from PySide6.QtGui import QColor, QImage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as c  # noqa: E402


def cemu_template():
    portable = Path(c.CEMU) / 'portable'
    portable.mkdir(parents=True)
    (portable.parent / 'CemuMario.exe').touch()
    (portable / 'settings.xml').write_text('<settings><mlc_path>original</mlc_path></settings>')


def leg(t, colours, minimum_pairs=1):
    """a leg's screenshots as cemu_run.ps1 leaves them, each one flat colour and the same on both screens, and what the check says of them"""
    os.makedirs(t.data + '/shots/cemu', exist_ok=True)
    for i, colour in enumerate(colours):
        img = QImage(128, 72, QImage.Format_RGB888)
        img.fill(QColor(colour))
        for screen in ('tv', 'pad'):
            img.save(f'{t.data}/shots/cemu/{screen}_{i:02d}.png')
    return c.pictures(t, 'blank_save', minimum_pairs=minimum_pairs)


def test_judge_quit_routes():
    """Strict evidence needs a complete, gracefully closed log from every leg."""
    with tempfile.TemporaryDirectory() as folder, mock.patch.object(c, 'CEMU', folder + '/cemu'):
        cemu_template()
        seed = folder + '/seed'
        os.makedirs(seed)
        with open(seed + '/test.bin', 'w') as file:
            file.write('private test seed')
        route = ['A 6', 'WAIT 100000', 'B 6']
        cases = [(True, False, False), (True, True, False), (True, False, True),
                 (False, False, False), (False, True, False), (False, False, True)]
        for index, (strict, kept, environment) in enumerate(cases):
            target = c.Target('closure_' + str(index), 'ff', folder + '/out' + str(index), folder + '/data' + str(index),
                              save=seed if kept else '', env={'TEST_SCENE': '1'} if environment else {})
            routes = []
            for storage in (target.common, target.player):
                marker = Path(storage) / 'manual-save.bin'
                marker.parent.mkdir(parents=True, exist_ok=True)
                marker.write_bytes(b'manual save')

            def renderer(current, seconds, interp, shots, captures):
                assert current.cemu_dir != c.CEMU
                assert not (Path(current.common) / 'manual-save.bin').exists()
                assert not (Path(current.player) / 'manual-save.bin').exists()
                assert (Path(current.player) / 'save/test.bin').is_file() == kept
                (Path(current.cemu_dir) / 'portable/log.txt').write_text('mock Cemu run')
                with open(current.common + '/live.txt') as file:
                    lines = file.read().splitlines()
                routes.append(lines)
                with open(current.common + '/log.txt', 'w') as file:
                    file.write('[fs] every file call goes straight\n[glsl] compiler thread ready\n')
                    file.write('[gx2] 60 fps, 0 on CPU, 0 fragment misses\n')
                    file.write(''.join(f'[test] {key} is {value}\n' for key, value in current.env.items()))
                    if lines[-1] == 'KEY quit':
                        file.write('[gx2 final] native draws 6, CPU batches 0, fragment misses 0, immediate native draws 1, immediate CPU runs 0\n')
                        file.write('[quit] sound system shut down\n')
                return 'closed'

            with mock.patch.object(c, 'STRICT', strict), mock.patch.dict(c.ROUTES, {target.name: route}), \
                 mock.patch.object(c, 'cemu', side_effect=renderer), mock.patch.object(c, 'pictures', return_value=(None, None)):
                faults, notes = c.judge(target, 80, [], measured=strict)
            assert len(routes) == (1 if kept or environment else 2), (strict, kept, environment, routes)
            for leg_index, lines in enumerate(routes):
                if strict or leg_index:
                    assert lines == ['A 6', 'KEY quit'], ('quit route is not bounded/complete', strict, kept, environment, lines)
                else:
                    assert lines == route, ('ordinary first-leg route changed', kept, environment, lines)
            assert not faults, (strict, kept, environment, faults, notes)
            for storage in (target.common, target.player):
                assert (Path(storage) / 'manual-save.bin').read_bytes() == b'manual save'
        settings = Path(c.CEMU) / 'portable/settings.xml'
        assert settings.read_text() == '<settings><mlc_path>original</mlc_path></settings>'
        original = '<settings><Graphic><AsyncCompile>true</AsyncCompile></Graphic></settings>'
        settings.write_text(original)
        with tempfile.TemporaryDirectory() as scratch:
            profile = Path(c.cemu_profile(scratch))
            made = c.ET.parse(profile / 'portable/settings.xml')
            assert made.findtext('Graphic/AsyncCompile') == 'false'
        assert settings.read_text() == original
        settings.unlink()
        with tempfile.TemporaryDirectory() as scratch:
            profile = Path(c.cemu_profile(scratch))
            made = c.ET.parse(profile / 'portable/settings.xml')
            assert Path(made.findtext('mlc_path')) == profile / 'portable/mlc01'
            assert made.findtext('Graphic/AsyncCompile') == 'false'
        assert not settings.exists()


def main():
    with tempfile.TemporaryDirectory() as d:
        t = c.Target('game', '0', d, d.replace('\\', '/'))
        fault, note = leg(t, ['red', 'green', 'blue', 'white'])
        assert not fault and 'no good pictures' in note, (fault, note)
        assert c.bless(t) == '8 pictures kept as a good run'
        assert leg(t, ['red', 'green', 'blue', 'white']) == (None, "blank_save: pictures like a good run's")
        assert not leg(t, ['red', 'green', 'blue', 'black'])[0]  # one picture off: a frame caught at another moment
        assert leg(t, ['red', 'green', 'yellow', 'cyan'])[0]     # two pictures no good run has
        assert leg(t, ['red', 'red', 'red', 'green'])[0]         # nothing wrong in what's there, but two scenes of the good run never came
        QImage(8, 8, QImage.Format_RGB888).save(t.data + '/shots/cemu/tv_00.png')
        assert 'GamePad' in c.pictures(t, 'blank_save')[0]       # a screen with no pictures at all
        # An early exit must not be described as missing reference scenes. Keep
        # it a failure even without references, before visual comparison begins.
        coverage = c.Target('coverage', '0', d, d.replace('\\', '/') + '/coverage')
        for colours in ([], ['red']):
            fault, note = leg(coverage, colours, minimum_pairs=2)
            assert 'insufficient visual coverage' in fault and not note, (fault, note)
            assert f'{len(colours)} TV/GamePad capture pairs' in fault
        assert not leg(coverage, ['red', 'green'], minimum_pairs=2)[0]
        assert not leg(coverage, ['red'])[0]  # a caller intentionally asking for one capture
        # Two files per screen are not two pairs if their capture numbers differ.
        leg(coverage, ['red', 'green'])
        kept = c.kept(coverage, 'check_shots') + '/blank_save'
        incoming = coverage.data + '/shots/cemu'
        for name in os.listdir(kept):
            os.rename(kept + '/' + name, incoming + '/' + name.replace('pad_01', 'pad_02'))
        assert 'insufficient visual coverage' in c.pictures(coverage, 'blank_save', minimum_pairs=2)[0]
        blank = c.Target('blank', '0', d, d.replace('\\', '/') + '/blank')
        fault, note = leg(blank, ['black', 'black'], minimum_pairs=2)
        assert 'nothing drawn' in fault and not note, (fault, note)
        # A bad saved reference cannot make an entirely black run pass.
        c.bless(blank)
        assert 'nothing drawn' in leg(blank, ['black', 'black'], minimum_pairs=2)[0]
        assert not leg(blank, ['black'])[0]  # a single intentional capture is inconclusive
        visible = c.Target('visible', '0', d, d.replace('\\', '/') + '/visible')
        assert not leg(visible, ['black', 'green'], minimum_pairs=2)[0]  # a normal fade
        # One black screen and fine detail on the other are both valid output.
        for screen in ('tv', 'pad'):
            for n in (0, 1):
                img = QImage(1280, 720, QImage.Format_RGB888)
                img.fill(QColor('black'))
                if screen == 'pad':
                    img.setPixelColor(31, 47, QColor('white'))
                img.save(f'{visible.data}/shots/cemu/{screen}_{n:02d}.png')
        assert not c.pictures(visible, 'blank_save', minimum_pairs=2)[0]
        design = c.Target('design', '0', d, d.replace('\\', '/') + '/design', route='acnl_design')
        incoming = design.data + '/shots/cemu'
        os.makedirs(incoming)
        editor = QImage(320, 240, QImage.Format_RGB888)
        editor.fill(QColor('white'))
        for screen in ('tv', 'pad'):
            editor.save(f'{incoming}/{screen}_00.png')
        assert 'design strokes missing' in c.pictures(design, 'kept_save')[0]
        editor.save(f'{incoming}/tv_00.png')
        for x, y in ((125, 71), (145, 71), (185, 71), (135, 111), (185, 111), (125, 155), (195, 155)):
            editor.setPixelColor(x, y, QColor('#ffaa88'))
        editor.save(f'{incoming}/pad_00.png')
        assert not c.pictures(design, 'kept_save')[0]
        for screen in ('tv', 'pad'):
            editor.save(f'{incoming}/{screen}_00.png')
        editor.setPixelColor(125, 155, QColor('white'))
        for screen in ('tv', 'pad'):
            editor.save(f'{incoming}/{screen}_01.png')
        assert 'design strokes missing' in c.pictures(design, 'kept_save')[0]
        corrupt = c.Target('corrupt', '0', d, d.replace('\\', '/') + '/corrupt')
        incoming = corrupt.data + '/shots/cemu'
        os.makedirs(incoming)
        for screen in ('tv', 'pad'):
            for n in (0, 1):
                with open(f'{incoming}/{screen}_{n:02d}.png', 'wb') as f:
                    f.write(b'not an image')
        fault, note = c.pictures(corrupt, 'blank_save', minimum_pairs=2)
        assert fault and 'unreadable' in fault and not note, (fault, note)
        reference = c.Target('bad_reference', '0', d, d.replace('\\', '/') + '/bad_reference')
        leg(reference, ['red', 'green'])
        c.bless(reference)
        import glob
        with open(glob.glob(reference.data + '/good_shots/**/*.png', recursive=True)[0], 'wb') as f:
            f.write(b'corrupt reference')
        fault, note = leg(reference, ['red', 'green'])
        assert fault and 'reference' in fault and 'unreadable' in fault and not note, (fault, note)
    # A restart can quit after 35s while the interpreter has a 150s allowance.
    # Keep its 50s/6 cadence, rather than capturing just once at 150s/6.
    with tempfile.TemporaryDirectory() as d, mock.patch.object(c, 'CEMU', d + '/cemu'):
        t = c.Target('cadence', 'ff', d + '/out', d + '/game')
        os.makedirs(t.out + '/title/code')
        open(t.out + '/title/code/game.rpx', 'wb').close()
        cemu_template()
        calls = []
        sequence = count(1)

        def runner(*argv):
            seconds = int(argv[argv.index('-Seconds') + 1])
            shots = int(argv[argv.index('-Shots') + 1])
            interp = '-Interpreter' in argv
            calls.append((seconds, shots, interp))
            profile = argv[argv.index('-CemuDir') + 1]
            assert profile != c.CEMU
            with open(profile + '/portable/log.txt', 'w') as f:
                f.write('attempt ' + 'x' * next(sequence))
            log = profile + '/portable/mlc01/usr/save/00050000/ff/user/common/log.txt'
            if len(calls) > 1 and not interp:
                if os.path.exists(log):
                    os.remove(log)
                return SimpleNamespace(stdout='exited by itself', returncode=0)
            with open(log, 'w') as f:
                f.write('[fs] every file call goes straight\n[glsl] compiler thread ready\n[quit] sound system shut down\n')
            count = int((35 if interp else seconds) / (seconds / shots))
            folder = argv[argv.index('-Out') + 1]
            os.makedirs(folder, exist_ok=True)
            img = QImage(128, 72, QImage.Format_RGB888)
            img.fill(QColor('green'))
            for i in range(count):
                for screen in ('tv', 'pad'):
                    img.save(f'{folder}/{screen}_{i:02d}.png')
            return SimpleNamespace(stdout='closed', returncode=0)

        with mock.patch.object(c, 'run', side_effect=runner):
            faults, notes = c.judge(t, 200, [])
        assert calls[0] == (200, c.SHOTS, False)
        assert calls[-1] == (150, c.SHOTS * 3, True), calls
        assert not any('insufficient visual coverage' in f for f in faults), (faults, notes)
        assert len(os.listdir(c.kept(t, 'check_shots') + '/restart')) >= 4
        calls.clear()
        with mock.patch.object(c, 'run', side_effect=runner), mock.patch.object(c, 'STRICT', True):
            faults, notes = c.judge(t, 200, [], measured=True)
        assert any('unverified rendering' in fault for fault in faults), faults
        assert any('lacks complete execution counters' in fault for fault in faults), faults
    test_judge_quit_routes()
    print('ok')


if __name__ == '__main__':
    main()
