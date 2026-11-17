#!/usr/bin/env python3
"""tools/check_ports.py's picture check, on made-up pictures: test_check_pictures.py. Needs PySide6, as the check does."""
import os
import sys
import tempfile

from PySide6.QtGui import QColor, QImage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as c  # noqa: E402


def leg(t, colours, minimum_pairs=1):
    """a leg's screenshots as cemu_run.ps1 leaves them, each one flat colour and the same on both screens, and what the check says of them"""
    os.makedirs(t.data + '/shots/cemu', exist_ok=True)
    for i, colour in enumerate(colours):
        img = QImage(128, 72, QImage.Format_RGB888)
        img.fill(QColor(colour))
        for screen in ('tv', 'pad'):
            img.save(f'{t.data}/shots/cemu/{screen}_{i:02d}.png')
    return c.pictures(t, 'blank_save', minimum_pairs=minimum_pairs)


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
    print('ok')


if __name__ == '__main__':
    main()
