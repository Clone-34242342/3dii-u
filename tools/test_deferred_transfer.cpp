// Execute the production deferred-readback body with controllable GPU/allocator stand-ins.
#include "surface_readback.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
struct Surface { u32 addr = 0x1000; u32 bytes() const { return 32; } };
static std::array<u8, 64> memory;
static bool fail_allocate, fail_read, fail_destination;
static unsigned readbacks, conversions, flushes;
static void *replay_malloc(size_t n) { return fail_allocate ? nullptr : std::malloc(n); }
static bool read_back_pixels(Surface &, u8 *pixels) {
    ++readbacks;
    if (fail_read) return false;
    memset(pixels, 7, 32);
    return true;
}
static u32 pixel_bytes(u32) { return 4; }
static u8 *pptr(u32 address) {
    assert(address >= 0x2000 && address <= 0x2020);
    return fail_destination ? nullptr : memory.data() + address - 0x2000;
}
static void transfer_convert(const u8 *src, u8 *dst, u32, u32, u32) {
    ++conversions;
    memcpy(dst, src, 32);
}
static void DCFlushRange(u8 *, u32) { ++flushes; }
static void metal_guest_read(u32, u32) {}
static struct Fills { void forget(u32, u32) {} } memory_fills;
#define malloc replay_malloc
#include "deferred_transfer_production.inc"
#undef malloc

int main() {
    Surface source;
    auto reset = [&] {
        for (auto &t : last_transfer) t = {};
        last_transfer[0] = {&source, 0x1000, 0x2000, 4 | 2 << 16, 4 | 2 << 16, 0, 32};
        memory.fill(0);
        fail_allocate = fail_read = fail_destination = false;
        readbacks = conversions = flushes = 0;
    };
    reset();
    gx2_cpu_will_read(0x3000, 1);
    assert(last_transfer[0].src == &source && !readbacks);
    gx2_cpu_will_read(0x2004, 1); // A tiny request materializes the complete transfer.
    assert(readbacks == 1 && conversions == 1 && flushes == 1);
    for (unsigned i = 0; i < 64; ++i) assert(memory[i] == (i < 32 ? 7 : 0));
    memset(memory.data(), 9, 16); // A later smaller transfer/CPU write must survive.
    gx2_cpu_will_read(0x2000, 32);
    for (unsigned i = 0; i < 32; ++i) assert(memory[i] == (i < 16 ? 9 : 7));
    assert(!last_transfer[0].src && readbacks == 1 && conversions == 1);
    for (int failure = 0; failure < 4; ++failure) {
        reset();
        if (failure == 0) fail_allocate = true;
        if (failure == 1) fail_read = true;
        if (failure == 2) fail_destination = true;
        if (failure == 3) last_transfer[0].in_paddr += 4;
        gx2_cpu_will_read(0x2000, 1);
        assert(last_transfer[0].src == &source && !conversions && !flushes);
        fail_allocate = fail_read = fail_destination = false;
        last_transfer[0].in_paddr = 0x1000;
        gx2_cpu_will_read(0x2000, 1);
        assert(!last_transfer[0].src && conversions == 1);
    }
    reset();
    last_transfer[2] = last_transfer[0];
    last_transfer[2].out_paddr = 0x2020;
    gx2_cpu_will_read(0x2000, 1);
    assert(!last_transfer[0].src && last_transfer[2].src == &source);
    memory[0] = 9;
    gx2_cpu_will_read(0x2020, 1);
    assert(!last_transfer[2].src && memory[0] == 9 && memory[32] == 7 && readbacks == 2);
    puts("Deferred transfer: complete copies consumed once; failures remain retryable");
}
