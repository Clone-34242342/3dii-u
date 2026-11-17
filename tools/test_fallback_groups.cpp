#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "cpu.h"
#include "rt.h"
#include "kernel.h"
#include "fallback.h"

// Synthetic memory and dispatch environment for unit test
u8 *g_mem = nullptr;
std::string g_save_base = ".";
volatile u32 rt_preempt_flag = 0;
void watch_report() {}
void rt_log(const char *, ...) {}
namespace kernel { Thread *current_thread() { return nullptr; } }
extern "C" void rt_preempt(CPU *) { assert(false && "unexpected preemption"); }
[[noreturn]] void rt_fatal(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    abort();
}

static int helper_invocations = 0;
static void compiled_helper(CPU *c, u32) {
    helper_invocations++;
    assert(c->r[0] == 10);
    c->r[0] += 5; // returns 15
    c->pc = c->r[14];
}

static const GuestEntry test_entries[] = {
    {0x100200, compiled_helper, 0x100200}
};

const GuestEntry *dispatch_lookup(u32 addr) {
    if (addr == 0x100200) return &test_entries[0];
    return nullptr;
}

void rt_dispatch(CPU *c, u32) {
    const GuestEntry *e = dispatch_lookup(c->pc);
    if (e) {
        e->fn(c, e->entry);
    } else {
        assert(false && "unexpected dispatch miss in test");
    }
}

int main() {
    g_mem = (u8 *)calloc(1, 0x200000);
    assert(g_mem);

    // 1. ARM Test Function at 0x100100:
    // push {r4, lr}        (0xe92d4010)
    // mov r4, #7           (0xe3a04007)
    // blx r2               (0xe12fff32) -> calls helper at r2 (0x100200)
    // add r0, r0, r4       (0xe0800004) -> 15 + 7 = 22
    // pop {r4, pc}         (0xe8bd8010)
    const u32 arm_code[] = {0xe92d4010, 0xe3a04007, 0xe12fff32, 0xe0800004, 0xe8bd8010};
    memcpy(g_mem + 0x100100, arm_code, sizeof arm_code);

    // 2. Thumb Test Function at 0x100140:
    // push {r4, lr}        (0xb510)
    // movs r4, #7          (0x2407)
    // blx r2               (0x4790) -> calls helper at r2 (0x100200)
    // adds r0, r0, r4      (0x1900) -> 15 + 7 = 22
    // pop {r4, pc}         (0xbd10)
    const u16 thumb_code[] = {0xb510, 0x2407, 0x4790, 0x1900, 0xbd10};
    memcpy(g_mem + 0x100140, thumb_code, sizeof thumb_code);

    // 3. Conditional branch test at 0x100180 (Thumb):
    // cmp r0, #10          (0x280a)
    // bne +4               (0xd101) -> if NE, skip adds
    // adds r0, #3          (0x3003)
    // bx lr                (0x4770)
    const u16 thumb_branch[] = {0x280a, 0xd101, 0x3003, 0x4770, 0x3005, 0x4770};
    memcpy(g_mem + 0x100180, thumb_branch, sizeof thumb_branch);

    CPU c{};
    for (int i = 0; i < 16; i++) c.r[i] = 0x1000 + i;
    c.r[13] = 0x180000;

    // Test ARM execution
    {
        c.r[0] = 10;
        c.r[2] = 0x100200; // helper target
        c.r[14] = 0xFFFF0004u; // magic stop
        c.pc = 0x100100;
        fallback::execute(&c, 0x100100);
        assert(c.r[0] == 22);
        assert(c.pc == 0xFFFF0004u);
        assert(helper_invocations == 1);
        assert(c.r[13] == 0x180000);
        assert(c.r[4] == 0x1004); // preserved by push/pop
    }

    // Test Thumb execution
    {
        c.r[0] = 10;
        c.r[2] = 0x100200; // helper target
        c.r[14] = 0xFFFF0004u; // magic stop
        c.pc = 0x100141; // thumb bit set
        fallback::execute(&c, 0x100141);
        assert(c.r[0] == 22);
        assert(c.pc == 0xFFFF0004u);
        assert(helper_invocations == 2);
        assert(c.r[13] == 0x180000);
        assert(c.r[4] == 0x1004); // preserved by push/pop
    }

    // Test Thumb branch (condition taken: r0 == 10, NE is false, does not skip)
    {
        c.r[0] = 10;
        c.r[14] = 0xFFFF0004u;
        c.pc = 0x100181;
        fallback::execute(&c, 0x100181);
        assert(c.r[0] == 13);
        assert(c.pc == 0xFFFF0004u);
    }

    // Test Thumb branch (condition not taken: r0 == 9, NE is true, skips to 0x100188)
    {
        c.r[0] = 9;
        c.r[14] = 0xFFFF0004u;
        c.pc = 0x100181;
        fallback::execute(&c, 0x100181);
        assert(c.r[0] == 14); // 9 + 5 = 14
        assert(c.pc == 0xFFFF0004u);
    }

    // Benchmark throughput & step latency
    {
        constexpr int kIterations = 20000;
        constexpr int kInstPerIter = 5;
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < kIterations; i++) {
            c.r[0] = 10;
            c.r[2] = 0x100200;
            c.r[14] = 0xFFFF0004u;
            c.pc = 0x100141;
            fallback::execute(&c, 0x100141);
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        const double total_inst = (double)kIterations * kInstPerIter;
        const double mips = (total_inst / (ns * 1e-9)) / 1e6;
        const double ns_per_inst = ns / total_inst;
        printf("fallback benchmark: %.2f MIPS (%.1f ns/instruction, %d steps)\n", mips, ns_per_inst, (int)total_inst);
    }

    free(g_mem);
    printf("test_fallback_groups: ok\n");
    return 0;
}
