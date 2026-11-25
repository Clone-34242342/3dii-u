// The console's own font for the options menu (ui_font.h): stb_truetype rasterizes the sizes the menu uses into one atlas
#include "ui_font.h"

#include <coreinit/memory.h>
#include <gx2/mem.h>
#include <gx2/event.h>
#include <malloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

namespace ui_font {
namespace {
// the menu's text on the GamePad (480 rows) and on the TV (720): notes, labels, its title; the title's size has no accents
constexpr float kSizes[] = {16, 19, 24, 28, 30, 45};
constexpr int kCount = sizeof kSizes / sizeof kSizes[0], kAtlas = 1024;
constexpr int kExtraRows = 224, kHeight = kAtlas + kExtraRows, kCell = 32, kExtraGlyphs = kAtlas / kCell * (kExtraRows / kCell);
constexpr float kExtraSize = 24;
constexpr float kAccentsBelow = 40;
stbtt_packedchar ascii[kCount][95], latin[kCount][96];  // U+0020-007E, U+00A0-00FF
float ascent[kCount];
GX2Texture atlas;
int state;  // 0 not tried yet, 1 baked, -1 no font
stbtt_fontinfo fonts[OS_SHAREDDATATYPE_FONT_MAX];
bool tried[OS_SHAREDDATATYPE_FONT_MAX];
struct ExtraGlyph {
    u32 cp;
    float ascent;
    stbtt_packedchar glyph;
};
std::vector<u32> page;
std::vector<ExtraGlyph> extra;

const stbtt_fontinfo *face(OSSharedDataType type) {
    auto &info = fonts[type];
    if (!tried[type]) {
        tried[type] = true;
        void *font = nullptr;
        u32 length = 0;
        if (OSGetSharedData(type, 0, &font, &length) && font && length >= 12) {
            const auto *data = static_cast<const u8 *>(font);
            const int offset = stbtt_GetFontOffsetForIndex(data, 0);
            if (offset >= 0 && (u32)offset < length && !stbtt_InitFont(&info, data, offset)) info = {};
        }
    }
    return info.data ? &info : nullptr;
}

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
    if (c < 0xC2 || c > 0xF4) return 0xFFFD;
    const int count = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    int more = count;
    u32 cp = c & (0x3F >> more);
    while (more--) {
        if (((u8)*s & 0xC0) != 0x80) return 0xFFFD;
        cp = cp << 6 | ((u8)*s++ & 0x3F);
    }
    if (cp < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
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
    float pen = 0;
    while (*utf8) {
        const u32 cp = next(utf8);
        if (cp < 0x20) continue;
        int index = 0;
        float pixels = kSizes[i], top = ascent[i];
        const stbtt_packedchar *cd = glyph(i, cp, index);
        if (!cd) {
            const auto at = std::lower_bound(extra.begin(), extra.end(), cp, [](const ExtraGlyph &g, u32 v) { return g.cp < v; });
            if (at != extra.end() && at->cp == cp) {
                cd = &at->glyph;
                pixels = kExtraSize;
                top = at->ascent;
            } else {
                cd = glyph(i, '?', index);
            }
        }
        const float scale = size / pixels;
        stbtt_aligned_quad q;
        float x = pen / scale, y = 0;
        stbtt_GetPackedQuad(cd, kAtlas, kHeight, index, &x, &y, &q, 1);
        f(q, top, scale);
        pen = x * scale;
    }
    f(stbtt_aligned_quad{pen, 0, 0, 0, pen, 0, 0, 0}, -1.0f, 1);
}
}  // namespace

bool load() {
    if (state) return state > 0;
    state = -1;
    const auto *info = face(OS_SHAREDDATATYPE_FONT_STANDARD);
    if (!info) return false;
    const auto *data = info->data;
    u8 *pixels = (u8 *)malloc(kAtlas * kAtlas);
    if (!pixels) return false;
    stbtt_pack_context pc;
    stbtt_pack_range ranges[kCount * 2];
    int n = 0;
    for (int i = 0; i < kCount; i++) {
        ranges[n++] = {kSizes[i], 0x20, nullptr, 95, ascii[i], 0, 0};
        if (kSizes[i] < kAccentsBelow) ranges[n++] = {kSizes[i], 0xA0, nullptr, 96, latin[i], 0, 0};
    }
    if (!stbtt_PackBegin(&pc, pixels, kAtlas, kAtlas, 0, 1, nullptr)) {
        free(pixels);
        return false;
    }
    const bool packed = stbtt_PackFontRanges(&pc, data, 0, ranges, n);
    stbtt_PackEnd(&pc);
    if (!packed) {
        free(pixels);
        return false;
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    for (int i = 0; i < kCount; i++) ascent[i] = asc * stbtt_ScaleForPixelHeight(info, kSizes[i]);
    GX2Surface &s = atlas.surface;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.width = kAtlas;
    s.height = kHeight;
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
    if (s.image) {
        memset(s.image, 0, s.imageSize);
        for (int y = 0; y < kAtlas; y++) memcpy((u8 *)s.image + (size_t)y * s.pitch, pixels + (size_t)y * kAtlas, kAtlas);
    }
    free(pixels);
    if (!s.image) return false;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
    state = 1;
    return true;
}

void prepare(const char *const *texts, size_t count) {
    if (state != 1) return;
    std::vector<u32> wanted;
    for (size_t i = 0; i < count && wanted.size() < kExtraGlyphs; i++) {
        for (const char *s = texts[i]; *s && wanted.size() < kExtraGlyphs;) {
            const u32 cp = next(s);
            if (cp >= 0x100 || (cp >= 0x7F && cp < 0xA0))
                if (std::find(wanted.begin(), wanted.end(), cp) == wanted.end()) wanted.push_back(cp);
        }
    }
    std::sort(wanted.begin(), wanted.end());
    if (wanted == page) return;
    page = std::move(wanted);
    extra.clear();
    if (page.empty()) return;
    auto &surface = atlas.surface;
    auto *pixels = (u8 *)surface.image + (size_t)kAtlas * surface.pitch;
    // Earlier draws may still refer to this part of the atlas.
    GX2DrawDone();
    memset(pixels, 0, (size_t)kExtraRows * surface.pitch);
    constexpr OSSharedDataType order[] = {OS_SHAREDDATATYPE_FONT_STANDARD, OS_SHAREDDATATYPE_FONT_CHINESE,
                                          OS_SHAREDDATATYPE_FONT_TAIWANESE, OS_SHAREDDATATYPE_FONT_KOREAN};
    for (u32 cp : page) {
        for (const auto which : order) {
            const auto *info = face(which);
            const int id = info ? stbtt_FindGlyphIndex(info, cp) : 0;
            if (!id) continue;
            const float scale = stbtt_ScaleForPixelHeight(info, kExtraSize);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(info, id, scale, scale, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            if (w < 0 || h < 0 || w >= kCell || h >= kCell) continue;
            const int x = (int)extra.size() % (kAtlas / kCell) * kCell;
            const int y = (int)extra.size() / (kAtlas / kCell) * kCell;
            if (w && h) stbtt_MakeGlyphBitmap(info, pixels + (size_t)y * surface.pitch + x, w, h, surface.pitch, scale, scale, id);
            int asc, advance;
            stbtt_GetFontVMetrics(info, &asc, nullptr, nullptr);
            stbtt_GetGlyphHMetrics(info, id, &advance, nullptr);
            extra.push_back({cp, asc * scale, {(unsigned short)x, (unsigned short)(kAtlas + y), (unsigned short)(x + w),
                              (unsigned short)(kAtlas + y + h), (float)x0, (float)y0, advance * scale, (float)x1, (float)y1}});
            break;
        }
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, pixels, (size_t)kExtraRows * surface.pitch);
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
