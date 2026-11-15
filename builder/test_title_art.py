"""Self-check for the start-up screens in the Virtual Console's manner (tools/make_game_title.py, console_screens): their sizes, the band and the
art where they go, a GameTDB case scan's strip left out and its whole height kept, the release line, and the names read from a made-up SMDH;
and the GamePad's bezel picture (bezel_pixels): its size, scale, cut, row order and see-through parts. Run: python builder/test_title_art.py"""
import os
import struct
import sys
import tempfile

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')  # no window needed; the text isn't checked
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import make_game_title as title  # noqa: E402
from PySide6.QtGui import QColor, QImage, QPainter  # noqa: E402


def smdh(english, publisher):
    """an SMDH with a Japanese name, an English one if given, and a green 48x48 icon"""
    d = bytearray(0x36C0)
    d[:4] = b'SMDH'
    for lang, name, pub in ((0, 'ゲーム', 'P-ja'), (1, english, publisher)):
        at = 8 + 0x200 * lang
        for off, text in ((0x80, name), (0x180, pub)):
            raw = text.encode('utf-16-le')
            d[at + off:at + off + len(raw)] = raw
    for i in range(48 * 48):
        struct.pack_into('<H', d, 0x24C0 + 2 * i, 0x07E0)
    return bytes(d)


def near(rgb, want, slack=40):
    return all(abs(a - b) <= slack for a, b in zip(rgb, want))


