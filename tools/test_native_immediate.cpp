// Actual extracted batching, upload barriers, latch mapping and primitive assembly.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gpu/pica.h"
int g_verbose;
void rt_log(const char *, ...) {}
namespace gpu {
GpuState g;
bool g_trace;
static DrawState test_state;
static bool native_accept = true;
static u32 job_finishes, native_calls, gs_calls;
static std::vector<std::array<V4, 16>> cpu_inputs, native_inputs;
static std::vector<std::array<OutVertex, 3>> triangles;
static std::vector<void *> allocations;
static float native_uniform;
static u32 native_code;
static bool native_bool;
struct OutMap { s8 slot_src[24]; };
struct { bool active; } gsp;
const DrawState *cur_ds;
void vtx_finish_jobs() { ++job_finishes; }
DrawState *draw_state() { return &test_state; }
void gs_reconfigure() { gsp.active = (g.regs[R::GS_MISC] & 3) == 2; }
void gs_submit_vertex(const V4 *) { ++gs_calls; }
const ShaderProgram *shader_get_program(ShaderSetup &) { return nullptr; }
#include "shader_init.inc"
void shader_run(const ShaderProgram *, ShaderUnit &u, const ShaderSetup &, u32) {
    std::array<V4, 16> input;
    memcpy(input.data(), &u.r[REG_IN], sizeof input);
    cpu_inputs.push_back(input);
    u.r[REG_OUT] = u.r[REG_IN];
    u.r[REG_OUT].v[0] += u.r[REG_UNI].v[0];
}
void raster_triangle(const DrawState *, const OutVertex &a, const OutVertex &b, const OutVertex &c) {
    triangles.push_back({a, b, c});
}
struct Stream { u32 location, buffer; bool instance; };
struct { u32 inputs = 0xffff; s32 location[16]; } vs;
struct { Stream s[16]; u32 count; } fk;
std::array<std::vector<V4>, 16> bound;
void *ring_alloc(u32 size, u32) { void *p = std::malloc(size); allocations.push_back(p); return p; }
void DCFlushRange(void *, u32) {}
void GX2SetAttribBuffer(u32 buffer, u32 size, u32 stride, const void *p) {
    if (stride != sizeof(V4) || size % stride) std::abort();
    const V4 *v = (const V4 *)p;
    bound[buffer].assign(v, v + size / stride);
}
Stream float4_stream(u32 location, u32 buffer, u32, bool instance) { return {location, buffer, instance}; }
bool gx2_draw(const Gx2Draw &d, const V4 *immediate) {
    if (!native_accept) return false;
    ++native_calls;
    native_uniform = g.vs.f[0].v[0]; native_code = g.vs.code[0]; native_bool = g.vs.b[0];
    if (!job_finishes || d.count % 3 || d.first || d.indexed || d.gs || d.num_attr != 16) std::abort();
    fk.count = 0;
    u32 buf = 0;
#include "native_mapping.inc"
    for (u32 r = 0; r < 16; r++) {
        if (!(vs.inputs & (1u << r)) || vs.location[r] < 0) continue;
        const s32 a = reg_attr[r];
#include "native_stream.inc"
    }
    for (u32 vertex = 0; vertex < d.count; vertex++) {
        std::array<V4, 16> input{};
        for (u32 i = 0; i < fk.count; i++) {
            if (fk.s[i].instance) std::abort();
            input[fk.s[i].location] = bound[fk.s[i].buffer][vertex];
        }
        native_inputs.push_back(input);
    }
    return true;
}
#include "vertex_production.inc"

// Unrelated register side effects are controlled; real uniform/code upload and barriers are retained.
bool special[0x300];
u8 run_kind[0x300];
enum { RUN_NONE, RUN_VS_UNIFORMS, RUN_VS_CODE, RUN_VS_SWIZZLE, RUN_LIGHT_TABLE };
void light_table_word(u32) {}
void vs_uniform_word(u32);
static void submit_immediate(u32);
void handle_special(u32 id, u32 value) {
    if (id == R::VS + R::SH_BOOL) for (u32 i = 0; i < 16; i++) g.vs.b[i] = value >> i & 1;
    else if (id >= R::VS + R::SH_FLOAT_DATA && id < R::VS + R::SH_FLOAT_DATA + 8) vs_uniform_word(value);
    else if (id >= R::FIXATTR_DATA && id < R::FIXATTR_DATA + 3) submit_immediate(value);
    else if (id == R::FIXATTR_INDEX) vtx_reset_immediate();
    else if (id == R::RESTART_PRIM) pa_reset();
    else if (id == R::TOPOLOGY) { g.pa.index = 0; g.pa.strip_ready = g.pa.winding = false; g.pa.topology = value >> 8 & 3; }
}
#include "write_production.inc"
}

