// Actual production replay/state/PNG I/O paths; raster/backend effects are deliberately stubbed.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "gpu/pica.h"
#include "gpu/gpu.h"

static bool fail_write, fail_close;
static size_t checked_write(const void *p, size_t size, size_t n, FILE *f) {
    return std::fwrite(p, size, fail_write && n ? n - 1 : n, f);
}
static int checked_close(FILE *f) { int result = std::fclose(f); return fail_close ? EOF : result; }
#define fwrite checked_write
#define fclose checked_close
#ifndef GPU_SOURCE
#define GPU_SOURCE "../third_party/3dsrecomp/runtime/gpu/gpu.cpp"
#endif
#include GPU_SOURCE
#ifndef PNG_SOURCE
#define PNG_SOURCE "../third_party/3dsrecomp/tools/gpu_test/png.cpp"
#endif
#include PNG_SOURCE
#undef fwrite
#undef fclose
#define main replay_cli_main
#ifndef REPLAY_SOURCE
#define REPLAY_SOURCE "../third_party/3dsrecomp/tools/gpu_test/replay.cpp"
#endif
#include REPLAY_SOURCE
#undef main
#ifndef REPLAY_INPUT_SOURCE
#define REPLAY_INPUT_SOURCE "../third_party/3dsrecomp/tools/gpu_test/replay_input.h"
#endif
#include REPLAY_INPUT_SOURCE

