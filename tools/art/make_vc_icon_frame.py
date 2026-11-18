#!/usr/bin/env python3
"""Draws vc_icon_frame.png, the bands and labels every game's Wii U Menu icon gets over its own picture (Walegy's design, issue #45;
make_game_title.vc_icon_pixels). Run by hand on Windows, with Pillow and Windows' Arial; the Builder only reads the PNG.
    python tools/art/make_vc_icon_frame.py"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
HEADER, RED, WHITE, EDGE = (226, 229, 229, 255), (204, 27, 37, 255), (255, 255, 255, 255), (30, 30, 30, 230)
BAND = (0, 0, 0, 104)
# (text, font, where its glyphs go at 512x512, colour, outline width): the header bold with a dark outline, to read over any picture at the
# Wii U Menu's 128x128; the footer as the design has it
LABELS = (('NINTENDO', 'arialbd.ttf', (34, 24, 274, 51), HEADER, 3), ('3', 'arialbd.ttf', (279, 24, 339, 76), RED, 3),
          ('DS', 'arialbd.ttf', (346, 24, 474, 76), HEADER, 3), ('Virtual Console', 'arialbi.ttf', (18, 437, 494, 479), WHITE, 0))


def label(text, font, rect, colour, edge):
    """the text drawn at size 100, cropped to its glyphs and stretched to rect, its outline (edge pixels at 512) grown out of them"""
    f = ImageFont.truetype(os.path.join(FONTS, font), 100)
    l, t, r, b = f.getbbox(text)
    mask = Image.new('L', (r - l + 40, b - t + 40), 0)
    ImageDraw.Draw(mask).text((20 - l, 20 - t), text, font=f, fill=255)
    x0, y0, x1, y1 = mask.getbbox()
    grow = round(edge * (y1 - y0) / (rect[3] - rect[1]))  # the outline's width at size 100
    size = (rect[2] - rect[0] + 2 * edge, rect[3] - rect[1] + 2 * edge)
    box = (x0 - grow, y0 - grow, x1 + grow, y1 + grow)
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    if edge:
        out.paste(Image.new('RGBA', size, EDGE), (0, 0), mask.filter(ImageFilter.MaxFilter(2 * grow + 1)).crop(box).resize(size, Image.LANCZOS))
    out.paste(Image.new('RGBA', size, colour), (0, 0), mask.crop(box).resize(size, Image.LANCZOS))
    return out, (rect[0] - edge, rect[1] - edge)


def main():
    frame = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    draw.rectangle((0, 0, 511, 98), fill=BAND)
    draw.rectangle((0, 406, 511, 511), fill=BAND)
    for text, font, rect, colour, edge in LABELS:
        piece, at = label(text, font, rect, colour, edge)
        frame.alpha_composite(piece, at)
    frame.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vc_icon_frame.png'), optimize=True)


if __name__ == '__main__':
    main()
