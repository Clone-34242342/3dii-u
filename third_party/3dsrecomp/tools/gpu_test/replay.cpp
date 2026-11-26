// gpu_replay <dumpdir> <out.png> [-t]
//
// Loads <dumpdir>/fcram.bin (guest VA 0x14000000.., physical FCRAM 0x20000000..) and vram.bin
// (physical 0x18000000), replays <dumpdir>/ops.txt through the software GPU and writes the
// presented framebuffers as PNGs (first present -> out.png, further ones -> out_1.png, ...).
// With -t, the linear output of every display transfer is also written (out_xfer<N>.png).
// ops.txt lines (all numbers hex except present width/height which are decimal):
//   cmdlist <paddr> <size>
//   fill <start> <end> <value> <control>
//   transfer <in> <out> <in_dims> <out_dims> <flags>
//   copy <in> <out> <size> <in_wg> <out_wg> <flags>
//   present <paddr> <width> <height> <stride> <format>
#include <chrono>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>
#include "gpu/gpu.h"
#include "gpu/pica.h"
#include "png.h"
#include "rt.h"
#include "replay_input.h"

static std::string out_name(const std::string &base, const std::string &suffix) {
    size_t dot = base.rfind('.');
    if (dot == std::string::npos) return base + suffix + ".png";
    return base.substr(0, dot) + suffix + base.substr(dot);
}
static void write_fb(const std::string &path, u32 paddr, u32 width, u32 height, u32 stride, u32 fmt) {
    replay::validate({replay::Present, {paddr, width, height, stride, fmt}});
    gpu::flush();
    const u8 *src = mem::paddr_ptr(paddr);
    replay::require(src != nullptr, "present: unmapped framebuffer");
    fmt &= 7;
    const u32 bpp = gpu::pixel_bytes(fmt);
    std::vector<u32> img((size_t)width * height);
    for (u32 l = 0; l < height; l++)
        for (u32 p = 0; p < width; p++)
            img[(size_t)(width - 1 - p) * height + l] = gpu::decode_pixel(fmt, src + (size_t)l * stride + p * bpp) | 0xff000000u;
    replay::require(write_png(path.c_str(), height, width, img.data()), "cannot write " + path);
    fprintf(stderr, "wrote %s (%ux%u)\n", path.c_str(), height, width);
}
static void write_hires(const std::string &path, u32 fb, u32 stride = 0, u32 lines = 0) {
    const u32 *px;
    int w, h;
    if (!gpu::get_display_image_for(fb, stride, lines, &px, &w, &h) &&
        !gpu::get_display_image_for(fb, 0, 0, &px, &w, &h)) return;
    replay::require(px && w > 0 && h > 0 && (u64)w * h <= mem::LINEAR_SIZE / sizeof(u32), "invalid high-resolution image");
    replay::require(write_png(path.c_str(), (u32)w, (u32)h, px), "cannot write " + path);
    fprintf(stderr, "wrote %s (%dx%d)\n", path.c_str(), w, h);
}
static void dispatch(const replay::Op &op) {
    const auto &p = op.p;
    switch (op.kind) {
    case replay::Cmdlist: gpu::process_command_list(p[0], p[1]); break;
    case replay::Fill: gpu::memory_fill(p[0], p[1], p[2], p[3]); break;
    case replay::Transfer: gpu::display_transfer(p[0], p[1], p[2], p[3], p[4]); break;
    case replay::Copy: gpu::texture_copy(p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case replay::Present: break;
    }
}
static int replay_main(int argc, char **argv) {
    replay::require(argc >= 3, "usage: gpu_replay <dumpdir> <out.png> [-t] [-r n] [--trust] [--inval] [--allow-missing-state] [-p presets] [-f addr w h]");
    const std::string dir = argv[1], out = argv[2];
    bool dump_transfers = false, inval = false, trust = false, allow_missing_state = false, final = false;
    int repeat = 1;
    std::string preset;
    u32 final_addr = 0, final_w = 0, final_h = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-t") dump_transfers = true;
        else if (a == "--inval") inval = true;
        else if (a == "--trust") trust = true;
        else if (a == "--allow-missing-state") allow_missing_state = true;
        else if (a == "-r") {
            replay::require(i + 1 < argc, "missing -r value");
            u32 n = replay::number(argv[++i], 10);
            replay::require(n > 0 && n <= INT_MAX, "invalid repeat count"); repeat = (int)n;
        } else if (a == "-p") {
            replay::require(i + 1 < argc, "missing preset path"); preset = argv[++i];
        } else if (a == "-f") {
            replay::require(i + 3 < argc, "missing final framebuffer arguments");
            final_addr = replay::number(argv[++i], 16); final_w = replay::number(argv[++i], 10); final_h = replay::number(argv[++i], 10);
            replay::require(final_w && final_h && final_w <= 0xffff && final_h <= 0xffff, "invalid final dimensions"); final = true;
        } else replay::require(false, "unknown argument: " + a);
    }
    const auto ops = replay::operations(dir + "/ops.txt");
    const auto pre_cmds = preset.empty() ? std::vector<u32>{} : replay::presets(preset);
    if (final) replay::validate({replay::Transfer, {final_addr, 0x27e00000, final_w | final_h << 16, final_w | final_h << 16, 0}});
    replay::exact_size(dir + "/fcram.bin", mem::LINEAR_SIZE);
    replay::exact_size(dir + "/vram.bin", mem::VRAM_SIZE);
    const std::string state = dir + "/gpustate.bin";
    const bool has_state = std::filesystem::exists(state);
    replay::require(has_state || allow_missing_state, "gpustate.bin missing; --allow-missing-state explicitly permits reset state");
    mem::init();
    replay::load_exact(dir + "/fcram.bin", g_mem + mem::LINEAR_BASE, mem::LINEAR_SIZE);
    replay::load_exact(dir + "/vram.bin", g_mem + mem::VRAM_BASE, mem::VRAM_SIZE);
    if (has_state) {
        replay::require(gpu::load_state(state.c_str()), "invalid gpustate.bin");
        fprintf(stderr, "loaded gpustate.bin (native V1, same ABI only)\n");
    } else {
        gpu::init();
        fprintf(stderr, "gpustate.bin absent: explicit reset-state replay\n");
    }
    if (trust) gpu::set_trust_invalidations(true);
    if (!pre_cmds.empty()) {
        memcpy(mem::paddr_ptr(0x27f00000), pre_cmds.data(), pre_cmds.size() * 4);
        gpu::process_command_list(0x27f00000, (u32)pre_cmds.size() * 4);
    }
    // Benchmark warmups intentionally keep state and memory from previous iterations.
    for (int it = 1; it < repeat; ++it) {
        const auto t0 = std::chrono::steady_clock::now();
        double per[4]{};
        for (const auto &op : ops) if (op.kind != replay::Present) {
            const auto start = std::chrono::steady_clock::now(); dispatch(op);
            per[op.kind] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        fprintf(stderr, "iteration %d: %.2f ms (cmdlists %.2f, fills %.2f, transfers %.2f, copies %.2f)\n", it,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), per[0], per[1], per[2], per[3]);
    }
    int presents = 0, transfers = 0;
    double gpu_ms = 0;
    for (const auto &op : ops) {
        const auto &p = op.p;
        if (inval) { gpu::invalidate_memory(mem::VRAM_PADDR, mem::VRAM_SIZE); gpu::invalidate_memory(mem::FCRAM_PADDR, mem::LINEAR_SIZE); }
        const auto t0 = std::chrono::steady_clock::now();
        if (op.kind == replay::Present) {
            const std::string name = presents == 0 ? out : out_name(out, "_" + std::to_string(presents));
            write_fb(name, p[0], p[1], p[2], p[3], p[4]);
            write_hires(out_name(name.substr(0, name.rfind('.')), "_hires"), p[0], p[3], p[2]); ++presents; continue;
        }
        dispatch(op);
        gpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (op.kind == replay::Transfer) {
            if (dump_transfers && !(p[4] & 2) && !(p[4] & 0x20)) {
                const u32 scale = p[4] >> 24 & 3, w = (p[3] & 0xffff) >> (scale != 0), h = (p[3] >> 16) >> (scale == 2), fmt = p[4] >> 12 & 7;
                write_fb(out_name(out, "_xfer" + std::to_string(transfers)), p[1], w, h, w * gpu::pixel_bytes(fmt), fmt);
                write_hires(out_name(out, "_xfer" + std::to_string(transfers) + "_hires"), p[1]);
            }
            ++transfers;
        }
    }
    replay::require(presents > 0, "no framebuffer output written");
    fprintf(stderr, "gpu time: %.2f ms\n", gpu_ms);
    if (final) {
        gpu::display_transfer(final_addr, 0x27e00000, final_w | final_h << 16, final_w | final_h << 16, 0);
        write_fb(out_name(out, "_final"), 0x27e00000, final_w, final_h, final_w * 4, 0);
    }
    return 0;
}
int main(int argc, char **argv) {
    try { return replay_main(argc, argv); }
    catch (const std::exception &e) { fprintf(stderr, "gpu_replay: %s\n", e.what()); return 1; }
}
