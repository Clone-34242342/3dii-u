// The console's own font for the options menu (ui_font.h): stb_truetype rasterizes the sizes the menu uses into one atlas
#include "ui_font.h"

#include <coreinit/memory.h>
#include <gx2/mem.h>
#include <malloc.h>

#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

namespace ui_font {
namespace {
// the menu's text on the GamePad (480 rows) and on the TV (720): notes, labels, its title; the title's size has no accents
constexpr float kSizes[] = {16, 19, 24, 28, 30, 45};
constexpr int kCount = sizeof kSizes / sizeof kSizes[0], kAtlas = 1024;
constexpr float kAccentsBelow = 40;
stbtt_packedchar ascii[kCount][95], latin[kCount][96];  // U+0020-007E, U+00A0-00FF
float ascent[kCount];
GX2Texture atlas;
int state;  // 0 not tried yet, 1 baked, -1 no font

int nearest(float size) {
    int best = 0;
    for (int i = 1; i < kCount; i++)
        if (std::fabs(kSizes[i] - size) < std::fabs(kSizes[best] - size)) best = i;
    return best;
}

// the next code point of a UTF-8 text
u32 next(const char *&s) {
    const u8 c = (u8)*s++;
    if (c < 0x80) return c;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    u32 cp = c & (0x3F >> more);
    while (more-- && ((u8)*s & 0xC0) == 0x80) cp = cp << 6 | ((u8)*s++ & 0x3F);
    return cp;
}

const stbtt_packedchar *glyph(int i, u32 cp, int &index) {
    if (cp >= 0x20 && cp < 0x7F) {
        index = (int)cp - 0x20;
        return ascii[i];
    }
    if (cp >= 0xA0 && cp <= 0xFF && kSizes[i] < kAccentsBelow) {
        index = (int)cp - 0xA0;
        return latin[i];
    }
    return nullptr;
}

// walks a text's glyphs at the baked size nearest to size, scaled to size
template <class F>
void each(const char *utf8, float size, F &&f) {
    const int i = nearest(size);
    const float scale = size / kSizes[i];
    float x = 0, y = 0;
    while (*utf8) {
        int index;
        const stbtt_packedchar *cd = glyph(i, next(utf8), index);
        if (!cd) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(cd, kAtlas, kAtlas, index, &x, &y, &q, 1);
        f(q, ascent[i], scale);
    }
    f(stbtt_aligned_quad{x, 0, 0, 0, x, 0, 0, 0}, -1.0f, scale);  // the end: where the pen stopped
}
}  // namespace

bool load() {
    if (state) return state > 0;
    state = -1;
    void *font = nullptr;
    u32 font_size = 0;
    if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &font_size) || !font || font_size < 12) return false;
    const u8 *data = (const u8 *)font;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
    u8 *pixels = (u8 *)malloc(kAtlas * kAtlas);
    if (!pixels) return false;
    stbtt_pack_context pc;
    stbtt_pack_range ranges[kCount * 2];
    int n = 0;
    for (int i = 0; i < kCount; i++) {
        ranges[n++] = {kSizes[i], 0x20, nullptr, 95, ascii[i], 0, 0};
        if (kSizes[i] < kAccentsBelow) ranges[n++] = {kSizes[i], 0xA0, nullptr, 96, latin[i], 0, 0};
    }
    const bool packed = stbtt_PackBegin(&pc, pixels, kAtlas, kAtlas, 0, 1, nullptr) && stbtt_PackFontRanges(&pc, data, 0, ranges, n);
    stbtt_PackEnd(&pc);
    if (!packed) {
        free(pixels);
        return false;
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    for (int i = 0; i < kCount; i++) ascent[i] = asc * stbtt_ScaleForPixelHeight(&info, kSizes[i]);
    GX2Surface &s = atlas.surface;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.width = s.height = kAtlas;
    s.depth = s.mipLevels = 1;
    s.format = GX2_SURFACE_FORMAT_UNORM_R8;
    s.aa = GX2_AA_MODE1X;
    s.use = GX2_SURFACE_USE_TEXTURE;
    s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    atlas.viewNumMips = atlas.viewNumSlices = 1;
    atlas.compMap = 0x05050500;  // white, the coverage as alpha
    GX2CalcSurfaceSizeAndAlignment(&s);
    GX2InitTextureRegs(&atlas);
    s.image = memalign(s.alignment, s.imageSize);
    if (s.image)
        for (int y = 0; y < kAtlas; y++) memcpy((u8 *)s.image + (size_t)y * s.pitch, pixels + (size_t)y * kAtlas, kAtlas);
    free(pixels);
    if (!s.image) return false;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
    state = 1;
    return true;
}

const GX2Texture &texture() { return atlas; }

float width(const char *utf8, float size) {
    float w = 0;
    if (state > 0)
        each(utf8, size, [&](const stbtt_aligned_quad &q, float a, float scale) {
            if (a < 0) w = q.x0 * scale;
        });
    return w;
}

int quads(const char *utf8, float size, float x, float y, Quad *out, int max) {
    int n = 0;
    if (state > 0)
        each(utf8, size, [&](const stbtt_aligned_quad &q, float a, float scale) {
            if (a >= 0 && n < max)
                out[n++] = {x + q.x0 * scale, y + (a + q.y0) * scale, x + q.x1 * scale, y + (a + q.y1) * scale, q.s0, q.t0, q.s1, q.t1};
        });
    return n;
}
}  // namespace ui_font
