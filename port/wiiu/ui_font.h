// The console's own font (OSGetSharedData's standard one, the Wii U Menu's), baked once into an atlas for the options menu, which
// gx2_backend.cpp draws in the Virtual Console's style. Sizes are pixel heights; a text's top is at the y it's given.
#pragma once
#include <gx2/texture.h>

#include "rt.h"

namespace ui_font {
struct Quad {
    float x0, y0, x1, y1, u0, v0, u1, v1;
};
bool load();                                 // (GPU thread) bakes it on the first call; false: the console gave no font
const GX2Texture &texture();                 // white, with the glyphs' coverage as alpha
float width(const char *utf8, float size);   // 0 without the font
int quads(const char *utf8, float size, float x, float y, Quad *out, int max);  // one a glyph it has, left to right
}  // namespace ui_font
