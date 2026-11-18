// Extend the existing production DSP tests and their App protocol, without replacing any DSP operation.
#include <atomic>
#include <cstddef>
#include <new>
#include <cstdlib>
namespace heap_count {
struct alignas(std::max_align_t) Header { size_t bytes; };
std::atomic<size_t> live{}, peak{};
}
void *operator new(size_t size) {
    auto *head = (heap_count::Header *)std::malloc(sizeof(heap_count::Header) + (size ? size : 1));
    if (!head) std::abort();
    head->bytes = size;
    const size_t value = heap_count::live.fetch_add(size) + size;
    size_t old = heap_count::peak;
    while (old < value && !heap_count::peak.compare_exchange_weak(old, value)) {}
    return head + 1;
}
void operator delete(void *pointer) noexcept {
    if (!pointer) return;
    auto *head = (heap_count::Header *)pointer - 1;
    heap_count::live -= head->bytes;
    std::free(head);
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
void operator delete(void *pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, size_t) noexcept { ::operator delete(pointer); }

#define main upstream_audio_main
#include "third_party/3dsrecomp/tools/audio_test/audio_test.cpp"
#undef main

namespace sink {
u64 frames, samples;
bool exact_constant;
}
namespace audio {
void init() {}
namespace hle {
void host_output(const Sample *sample, size_t count) {
    CHECK_EQ(count, 160);
    sink::frames++;
    sink::samples += count;
    if (sink::exact_constant)
        for (size_t i = 0; i < count; i++) {
            CHECK_EQ(sample[i][0], 1234);
            CHECK_EQ(sample[i][1], 1234);
        }
}
int host_queued() { return 0; }
}
}
namespace mem {
u8 *paddr_ptr(u32 pa) {
    if (pa >= FCRAM_PADDR && pa - FCRAM_PADDR < LINEAR_SIZE) return g_mem + LINEAR_BASE + pa - FCRAM_PADDR;
    if (pa >= VRAM_PADDR && pa - VRAM_PADDR < VRAM_SIZE) return g_mem + VRAM_BASE + pa - VRAM_PADDR;
    if (pa >= DSP_PADDR && pa - DSP_PADDR < DSP_SIZE) return g_mem + DSP_BASE + pa - DSP_PADDR;
    return nullptr;
}
}

static void configure_loop(App &app) {
    auto &m = app.next();
    setup_master(m);
    auto &c = m.source_config[0];
    c.enable = 1;
    c.rate_multiplier = 1;
    c.gain[0][0] = c.gain[0][1] = 1;
    c.flags1 = 1 | ((u16)Format::PCM16 << 2);
    c.flags2 = 2;
    c.paddr.set(0x20010000);
    c.length.set(8192);
    c.buffer_id = 1;
    c.dirty = SRC_ENABLE_DIRTY | SRC_RATE_DIRTY | SRC_GAIN0_DIRTY | SRC_EMBEDDED_DIRTY | SRC_FORMAT_DIRTY;
}

int main(int argc, char **argv) {
    const int before = upstream_audio_main(1, argv);
    CHECK_EQ(before, 0);
    dsp_init();
    audio::pipe_read(2, 32);
    for (u32 i = 0; i < 8192; i++) w16(mem::LINEAR_BASE + 0x10000 + 2 * i, 1234);
    App app;
    configure_loop(app);
    for (int i = 0; i < 8; i++) CHECK(app.submit());
    sink::frames = sink::samples = 0;
    sink::exact_constant = true;
    const size_t heap_before = heap_count::live;
    u64 delayed = 0;
    for (u32 i = 0; i < 70000; i++) {
        if (i % 97 == 0) {
            SharedMemory rd = app.next(), wr = app.out();
            bool fresh = true;
            CHECK(audio::tick(false, &fresh));
            CHECK(!fresh);
            CHECK(!memcmp(&rd, &app.next(), sizeof(rd)));
            CHECK(!memcmp(&wr, &app.out(), sizeof(wr)));
            delayed++;
        }
        CHECK(app.submit());
        CHECK_EQ(app.out().source_status[0].is_enabled, 1);
    }
    CHECK_EQ(sink::frames, 70000 + delayed);
    CHECK_EQ(sink::samples, (70000 + delayed) * 160);
    CHECK_EQ(app.counter, (u16)(70000 + 8));
    CHECK(heap_count::live.load() <= heap_before + 4096);
    sink::exact_constant = false;
    const u64 clock_frames = sink::frames, clock_samples = sink::samples;
    const size_t stable_heap = heap_count::live;
    for (u32 cycle = 0; cycle < 2000; cycle++) {
        set_state(1);
        CHECK(!audio::tick(true));
        dsp_init();
        audio::pipe_read(2, 32);
        App restarted;
        configure_loop(restarted);
        CHECK(restarted.submit());
        set_state(3);
        audio::pipe_read(2, 32);
        CHECK(!audio::tick(true));
        set_state(2);
        audio::pipe_read(2, 32);
        CHECK(restarted.submit());
    }
    const size_t heap_after = heap_count::live;
    // One512KiB sleep snapshot is intentionally retained for reuse; growth after that must stop.
    CHECK(heap_after <= stable_heap + kDspRamSize + 65536);
    printf("{\"long_frames\":%llu,\"samples\":%llu,\"virtual_seconds\":%.6f,\"delayed_ticks\":%llu,"
           "\"counter_wraps\":1,\"reset_sleep_cycles\":2000,\"heap_before\":%zu,\"heap_after\":%zu,"
           "\"peak_cpp_heap\":%zu,\"checks\":%d,\"failures\":%d}\n",
        (unsigned long long)clock_frames, (unsigned long long)clock_samples, clock_samples / 32728.0,
        (unsigned long long)delayed, heap_before, heap_after, heap_count::peak.load(), g_checks, g_failures);
    return g_failures ? 1 : 0;
}
