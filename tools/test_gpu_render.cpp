// One synthetic PICA command fixture, shared verbatim by the host and PPC builds.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "gpu/gpu.h"
#include "gpu/pica.h"
#include "gpu_iface.h"
#ifdef __WIIU__
#include <coreinit/cache.h>
#include <gx2/event.h>
#include "wiiu.h"
void replay_target_init();
bool replay_target_counters(FILE *file);
void replay_target_finish();
#endif

using namespace gpu;
namespace {
constexpr u32 CB = 0x18000000, DB = 0x18100000, BOTTOM = 0x18200000;
constexpr u32 TEX = 0x18300000, CL = 0x20100000, VB = 0x20200000, OUT = 0x20400000;
constexpr u32 W = 240, H = 400;
struct Pixel { u8 r, g, b, a; };
static_assert(sizeof(Pixel) == 4);
constexpr Pixel background{16, 32, 48, 255};
constexpr Pixel texture_quadrants[] = {
    {255, 0, 0, 255}, {0, 255, 0, 255},
    {0, 0, 255, 255}, {255, 255, 0, 255},
};

void need(bool condition, const char *why) {
    if (!condition) FATAL("REPLAY FAIL: %s\n", why);
}

void write_file(const std::string &path, const void *bytes, size_t count) {
    FILE *file = fopen(path.c_str(), "wb");
    need(file != nullptr, "open output");
    const size_t written = fwrite(bytes, 1, count, file);
    const int closed = fclose(file);
    need(written == count && !closed, "short output");
}

// Explicit LE words make the fixture identical on x86 and big-endian PPC.
void put(std::vector<u8> &bytes, u32 value) {
    for (int i = 0; i < 4; i++) bytes.push_back((u8)(value >> (8 * i)));
}
void number(std::vector<u8> &bytes, float value) {
    u32 word;
    memcpy(&word, &value, 4);
    put(bytes, word);
}
u32 f24_bits(float value) {
    if (value == 0) return std::signbit(value) ? 0x800000 : 0;
    u32 bits;
    memcpy(&bits, &value, 4);
    return (bits >> 31) << 23 | (((bits >> 23 & 255) - 127 + 63) << 16) | ((bits & 0x7fffff) >> 7);
}
struct Commands {
    std::vector<u8> bytes;
    void reg(u32 id, u32 value) { put(bytes, value); put(bytes, id | 0xfu << 16); }
};

void setup(Commands &c) {
    c.reg(R::VS + R::SH_CODE_IDX, 0);
    for (u32 i = 0; i < 3; i++) c.reg(R::VS + R::SH_CODE_DATA, 0x13u << 26 | i << 21 | i << 12);
    c.reg(R::VS + R::SH_CODE_DATA, 0x88000000);
    c.reg(R::VS + R::SH_SWZ_IDX, 0);
    // Full destination mask and identity source selectors, derived from the PICA descriptor fields.
    c.reg(R::VS + R::SH_SWZ_DATA, 0xf | 0x1bu << 5 | 0x1bu << 14 | 0x1bu << 23);
    c.reg(R::VS + R::SH_ENTRY, 0);
    c.reg(R::VS + R::SH_INPUT_CFG, 2 | 0xa0u << 24);
    c.reg(R::VS + R::SH_PERM_LO, 0x210);
    c.reg(R::VS + R::SH_PERM_HI, 0);
    c.reg(R::VS + R::SH_OUTMASK, 7);
    c.reg(R::VS_NUM_ATTR, 2);
    c.reg(R::VS_OUT_TOTAL, 3);
    c.reg(R::VS_OUT_MAP, 0x03020100);
    c.reg(R::VS_OUT_MAP + 1, 0x0b0a0908);
    c.reg(R::VS_OUT_MAP + 2, 0x1f1f0d0c);
    c.reg(R::CULL, 0);
    c.reg(R::VP_W, f24_bits(W / 2.0f));
    c.reg(R::VP_H, f24_bits(H / 2.0f));
    c.reg(R::VP_CORNER, 0);
    c.reg(R::DEPTHMAP_EN, 1);
    c.reg(R::DEPTH_SCALE, f24_bits(-1));
    c.reg(R::DEPTH_OFFSET, 0);
    c.reg(R::TEX_CONFIG, 0);
    c.reg(R::LIGHTING_DISABLE, 1);
    c.reg(R::TEV0, 0);
    c.reg(R::TEV0 + 1, 0);
    c.reg(R::TEV0 + 2, 0);
    c.reg(R::TEV0 + 3, 0);
    c.reg(R::TEV0 + 4, 0);
    for (u32 base : {R::TEV1, R::TEV2, R::TEV3, R::TEV4, R::TEV5}) {
        c.reg(base, 0x000f000f);
        for (u32 i = 1; i <= 4; i++) c.reg(base + i, 0);
    }
    c.reg(R::OM_MODE, 0);
    c.reg(R::LOGIC_OP, 3);
    c.reg(R::BLEND, 0x11110000);
    c.reg(R::ALPHA_TEST, 0);
    c.reg(R::STENCIL_TEST, 0);
    c.reg(R::DEPTH_COLOR_MASK, 0xf00);
    c.reg(R::COLOR_WRITE, 0xf);
    c.reg(R::DEPTH_WRITE, 0);
    c.reg(R::COLOR_FMT, 0);
    c.reg(R::COLOR_ADDR, CB / 8);
    c.reg(R::DEPTH_ADDR, DB / 8);
    c.reg(R::DEPTH_FMT, 3);
    c.reg(R::FB_DIM, W | (H - 1) << 12);
    c.reg(R::SCISSOR_MODE, 0);
    c.reg(R::GS_MISC, 0);
    c.reg(R::GS_EXCLUSIVE, 0);
    c.reg(R::TOPOLOGY, 0);
    c.reg(R::ATTR_BASE, VB / 8);
    c.reg(R::ATTR_FMT_LO, 0xfff);  // Three float4 attributes: position, color, UV.
    c.reg(R::ATTR_FMT_HI, 2u << 28);
    c.reg(R::ATTR_LOADER, 0);
    c.reg(R::ATTR_LOADER + 1, 0x210);
    c.reg(R::ATTR_LOADER + 2, 48 << 16 | 3u << 28);
}

void quad(Commands &c, std::vector<u8> &vertices, float z, const std::array<float, 4> &color) {
    const u32 first = (u32)vertices.size() / 48;
    constexpr float xy[4][2] = {{-0.75f, -0.75f}, {0.75f, -0.75f}, {-0.75f, 0.75f}, {0.75f, 0.75f}};
    for (int k : {0, 1, 2, 2, 1, 3}) {
        for (float v : {xy[k][0], xy[k][1], z, 1.0f}) number(vertices, v);
        for (float v : color) number(vertices, v);
        for (float v : {(xy[k][0] + 0.75f) / 1.5f, (xy[k][1] + 0.75f) / 1.5f, 0.0f, 1.0f}) number(vertices, v);
    }
    c.reg(R::VERTEX_OFFSET, first);
    c.reg(R::NUM_VERTICES, 6);
    c.reg(R::RESTART_PRIM, 1);
    c.reg(R::DRAW, 1);
}

void immediate_quad(Commands &c, float z, const std::array<float, 4> &color) {
    c.reg(R::RESTART_PRIM, 1);
    c.reg(R::FIXATTR_INDEX, 15);
    constexpr float xy[4][2] = {{-0.75f, -0.75f}, {0.75f, -0.75f}, {-0.75f, 0.75f}, {0.75f, 0.75f}};
    const auto attribute = [&](const std::array<float, 4> &v) {
        const u32 x = f24_bits(v[0]), y = f24_bits(v[1]), z = f24_bits(v[2]), w = f24_bits(v[3]);
        c.reg(R::FIXATTR_DATA, w << 8 | z >> 16);
        c.reg(R::FIXATTR_DATA + 1, z << 16 | y >> 8);
        c.reg(R::FIXATTR_DATA + 2, y << 24 | x);
    };
    for (int k : {0, 1, 2, 2, 1, 3}) {
        attribute({xy[k][0], xy[k][1], z, 1});
        attribute(color);
        attribute({(xy[k][0] + 0.75f) / 1.5f, (xy[k][1] + 0.75f) / 1.5f, 0, 1});
    }
}

u64 checksum(const void *data, size_t size) {
    u64 hash = 14695981039346656037ull;
    const auto *bytes = (const u8 *)data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

bool output(const std::string &folder, const char *name, u32 input, u32 height, Pixel inside,
            bool has_quad, bool textured, FILE *report) {
    gpu::display_transfer(input, OUT, W | height << 16, W | height << 16, 0);
#ifdef __WIIU__
    gpu::gx2_cpu_will_read(OUT, W * height * 4);
    GX2DrawDone();
    DCInvalidateRange(mem::paddr_ptr(OUT), W * height * 4);
#else
    gpu::flush();
#endif
    const u8 *source = mem::paddr_ptr(OUT);
    std::vector<Pixel> pixels((size_t)W * height);
    u32 wrong = 0;
    for (u32 line = 0; line < height; line++) {
        for (u32 p = 0; p < W; p++) {
            const u8 *raw = source + ((size_t)line * W + p) * 4;
            const Pixel actual{raw[3], raw[2], raw[1], raw[0]};
            pixels[(size_t)(W - 1 - p) * height + line] = actual;
            // PICA clip-space quad covers x=[30,210), y=[50,350). All outer edges
            // lie between pixel centers. The shared diagonal has no effect on the union.
            const u32 y = height - 1 - line;
            const bool covered = has_quad && p >= 30 && p < 210 && y >= 50 && y < 350;
            Pixel expected = background;
            if (covered) {
                // The upper half of PICA V addresses the first rows in texture memory.
                expected = textured ? texture_quadrants[(p >= 120) + 2 * (y < 200)] : inside;
            }
            if (memcmp(&actual, &expected, 4)) {
                if (wrong < 4) LOG("%s pixel(%u,%u): got %u,%u,%u,%u expected %u,%u,%u,%u\n", name, p, y,
                    actual.r, actual.g, actual.b, actual.a, expected.r, expected.g, expected.b, expected.a);
                wrong++;
            }
        }
    }
    write_file(folder + "/" + name + ".rgba", pixels.data(), pixels.size() * sizeof(Pixel));
    fprintf(report, "{\"name\":\"%s\",\"width\":%u,\"height\":%u,\"wrong_pixels\":%u,\"fnv64\":\"%016llx\"}",
            name, height, W, wrong, (unsigned long long)checksum(pixels.data(), pixels.size() * 4));
    LOG("REPLAY frame %s: %u wrong pixels\n", name, wrong);
    return wrong == 0;
}
}  // namespace

int main(int argc, char **argv) {
#ifdef __WIIU__
    replay_target_init();  // All GX2 initialization, execution and readback stay on this thread.
    const std::string folder = wiiu::data_dir();
#else
    if (argc != 2) { fprintf(stderr, "usage: graphics_replay <existing-output-directory>\n"); return 2; }
    const std::string folder = argv[1];
#endif
    mem::init();
    gpu::init();
    FILE *report = fopen((folder + "/result.json").c_str(), "wb");
    need(report != nullptr, "open result");
    fprintf(report, "{\"version\":1,\"frames\":[");
    bool okay = true;
    for (int scene = 0; scene < 5; scene++) {
        const char *name = std::array<const char *, 5>{"clear", "triangle", "depth", "texture", "blend"}[scene];
        Commands c;
        std::vector<u8> vertices;
        setup(c);
        Pixel inside = background;
        if (scene == 1) {
            immediate_quad(c, -0.5f, {1, 0, 0, 1});
            inside = {255, 0, 0, 255};
        } else if (scene == 2) {
            c.reg(R::DEPTH_WRITE, 3);
            c.reg(R::DEPTH_COLOR_MASK, 1 | 4 << 4 | 0xf00 | 1 << 12);
            quad(c, vertices, -0.75f, {1, 0, 0, 1});
            quad(c, vertices, -0.25f, {0, 1, 0, 1});
            quad(c, vertices, -0.5f, {0, 0, 1, 1});
            inside = {0, 255, 0, 255};
        } else if (scene == 3) {
            c.reg(R::TEV0, 3 | 3 << 16);
            c.reg(R::TEX_CONFIG, 1);
            c.reg(R::TEX0 + 1, 8 | 8 << 16);
            c.reg(R::TEX0 + 2, 2 << 8 | 2 << 12);
            c.reg(R::TEX0 + 3, 0);
            c.reg(R::TEX0 + 4, TEX / 8);
            c.reg(R::TEX0_TYPE, 0);
            std::array<u8, 256> texture;
            for (u32 i = 0; i < 64; i++) {
                // The two high Morton bits select a 4x4 quadrant. PICA stores ABGR bytes.
                const Pixel color = texture_quadrants[i / 16];
                texture[i * 4] = color.a;
                texture[i * 4 + 1] = color.b;
                texture[i * 4 + 2] = color.g;
                texture[i * 4 + 3] = color.r;
            }
            memcpy(mem::paddr_ptr(TEX), texture.data(), texture.size());
            gpu::dma_written(TEX, (u32)texture.size());
            write_file(folder + "/texture.bin", texture.data(), texture.size());
            quad(c, vertices, -0.5f, {1, 1, 1, 1});
        } else if (scene == 4) {
            c.reg(R::OM_MODE, 0x100);
            quad(c, vertices, -0.5f, {0.25f, 0.125f, 0.0625f, 1});
            inside = {80, 64, 64, 255};
        }
        need(!c.bytes.empty() && c.bytes.size() < 0x10000 && vertices.size() < 0x10000, "fixture bounds");
        memcpy(mem::paddr_ptr(CL), c.bytes.data(), c.bytes.size());
        memcpy(mem::paddr_ptr(VB), vertices.data(), vertices.size());
        gpu::dma_written(VB, (u32)vertices.size());
        write_file(folder + "/" + name + "_commands.bin", c.bytes.data(), c.bytes.size());
        write_file(folder + "/" + name + "_vertices.bin", vertices.data(), vertices.size());
        gpu::memory_fill(CB, CB + W * H * 4, 0x102030ff, 0x201);
        gpu::memory_fill(DB, DB + W * H * 4, 0x00ffffff, 0x201);
        gpu::process_command_list(CL, (u32)c.bytes.size());
#ifdef __WIIU__
        gpu::gx2_finish_list();
#endif
        if (scene) fprintf(report, ",");
        okay &= output(folder, name, CB, H, inside, scene != 0, scene == 3, report);
    }
    gpu::memory_fill(BOTTOM, BOTTOM + W * 320 * 4, 0x102030ff, 0x201);
    fprintf(report, ",");
    okay &= output(folder, "bottom_clear", BOTTOM, 320, background, false, false, report);
    fprintf(report, "],\"counters\":");
#ifdef __WIIU__
    okay &= replay_target_counters(report);
#else
    fprintf(report, "{\"backend\":\"software\"}");
#endif
    fprintf(report, ",\"pass\":%s}\n", okay ? "true" : "false");
    need(fclose(report) == 0, "close result");
    LOG("REPLAY COMPLETE: %s; six frames required\n", okay ? "PASS" : "FAIL");
#ifdef __WIIU__
    replay_target_finish();
#endif
    return okay ? 0 : 1;
}

#ifndef __WIIU__
u64 g_frame = 0;
#endif