static int checks, failures;
static void expect(bool okay, const char *name) { ++checks; if (!okay) { ++failures; std::printf("FAIL: %s\n", name); } }
using namespace gpu;
static u8 memory[256];
static std::vector<u32> commands;
u8 *g_mem;
namespace mem { u8 *paddr_ptr(u32 address) { return address == 0x1000 ? (u8 *)commands.data() : memory; } }
static void reset() {
    for (void *p : allocations) std::free(p);
    allocations.clear();
    g = GpuState{}; test_state = DrawState{}; test_state.rt.color_addr = 1;
    cpu_inputs.clear(); native_inputs.clear(); triangles.clear();
    native_accept = true; job_finishes = native_calls = gs_calls = 0;
    vtx_immediate_cpu_runs = 0;
    gsp.active = false; cur_ds = &test_state;
    g.imm_reset_gs = true; g.vs.f[0].v[0] = 10;
    g.regs[R::VS + R::SH_OUTMASK] = 1;
    g.regs[R::VS_OUT_TOTAL] = 1;
    g.regs[R::VS_OUT_MAP] = 0x03020100;
    g.regs[R::FIXATTR_INDEX] = 15;
    for (int i = 0; i < 16; i++) vs.location[i] = i;
    commands.clear();
    std::fill(special, special + 0x300, false);
    std::fill(run_kind, run_kind + 0x300, 0);
    init_special();
}
static void vertex(float x) { g.imm_vertex[0] = V4{{x, 0, 0, 1}}; vtx_immediate_vertex(); }
static void uniform(float x, bool fast) {
    u32 bits; memcpy(&bits, &x, 4);
    g.regs[R::VS + R::SH_FLOAT_IDX] = 0x80000000;
    if (fast) { const u32 rest[] = {le32(0), le32(0), le32(bits)}; write_run(RUN_VS_UNIFORMS, R::VS + R::SH_FLOAT_DATA, 0, rest, 3); }
    else { special[R::VS + R::SH_FLOAT_DATA] = true; for (u32 word : {0u, 0u, 0u, bits}) write_reg(R::VS + R::SH_FLOAT_DATA, word, 15); }
}
static void packet(u32 id, const std::vector<u32> &values, bool group = false) {
    commands.push_back(le32(values[0]));
    commands.push_back(le32(id | 15u << 16 | (u32)(values.size() - 1) << 20 | (group ? 0x80000000u : 0)));
    for (size_t i = 1; i < values.size(); i++) commands.push_back(le32(values[i]));
    if (commands.size() & 1) commands.push_back(0);
}
static void wire_vertex(std::vector<u32> &words, float x) {
    for (const V4 value : {V4{{x, 0, 0, 1}}, V4{{1, 0, 0, 1}}, V4{{0, 0, 0, 1}}}) {
        const u32 a = f24_bits(value.v[0]), b = f24_bits(value.v[1]), c = f24_bits(value.v[2]), d = f24_bits(value.v[3]);
        words.push_back(d << 8 | c >> 16); words.push_back(c << 16 | b >> 8); words.push_back(b << 24 | a);
    }
}
namespace wiiu {
static int shutdown_calls, final_calls;
void gfx_shutdown() { ++shutdown_calls; }
void gfx_report_final() { ++final_calls; }
}
static void completion(bool parked, bool did_drain) {
    bool gpu_parked = parked, drained = did_drain;
    wiiu::shutdown_calls = wiiu::final_calls = 0;
#include "gpu_completion.inc"
    expect(wiiu::shutdown_calls == (parked ? 0 : 1) && wiiu::final_calls == (!parked || did_drain ? 1 : 0),
           "actual GPU-thread completion reports route/menu/OS exit, never reissues GX2 work while parked");
}
int main() {
    reset(); vertex(1); vertex(2); vertex(3); vertex(4); vertex(5); vertex(6); vtx_flush_immediate();
    expect(native_calls == 1 && native_inputs.size() == 6 && cpu_inputs.empty(), "six inputs form one native batch without CPU shading");
    expect(!g.imm_reset_gs && g.pa.index == 0, "native list preserves GS reset and empty PA");
    reset(); vertex(1); vertex(2); write_reg(R::RESTART_PRIM, 1, 15);
    expect(vtx_immediate_cpu_runs == 2 && triangles.empty() && native_calls == 0 && g.pa.index == 0,
           "discarded partial primitive still counts both actual CPU shader invocations");
    for (bool group : {false, true}) {
        reset();
        g.regs[R::VS_NUM_ATTR] = g.regs[R::VS + R::SH_INPUT_CFG] = 2;
        g.regs[R::VS + R::SH_PERM_LO] = 0x210;
        std::vector<u32> words;
        for (int i = 1; i <= 6; i++) wire_vertex(words, (float)i);
        if (group) {
            for (size_t i = 0; i < words.size(); i += 3) packet(R::FIXATTR_DATA, {words[i], words[i + 1], words[i + 2]}, true);
        } else packet(R::FIXATTR_DATA, words);
        cmd_run(0x1000, (u32)commands.size() * 4); vtx_flush_immediate();
        expect(native_calls == 1 && native_inputs.size() == 6 && cpu_inputs.empty() && vtx_immediate_cpu_runs == 0,
               "actual parser handles grouped and bulk packed-f24 immediate input as one native batch");
        expect(native_inputs.size() == 6 && native_inputs[5][0].v[0] == 6 && native_inputs[5][1].v[0] == 1,
               "actual f24 decoder and attribute latch preserve wire values");
    }
    reset();
    g.regs[R::VS_NUM_ATTR] = g.regs[R::VS + R::SH_INPUT_CFG] = 2; g.regs[R::VS + R::SH_PERM_LO] = 0x210;
    { std::vector<u32> words; wire_vertex(words, 1); wire_vertex(words, 2); packet(R::FIXATTR_DATA, words); }
    packet(R::VS + R::SH_FLOAT_IDX, {0x80000000u});
    u32 hundred; const float hundred_value = 100; memcpy(&hundred, &hundred_value, 4);
    packet(R::VS + R::SH_FLOAT_DATA, {0, 0, 0, hundred});
    { std::vector<u32> words; wire_vertex(words, 3); packet(R::FIXATTR_DATA, words); }
    cmd_run(0x1000, (u32)commands.size() * 4); vtx_flush_immediate();
    expect(triangles.size() == 1 && triangles[0][0].pos[0] == 11 && triangles[0][1].pos[0] == 12 && triangles[0][2].pos[0] == 103 && cpu_inputs.size() == 3,
           "actual bulk parser preserves two old vertices across fast uniform upload before continuation");
    for (bool fast : {false, true}) {
        reset(); vertex(1); vertex(2); uniform(100, fast);
        expect(cpu_inputs.size() == 2 && vtx_immediate_cpu_runs == 2 && g.pa.index == 2, "uniform upload drains two old vertices before mutation");
        vertex(3); vtx_flush_immediate();
        expect(triangles.size() == 1 && triangles[0][0].pos[0] == 11 && triangles[0][1].pos[0] == 12 && triangles[0][2].pos[0] == 103,
               "partial primitive retains original uniform state for its first two vertices");
        expect(native_calls == 0 && vtx_immediate_cpu_runs == 3, "continued primitive stays explicitly counted CPU work");
    }
    reset(); vertex(1); vertex(2); vertex(3); write_reg(R::VS + R::SH_BOOL, 1, 15);
    expect(native_calls == 1 && !native_bool && g.vs.b[0], "VS register >=0x200 drains before bool change");
    reset(); g.vs.code[0] = 7; vertex(1); vertex(2); vertex(3);
    { const u32 extra[] = {le32(9)}; write_run(RUN_VS_CODE, R::VS + R::SH_CODE_DATA, 8, extra, 1); }
    expect(native_calls == 1 && native_code == 7 && g.vs.code[0] == 8, "fast code upload drains before mutation");
    for (u32 count : {1u, 2u, 4u, 5u, 96u, 97u, 98u}) {
        reset(); for (u32 i = 0; i < count; i++) vertex((float)i); vtx_flush_immediate();
        expect(native_inputs.size() == count / 3 * 3 && cpu_inputs.size() == count % 3 && g.pa.index == count % 3, "bounded batches retain trailing vertices");
    }
    reset(); g.pa.topology = 3; g.pa.winding = true; vertex(1); vertex(2); vertex(3); vtx_flush_immediate();
    expect(native_inputs.size() == 3 && native_inputs[0][0].v[0] == 2 && native_inputs[1][0].v[0] == 1 && !g.pa.winding, "topology3 flips only its pending first triangle");
    reset(); g.pa.topology = 3; g.pa.winding = true; native_accept = false; vertex(1); vertex(2); vertex(3); vtx_flush_immediate();
    expect(triangles.size() == 1 && triangles[0][0].pos[0] == 12 && triangles[0][1].pos[0] == 11, "missing native shader preserves CPU winding");
    std::vector<std::array<V4, 16>> reference;
    for (bool native : {false, true}) {
        reset(); native_accept = native;
        g.regs[R::VS + R::SH_INPUT_CFG] = 5;
        g.regs[R::VS + R::SH_PERM_LO] = 0x725432; // attribute4 overwrites register2; attribute5 remains latched beyond VS_NUM_ATTR
        g.regs[R::VS_NUM_ATTR] = 0;
        for (u32 a = 0; a < 16; a++) g.imm_vertex[a] = V4{{float(a + 20), 1, 2, 3}};
        vertex(1); vertex(2); vertex(3); vtx_flush_immediate();
        if (!native) reference = cpu_inputs;
        else expect(native_inputs.size() == reference.size() && !memcmp(native_inputs.data(), reference.data(), reference.size() * sizeof(reference[0])),
                    "actual native mapping equals CPU inputs for retained attrs, duplicate last-wins and zero unmapped registers");
    }
    for (u32 topology : {1u, 2u}) { reset(); g.pa.topology = topology; vertex(1); vertex(2); vertex(3); vtx_flush_immediate(); expect(native_calls == 0 && cpu_inputs.size() == 3, "strip/fan remains counted CPU route"); }
    reset(); g.regs[R::GS_MISC] = 2; vertex(1); vertex(2); vertex(3); vtx_flush_immediate();
    expect(native_calls == 0 && gs_calls == 3 && cpu_inputs.size() == 3, "active GS remains counted CPU route");
    completion(false, false); // route/menu/OS exit while active
    completion(true, true);   // exit while normally parked after HOME drain
    completion(true, false);  // exceptional park without a completed drain must fail closed
    std::printf("{\"checks\":%d,\"failures\":%d}\n", checks, failures);
    return failures ? 1 : 0;
}
