// Real dispatcher + generated C; only the external lazy compiler is replaced by a synthetic callee provider.
#include <cassert>
#include <fstream>
#include <sstream>
#include <sys/mman.h>
#include <dlfcn.h>
#include "kernel.h"
#include "loader.h"

static int provide_callee(const char *);
static void *open_callee(const char *, int);
static void *callee_symbol(void *, const char *);
#define system provide_callee
#define dlopen open_callee
#define dlsym callee_symbol
#include "../third_party/3dsrecomp/runtime/core.cpp"
#undef system
#undef dlopen
#undef dlsym

std::string g_save_base = ".";
volatile u32 rt_preempt_flag;
void watch_report() {}
namespace kernel { Thread *current_thread() { return nullptr; } }
extern "C" void rt_preempt(CPU *) { assert(false && "unexpected preemption"); }
static int helper_calls, misses, interpreted_calls;
extern "C" void rt_svc(CPU *c, u32 imm) {
    assert(imm == 0x7f && c->r[4] == 99);
    helper_calls++;
}

// This fixture accepts only its five known instructions. It does not promise general decoding or memory validation.
static void interpret_fixture(CPU *c, u32) {
    interpreted_calls++;
    const u32 stop = c->r[14];
    for (int steps = 0; steps < 8; steps++) {
        if (c->pc == stop) return;
        const bool thumb = c->pc & 1;
        const u32 at = c->pc & ~1u;
        assert((!thumb && at >= 0x100100 && at < 0x100114) ||
               (thumb && at >= 0x100140 && at < 0x10014a));
        const u32 op = thumb ? r16(at) : r32(at);
        const u32 next = (at + (thumb ? 2 : 4)) | thumb;
        if (op == (thumb ? 0xb510u : 0xe92d4010u)) {
            c->r[13] -= 8;
            w32(c->r[13], c->r[4]);
            w32(c->r[13] + 4, c->r[14]);
        } else if (op == (thumb ? 0x2407u : 0xe3a04007u)) {
            c->r[4] = 7;
        } else if (op == (thumb ? 0x4790u : 0xe12fff32u)) {
            // ABI functions preserve these registers in C locals, not necessarily in the shared CPU slots.
            u32 saved[9];
            for (int i = 0; i < 8; i++) saved[i] = c->r[4 + i];
            saved[8] = c->r[13];
            c->r[14] = next;
            c->pc = c->r[2];
            assert(dispatch_lookup(c->pc));
            rt_dispatch(c, 0);
            assert(c->pc == next);
            for (int i = 0; i < 8; i++) c->r[4 + i] = saved[i];
            c->r[13] = saved[8];
        } else if (op == (thumb ? 0x1900u : 0xe0800004u)) {
            c->r[0] += c->r[4];
        } else if (op == (thumb ? 0xbd10u : 0xe8bd8010u)) {
            c->r[4] = r32(c->r[13]);
            c->pc = r32(c->r[13] + 4);
            c->r[13] += 8;
            assert(c->pc == stop);
            return;
        } else {
            assert(false && "unexpected fixture opcode");
        }
        c->pc = next;
    }
    assert(false && "fixture did not return");
}

static const GuestEntry provided[] = {{0x100100, interpret_fixture, 0}, {0x100141, interpret_fixture, 0}};
static const u32 provided_count = 2;
static int provide_callee(const char *command) {
    assert(std::string(command).find("lazy.cmd") != std::string::npos);
    misses++;
    return 0;
}
static void *open_callee(const char *, int) { return (void *)provided; }
static void *callee_symbol(void *, const char *name) {
    if (!strcmp(name, "guest_lazy_entries")) return (void *)provided;
    if (!strcmp(name, "guest_lazy_count")) return (void *)&provided_count;
    assert(false && "unexpected lazy symbol");
    return nullptr;
}

int main() {
    std::ifstream input("../code.bin", std::ios::binary);
    assert(input);
    g_mem = (u8 *)calloc(1, 0x200000);
    assert(g_mem);
    input.read((char *)g_mem + 0x100000, 0x220);
    assert(input.gcount() == 0x220);
    CPU c{};
    for (int i = 0; i < 16; i++) c.r[i] = 0xa000 + i;
    c.r[13] = 0x180000;
    c.pc = 0x123456;
    for (u32 target : {0x100100u, 0x100141u}) {
        assert((dispatch_lookup(target) == nullptr) == (OMIT_CALLEES != 0));
        const CPU before = c;
        assert(rt_guest_call(&c, 0x100000, 10, target, 0x100200, 0) == 71);
        assert(!memcmp(&c, &before, sizeof c));
        assert(rt_guest_call(&c, target, 10, 0, 0x100200, 0) == 22);
        assert(!memcmp(&c, &before, sizeof c));
    }
    assert(helper_calls == 4);
    // The first provider returns both missing entries; later dispatches reuse them.
    assert(misses == (OMIT_CALLEES ? 1 : 0));
    assert(interpreted_calls == (OMIT_CALLEES ? 4 : 0));
    if (OMIT_CALLEES) {
        std::ifstream log("missing_functions.txt");
        std::string address, extra;
        assert(log >> address && address == "0x00100100" && !(log >> extra));
    }
    free(g_mem);
}