u8 *g_mem;
int g_verbose;
u64 g_frame;
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { std::abort(); }
namespace mem {
void init() {
    if (g_mem) return;
    g_mem = (u8 *)VirtualAlloc(nullptr, 0x20000000, MEM_RESERVE, PAGE_NOACCESS);
    assert(g_mem);
    assert(VirtualAlloc(g_mem + LINEAR_BASE, LINEAR_SIZE, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(g_mem + VRAM_BASE, VRAM_SIZE, MEM_COMMIT, PAGE_READWRITE));
}
u8 *paddr_ptr(u32 a) {
    if (a >= FCRAM_PADDR && (u64)a < (u64)FCRAM_PADDR + LINEAR_SIZE) return g_mem + LINEAR_BASE + (a - FCRAM_PADDR);
    if (a >= VRAM_PADDR && (u64)a < (u64)VRAM_PADDR + VRAM_SIZE) return g_mem + VRAM_BASE + (a - VRAM_PADDR);
    return nullptr;
}
}
namespace gpu {
std::atomic<u64> g_sh_instr{0}, g_sh_runs{0};
static unsigned fills, transfers, copies;
void raster_flush() {}
void raster_init() {}
void metal_writeback_all() {}
void metal_poll() {}
void texcache_invalidate(u32, u32) {}
void texcache_new_epoch() {}
void texcache_trim() {}
bool vtx_jobs_pending() { return false; }
void vtx_finish_jobs() {}
void vtx_draw(bool) { assert(false); }
void vtx_immediate_vertex() { assert(false); }
void vtx_reset_immediate() { g.imm_attr = g.imm_n = 0; }
void pa_reset() { g.pa.index = 0; g.pa.strip_ready = g.pa.winding = false; }
void pa_reconfigure(u32 t) { pa_reset(); g.pa.topology = t; }
void memory_fill(u32, u32, u32, u32) { ++fills; }
void display_transfer(u32, u32, u32, u32, u32) { ++transfers; }
void texture_copy(u32, u32, u32, u32, u32, u32) { ++copies; }
void invalidate_memory(u32, u32) {}
void set_trust_invalidations(bool) {}
bool get_display_image_for(u32, u32, u32, const u32 **, int *, int *) { return false; }
}

namespace fs = std::filesystem;
static unsigned checks;
static std::vector<u8> bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary); assert(f.is_open());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void put(const fs::path &p, const std::vector<u8> &d) {
    std::ofstream f(p, std::ios::binary); f.write((const char *)d.data(), d.size()); assert(f.good());
}
static void text(const fs::path &p, const std::string &s) { put(p, {s.begin(), s.end()}); }
static int cli(const fs::path &dir, const fs::path &out, std::vector<std::string> extra = {}) {
    std::vector<std::string> args = {"gpu_replay", dir.string(), out.string()}; args.insert(args.end(), extra.begin(), extra.end());
    std::vector<char *> argv; for (auto &s : args) argv.push_back(s.data());
    return replay_cli_main((int)argv.size(), argv.data());
}
static void state_checks(const fs::path &dir) {
    gpu::init(); gpu::g.vs.code[0] = 0x12345678; gpu::g.vs.code_size = 1;
    const fs::path valid = dir / "valid.bin", bad = dir / "bad.bin";
    assert(gpu::save_state(valid.string().c_str())); const auto good = bytes(valid);
    gpu::g.vs.code[0] = 0x87654321; gpu::g.vs.dirty = false;
    auto rejected = [&](std::vector<u8> changed) {
        put(bad, changed);
        const std::vector<u8> live((u8 *)&gpu::g, (u8 *)&gpu::g + sizeof(gpu::g));
        assert(!gpu::load_state(bad.string().c_str()));
        assert(!memcmp(live.data(), &gpu::g, live.size())); ++checks;
    };
    auto short_file = good; short_file.resize(good.size() - 1); rejected(short_file); // red: old loader partially overwrites g
    auto changed = good; changed.push_back(0); rejected(changed); // old loader ignored trailing data
    changed = good; changed[0] ^= 1; rejected(changed);
    changed = good; changed[4] ^= 1; rejected(changed);
    fail_close = true;
    const std::vector<u8> before_close((u8 *)&gpu::g, (u8 *)&gpu::g + sizeof(gpu::g));
    assert(!gpu::load_state(valid.string().c_str()) && !memcmp(before_close.data(), &gpu::g, before_close.size()));
    fail_close = false; ++checks;
    auto offset = [&](const void *field) {
        size_t at = 8, found = SIZE_MAX;
        gpu::state_fields([&](void *p, size_t n) {
            if ((uintptr_t)field >= (uintptr_t)p && (uintptr_t)field - (uintptr_t)p < n) found = at + (uintptr_t)field - (uintptr_t)p;
            at += n;
        }); assert(found != SIZE_MAX); return found;
    };
    auto invalid_integer = [&](const u32 *field, u32 value) {
        auto d = good; memcpy(d.data() + offset(field), &value, 4); rejected(d);
    };
    for (const gpu::ShaderSetup *s : {&gpu::g.vs, &gpu::g.gs}) {
        invalid_integer(&s->queue_n, 4); invalid_integer(&s->code_size, 4097); invalid_integer(&s->swizzle_size, 4097);
        for (const bool &b : s->b) { auto d = good; d[offset(&b)] = 2; rejected(d); }
    }
    invalid_integer(&gpu::g.imm_n, 3); invalid_integer(&gpu::g.imm_attr, 16);
    invalid_integer(&gpu::g.gs_unit.out_bank, 1); invalid_integer(&gpu::g.pa.topology, 4); invalid_integer(&gpu::g.pa.index, 3);
    for (const bool *b : {&gpu::g.imm_reset_gs, &gpu::g.gs_unit.cond[0], &gpu::g.gs_unit.cond[1], &gpu::g.pa.strip_ready, &gpu::g.pa.winding}) {
        auto d = good; d[offset(b)] = 0xff; rejected(d);
    }
    changed = good; u32 one = 1, two = 2; memcpy(changed.data() + offset(&gpu::g.pa.topology), &one, 4);
    memcpy(changed.data() + offset(&gpu::g.pa.index), &two, 4); rejected(changed);
    assert(gpu::load_state(valid.string().c_str()) && gpu::g.vs.code[0] == 0x12345678); ++checks;
    assert(gpu::save_state((dir / "roundtrip.bin").string().c_str()) && bytes(dir / "roundtrip.bin") == good); ++checks;
    for (gpu::ShaderSetup *s : {&gpu::g.vs, &gpu::g.gs}) {
        s->queue_n = 3; s->code_size = s->swizzle_size = 4096;
    }
    gpu::g.imm_n = 2; gpu::g.imm_attr = 15; gpu::g.gs_unit.out_bank = 16;
    gpu::g.pa.topology = 3; gpu::g.pa.index = 2;
    assert(gpu::save_state((dir / "boundary.bin").string().c_str()));
    assert(gpu::load_state((dir / "boundary.bin").string().c_str()));
    assert(gpu::load_state(valid.string().c_str())); ++checks;
    assert(!gpu::save_state((dir / "missing/state.bin").string().c_str())); ++checks;
    fail_write = true; assert(!gpu::save_state((dir / "write_fail.bin").string().c_str())); fail_write = false; ++checks;
    fail_close = true; assert(!gpu::save_state((dir / "close_fail.bin").string().c_str())); fail_close = false; ++checks;
    const u32 pixel = 0xff804020;
    fail_write = true; assert(!write_png((dir / "png_write_fail.png").string().c_str(), 1, 1, &pixel)); fail_write = false; ++checks;
    fail_close = true; assert(!write_png((dir / "png_close_fail.png").string().c_str(), 1, 1, &pixel)); fail_close = false; ++checks;
}
static void replay_checks(const fs::path &dir) {
    const auto fcram = dir / "fcram.bin", vram = dir / "vram.bin", ops = dir / "ops.txt", state = dir / "gpustate.bin", out = dir / "out.png";
    text(fcram, ""); fs::resize_file(fcram, mem::LINEAR_SIZE); text(vram, ""); fs::resize_file(vram, mem::VRAM_SIZE);
    assert(gpu::save_state(state.string().c_str())); const auto good_state = bytes(state);
    const std::string present = "present 20000000 2 2 8 0\n";
    auto reject = [&](const std::string &s, std::vector<std::string> args = {}) {
        text(ops, s); assert(cli(dir, out, args) != 0); ++checks;
    };
    for (const auto &s : {"", "unknown 1\n", "present 20000000 2 2 8\n", "present 20000000 2 2 8 0 extra\n",
        "present 20000000 -2 2 8 0\n", "present 20000000 2x 2 8 0\n", "present 20000000 4294967296 2 8 0\n",
        "present 20000000 0 2 8 0\n", "present 20000000 2 2 7 0\n", "present 20000000 2 2 8 7\n",
        "present fffffffc 2 2 8 0\n", "present 27fffffc 2 2 8 0\n", "cmdlist 20000000 8\n"}) reject(s);
    for (const auto &s : {"cmdlist 20000001 8\n", "cmdlist 20000000 4\n", "fill 20000010 20000000 0 1\n",
        "transfer 20000000 20000100 20002 20002 500\n", "transfer 20000000 20000100 0 20002 0\n",
        "transfer 20000000 20000100 20002 40004 0\n", "transfer 20000000 20000100 20002 20002 3000000\n",
        "copy 20000000 20000100 11 0 0 0\n", "copy 20000000 20000100 20 10000 0 0\n",
        "copy 27fffff0 20000100 20 0 0 0\n"}) reject(std::string(s) + present);
    text(ops, std::string(512, 'a')); assert(cli(dir, out) != 0); ++checks;
    text(ops, ""); fs::resize_file(ops, (16u << 20) + 1); assert(cli(dir, out) != 0); ++checks;
    text(ops, present);
    for (const auto &file : {fcram, vram, ops}) {
        const auto held = fs::path(file.string() + ".held");
        fs::rename(file, held); assert(cli(dir, out) != 0); fs::rename(held, file); ++checks;
    }
    for (const auto &args : {std::vector<std::string>{"--unknown"}, {"-r"}, {"-r", "0"}, {"-r", "-1"}, {"-r", "1x"}, {"-p"}, {"-f", "20000000", "2"}}) {
        assert(cli(dir, out, args) != 0); ++checks;
    }
    for (auto file : {fcram, vram}) {
        const auto size = fs::file_size(file);
        for (auto n : {0ull, (unsigned long long)size - 1, (unsigned long long)size + 1}) {
            fs::resize_file(file, n); assert(cli(dir, out) != 0); ++checks;
        }
        fs::resize_file(file, size);
    }
    fs::remove(state); assert(cli(dir, out) != 0); ++checks;
    assert(cli(dir, out, {"--allow-missing-state"}) == 0 && fs::file_size(out) > 8); ++checks;
    put(state, {1, 2, 3}); assert(cli(dir, out, {"--allow-missing-state"}) != 0); ++checks;
    put(state, good_state); assert(cli(dir, dir / "missing/out.png") != 0); ++checks;
    const auto preset = dir / "presets.txt"; text(preset, "reg 300 0\n"); assert(cli(dir, out, {"-p", preset.string()}) != 0); ++checks;
    text(preset, "reg 0 0 extra\n"); assert(cli(dir, out, {"-p", preset.string()}) != 0); ++checks;
    // Three passes upload one f32 uniform word each. A reset between warmups would leave queue_n=1, not3.
    gpu::g.vs.queue_n = 0; gpu::g.regs[gpu::R::VS + gpu::R::SH_FLOAT_IDX] = 0x80000000;
    assert(gpu::save_state(state.string().c_str()));
    const u32 command[2] = {0x3f800000, gpu::R::VS + gpu::R::SH_FLOAT_DATA | 0xfu << 16};
    { std::fstream f(fcram, std::ios::binary | std::ios::in | std::ios::out); f.seekp(0x1000); f.write((const char *)command, sizeof command); }
    text(ops, "cmdlist 20001000 8\nfill 20000100 20000110 0 1\ntransfer 20000000 20000100 80008 80008 0\ncopy 20000000 20000100 20 0 0 0\n" + present);
    gpu::fills = gpu::transfers = gpu::copies = 0;
    assert(cli(dir, out, {"-r", "3"}) == 0 && gpu::g.vs.queue_n == 3);
    assert(gpu::fills == 3 && gpu::transfers == 3 && gpu::copies == 3); ++checks;
}
int main(int argc, char **argv) {
    _putenv("GPU_THREADS=1");
    assert(argc == 2 || (argc == 3 && std::string(argv[1]) == "--replay-only") || (argc == 4 && std::string(argv[1]) == "--preflight"));
    const fs::path own = argc == 2 ? argv[1] : argc == 3 ? argv[2] : argv[3]; fs::create_directories(own);
    mem::init();
    if (argc == 4) {
        const fs::path capture = argv[2];
        const auto ops = replay::operations((capture / "ops.txt").string());
        replay::load_exact((capture / "fcram.bin").string(), g_mem + mem::LINEAR_BASE, mem::LINEAR_SIZE);
        replay::load_exact((capture / "vram.bin").string(), g_mem + mem::VRAM_BASE, mem::VRAM_SIZE);
        assert(gpu::load_state((capture / "gpustate.bin").string().c_str()));
        assert(gpu::save_state((own / "state_roundtrip.bin").string().c_str()));
        assert(bytes(capture / "gpustate.bin") == bytes(own / "state_roundtrip.bin"));
        printf("real capture preflight: %zu operations, nativeV1 state roundtrip exact; no draw dispatched\n", ops.size()); return 0;
    }
    if (argc != 3) state_checks(own);
    replay_checks(own);
    printf("actual replay/state/PNG validation: %u positive/negative controls passed\n", checks);
}