BLUE, WHITE, GREEN = (20, 40, 220), (255, 255, 255), (20, 200, 20)
with tempfile.TemporaryDirectory() as tmp:
    icon, japanese = os.path.join(tmp, 'icon.bin'), os.path.join(tmp, 'ja.bin')
    open(icon, 'wb').write(smdh('Some\nGame  Name', ' Maker '))
    open(japanese, 'wb').write(smdh('', ''))
    assert title.smdh_names(icon) == ('Some Game Name', 'Maker'), title.smdh_names(icon)
    assert title.smdh_names(japanese) == ('ゲーム', 'P-ja'), title.smdh_names(japanese)

    # a picture of any other size fills its frame whole: 344 wide, as tall as its shape asks within 200-300, centred in the band
    cover = os.path.join(tmp, 'cover.png')
    picture = QImage(300, 400, QImage.Format_RGB888)
    picture.fill(QColor(*BLUE))
    assert picture.save(cover)
    screens = title.console_screens(cover, icon, 'Released in Europe: 2011')
    tv, pad = screens['bootTvTex.tga'], screens['bootDrcTex.tga']
    assert (len(tv), len(tv[0]), len(pad), len(pad[0])) == (720, 1280, 480, 854)
    assert near(tv[372][244], BLUE) and near(tv[230][80], BLUE) and near(tv[514][408], BLUE), (tv[372][244], tv[230][80], tv[514][408])
    assert near(tv[200][480], WHITE, 12), tv[200][480]                 # the band
    assert near(tv[10][10], (247, 247, 247), 12), tv[10][10]          # the background above it
    assert near(tv[700][400], (214, 214, 214), 16), tv[700][400]      # and below
    assert near(pad[248][163], BLUE), pad[248][163]                    # the GamePad's is the TV's, smaller

    # a GameTDB case scan (768x680, case_scan): its white strip down the right is left out, the whole height stays (the green foot, where a
    # logo can sit), and it shows whole in its frame, 344 across and as tall as its shape asks
    case = os.path.join(tmp, 'case.jpg')
    picture = QImage(768, 680, QImage.Format_RGB888)
    picture.fill(QColor(*BLUE))
    paint = QPainter(picture)
    paint.fillRect(684, 0, 84, 680, QColor(*WHITE))
    paint.fillRect(0, 600, 684, 80, QColor(*GREEN))
    paint.end()
    assert picture.save(case, quality=95)
    tv = title.console_screens(case, icon, case_scan=True)['bootTvTex.tga']
    top, bottom = round(372 - 342 / 2) + 3, round(372 + 342 / 2) - 4
    for y, x in ((top, 76), (top, 412), (372, 244), (372, 412)):
        assert near(tv[y][x], BLUE), (y, x, tv[y][x])
    for y, x in ((bottom, 76), (bottom, 412), (bottom, 244)):
        assert near(tv[y][x], GREEN), (y, x, tv[y][x])
    assert near(tv[372][424], WHITE, 12) and near(tv[190][244], WHITE, 12), 'the frame is not the picture whole'
    # the same picture from the user (not a case scan) isn't cut: its white strip is still at its right
    tv = title.console_screens(case, icon)['bootTvTex.tga']
    assert near(tv[372][410], WHITE, 20), tv[372][410]
    # a tall case scan fits the band's height with a margin
    tall = QImage(768, 1400, QImage.Format_RGB888)
    tall.fill(QColor(*BLUE))
    assert tall.save(os.path.join(tmp, 'tall.png'))
    tv = title.console_screens(os.path.join(tmp, 'tall.png'), icon, case_scan=True)['bootTvTex.tga']
    assert near(tv[372][200], BLUE) and near(tv[210][200], BLUE) and near(tv[530][200], BLUE), 'a tall picture is cut'
    assert near(tv[190][200], WHITE, 12) and near(tv[550][200], WHITE, 12), 'a tall picture runs to the band edge'

    # the line under the game's name: released's words, or the maker, or nothing where there is neither
    nameless = os.path.join(tmp, 'nameless.bin')
    open(nameless, 'wb').write(smdh('Some Game', ''))
    with_year = title.console_screens(cover, nameless, 'Released in Europe: 2011')['bootTvTex.tga']
    without = title.console_screens(cover, nameless)['bootTvTex.tga']
    line = lambda tv, a, b: [row[552:1240] for row in tv[a:b]]
    assert line(with_year, 456, 504) != line(without, 456, 504), 'the release line is not drawn'
    assert line(with_year, 344, 440) == line(without, 344, 440) and line(with_year, 510, 560) == line(without, 510, 560)

    open(os.path.join(tmp, 'bad.png'), 'wb').write(b'not a picture')
    assert title.console_screens(os.path.join(tmp, 'bad.png'), icon) is None

    # the GamePad's bezel: 854x480 RGBA, row 0 the top. A 3416x960 picture is halved, then cut at its sides and centred; its top rows yellow,
    # a see-through part black.
    wide = QImage(3416, 960, QImage.Format_RGBA8888)
    wide.fill(QColor(0, 0, 0, 0))
    painter = QPainter(wide)
    painter.fillRect(0, 0, 854, 960, QColor(255, 0, 0))  # cut off
    painter.fillRect(854, 0, 1708, 960, QColor(0, 0, 255))  # kept: the middle
    painter.fillRect(2562, 0, 854, 960, QColor(255, 0, 0))  # cut off
    painter.fillRect(854, 0, 1708, 40, QColor(255, 255, 0))  # the top rows
    painter.setCompositionMode(QPainter.CompositionMode_Source)
    painter.fillRect(854, 0, 200, 960, QColor(0, 255, 0, 0))  # see-through within it
    painter.end()
    wide_path = os.path.join(tmp, 'bezel.png')
    assert wide.save(wide_path)
    px = title.bezel_pixels(wide_path)
    assert px and len(px) == 854 * 480 * 4, len(px or b'')
    at = lambda x, y: tuple(px[(y * 854 + x) * 4:(y * 854 + x) * 4 + 4])
    assert at(2, 2)[:3] == (0, 0, 0) and at(50, 240) == (0, 0, 0, 255), 'a see-through part is not black'
    assert near(at(400, 5)[:3], (255, 255, 0)) and near(at(400, 470)[:3], (0, 0, 255)), ('upside down, or not scaled', at(400, 5), at(400, 470))
    assert near(at(850, 240)[:3], (0, 0, 255)), ('not cut at the sides', at(850, 240))
    assert title.bezel_pixels(os.path.join(tmp, 'bad.png')) is None
print('title art: ok')
