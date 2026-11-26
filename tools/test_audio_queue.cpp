// Production Wii U audio host with an allocator/AX boundary only; no queue algorithm is copied.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unordered_map>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include "rt.h"
#include "fake_audio_sdk.h"

namespace hal {
std::unordered_map<void *, size_t> blocks;
size_t allocated, peak, voices, full_flushes;
AXVoice *channels[8]{};
int fail_alloc_at, fail_voice_at, alloc_attempts, voice_attempts, init_calls, quit_calls;
uint64_t now, stop, consumed, fraction, supplied;
int ppm;
bool quitting_clock;
std::atomic<bool> hold_offsets{}, entered_offsets{}, release_offsets{};
void *allocate(size_t align, size_t size) {
    if (++alloc_attempts == fail_alloc_at) return nullptr;
#ifdef _WIN32
    void *pointer = _aligned_malloc(size, align);
#else
    void *pointer = nullptr;
    if (posix_memalign(&pointer, align, size)) return nullptr;
#endif
    assert(pointer);
    blocks[pointer] = size;
    allocated += size;
    peak = std::max(peak, allocated);
    return pointer;
}
void release(void *pointer) {
    if (!pointer) return;
    const auto found = blocks.find(pointer);
    assert(found != blocks.end());
    allocated -= found->second;
    blocks.erase(found);
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}
void rt_log(const char *format, ...) { va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); }
[[noreturn]] void rt_fatal(const char *format, ...) { va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); abort(); }

#define memalign hal::allocate
#define free hal::release
#ifndef AUDIO_HOST_SOURCE
#define AUDIO_HOST_SOURCE "port/wiiu/audio_wiiu.cpp"
#endif
#include AUDIO_HOST_SOURCE
#undef free
#undef memalign

void AXInitWithParams(const AXInitParams *) { hal::init_calls++; }
void AXQuit() { hal::quit_calls++; }
AXVoice *AXAcquireVoice(unsigned, void *, void *) {
    if (++hal::voice_attempts == hal::fail_voice_at) return nullptr;
    auto *voice = new AXVoice{};
    for (auto &channel : hal::channels)
        if (!channel) { channel = voice; hal::voices++; return voice; }
    assert(false && "HAL voice limit exceeded");
    return voice;
}
void AXFreeVoice(AXVoice *voice) {
    assert(voice && hal::voices);
    for (auto &channel : hal::channels) if (channel == voice) channel = nullptr;
    hal::voices--; delete voice;
}
void AXVoiceBegin(AXVoice *voice) { assert(voice); }
void AXVoiceEnd(AXVoice *voice) { assert(voice); }
void AXSetVoiceType(AXVoice *, unsigned) {}
void AXSetVoiceState(AXVoice *voice, unsigned state) { voice->playing = state == AX_VOICE_STATE_PLAYING; }
void AXSetVoiceVe(AXVoice *, const AXVoiceVeData *) {}
void AXSetVoiceDeviceMix(AXVoice *, unsigned, unsigned, const AXVoiceDeviceMixData *) {}
void AXSetVoiceOffsets(AXVoice *voice, const AXVoiceOffsets *offsets) { voice->offsets = *offsets; }
void AXGetVoiceOffsets(AXVoice *voice, AXVoiceOffsets *offsets) {
    assert(voice);
    if (hal::hold_offsets) {
        hal::entered_offsets = true;
        while (!hal::release_offsets) std::this_thread::yield();
    }
    *offsets = voice->offsets;
}
void AXSetVoiceSrcType(AXVoice *, unsigned) {}
void AXSetVoiceSrcRatio(AXVoice *, float ratio) { assert(ratio > 0.68f && ratio < 0.69f); }
unsigned AXGetInputSamplesPerSec() { return 48000; }
void DCFlushRange(void *pointer, unsigned bytes) {
    if (bytes == 8192 * 2) hal::full_flushes++;
    const auto address = (uintptr_t)pointer;
    for (const auto &[base, size] : hal::blocks)
        if (address >= (uintptr_t)base && address + bytes <= (uintptr_t)base + size) return;
    assert(false && "cache flush outside allocated ring");
}

// Only the AX/OS clock is simulated. This is the production frontend's extracted loop.
using OSTime = long long;
static bool quitting, gpu_pause, audio_parked;
static OSTime OSNanosecondsToTicks(OSTime value) { return value; }
static OSTime OSMillisecondsToTicks(OSTime value) { return value * 1000000; }
static OSTime OSSecondsToTicks(OSTime value) { return value * 1000000000; }
static OSTime OSTicksToMilliseconds(OSTime value) { return value / 1000000; }
static OSTime OSGetSystemTime() { return hal::now; }
static void OSSleepTicks(OSTime ticks) {
    assert(ticks > 0);
    const u64 next = hal::now + ticks;
    // AX consumes in3ms bursts; fractional sample phase and ±0.5% device skew are retained.
    for (u64 block = hal::now / 3000000; block < next / 3000000; block++) {
        if (!hal::channels[0] || !hal::channels[0]->playing) continue;
        hal::fraction += 32728ull * (1000000 + hal::ppm) * 3000000;
        const u64 samples = hal::fraction / 1000000000000000ull;
        hal::fraction %= 1000000000000000ull;
        hal::consumed += samples;
        for (auto *channel : hal::channels)
            if (channel) channel->offsets.currentOffset = (channel->offsets.currentOffset + samples) % 8192;
    }
    hal::now = next;
    if (hal::stop && hal::now >= hal::stop) quitting = true;
}
namespace kernel {
void dsp_tick() {
    audio::hle::Sample frame[160]{};
    audio::hle::host_output(frame, 160);
    hal::supplied += 160;
}
}
#include "audio_frontend_thread.inc"

static void long_clock(int ppm) {
    quitting = gpu_pause = audio_parked = false;
    hal::now = hal::consumed = hal::fraction = hal::supplied = 0;
    hal::stop = 3600ull * 1000000000;
    hal::ppm = ppm;
    audio::init();
    const size_t initial_flushes = hal::full_flushes;
    audio_thread(0, nullptr);
    assert(audio_parked && hal::full_flushes == initial_flushes);
    const int queued = audio::hle::host_queued();
    assert(queued >= 1400 && queued < 1800);
    assert(1600 + hal::supplied - hal::consumed == (u64)queued);
    printf("{\"clock_ppm\":%d,\"virtual_seconds\":3600,\"supplied\":%llu,\"consumed\":%llu,\"queued\":%d,\"unplanned_resyncs\":0}\n",
        ppm, (unsigned long long)hal::supplied, (unsigned long long)hal::consumed, queued);
    audio::shutdown();
    hal::stop = 0;
}

static void in_flight_shutdown() {
    audio::init();
    audio::hle::Sample frame[160]{};
    for (int i = 0; i < 10; i++) audio::hle::host_output(frame, 160);
    hal::hold_offsets = true;
    hal::entered_offsets = hal::release_offsets = false;
    std::thread writer([&] { audio::hle::host_output(frame, 160); });
    while (!hal::entered_offsets) std::this_thread::yield();
    std::atomic<bool> closing{}, closed{};
    std::thread closer([&] { closing = true; audio::shutdown(); closed = true; });
    while (!closing) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!closed && hal::voices == 2);
    hal::release_offsets = true;
    writer.join(); closer.join();
    hal::hold_offsets = false;
    assert(closed && !hal::voices && !hal::allocated);
}

static void underrun_recovery() {
    hal::now = hal::fraction = hal::consumed = hal::stop = 0;
    hal::ppm = 0;
    audio::init();
    audio::hle::Sample frame[160];
    for (auto &sample : frame) sample = {777, 777};
    for (int i = 0; i < 10; i++) audio::hle::host_output(frame, 160);
    const u32 before = hal::channels[0]->offsets.currentOffset;
    audio::pause(true);
    OSSleepTicks(500000000);
    assert(hal::channels[0]->offsets.currentOffset == before);
    audio::pause(false);
    const size_t initial_flushes = hal::full_flushes;
    OSSleepTicks(200000000);  // Deliberately starve the producer beyond the available queue.
    for (auto &sample : frame) sample = {111, 222};
    audio::hle::host_output(frame, 160);
    assert(hal::full_flushes == initial_flushes + 2);
    assert(audio::hle::host_queued() == 1760);
    for (int channel = 0; channel < 2; channel++) {
        const auto *samples = (const s16 *)hal::channels[channel]->offsets.data;
        u32 fresh = 0;
        for (u32 i = 0; i < 8192; i++) {
            assert(samples[i] == 0 || samples[i] == (channel ? 222 : 111));
            fresh += samples[i] != 0;
        }
        assert(fresh == 160);
    }
    audio::shutdown();
    puts("{\"intentional_starvation_ms\":200,\"recovery_queue\":1760,\"stale_nonzero_samples\":0,\"pause_clock_preserved\":true}");
}

static int startup_probe(const char *kind, int index) {
    if (!strcmp(kind, "alloc")) hal::fail_alloc_at = index;
    else if (!strcmp(kind, "voice")) hal::fail_voice_at = index;
    else assert(!strcmp(kind, "idempotent"));
    audio::init();
    if (strcmp(kind, "idempotent")) {
        const bool clean = !hal::allocated && !hal::voices && hal::init_calls == 1 && hal::quit_calls == 1;
        printf("{\"failure\":\"%s\",\"index\":%d,\"clean_rollback\":%s,\"bytes\":%zu,\"voices\":%zu}\n",
               kind, index, clean ? "true" : "false", hal::allocated, hal::voices);
        if (!clean) return 1;
        audio::shutdown();
        assert(hal::quit_calls == 1);
        hal::fail_alloc_at = hal::fail_voice_at = 0;
        audio::init();
    }
    const int init_calls = hal::init_calls, allocs = hal::alloc_attempts, voices = hal::voice_attempts;
    audio::init();
    const bool idempotent = hal::init_calls == init_calls && hal::alloc_attempts == allocs &&
        hal::voice_attempts == voices && hal::allocated == 32768 && hal::voices == 2;
    if (!idempotent) { puts("{\"idempotent_init\":false}"); return 1; }
    audio::hle::Sample frame[160]{};
    audio::hle::host_output(frame, 160);
    audio::shutdown();
    const int quit_calls = hal::quit_calls;
    audio::shutdown();
    const bool clean = !hal::allocated && !hal::voices && hal::quit_calls == quit_calls;
    printf("{\"retry\":true,\"idempotent_init\":true,\"idempotent_shutdown\":%s,\"pass\":%s}\n",
           clean ? "true" : "false", clean ? "true" : "false");
    return clean ? 0 : 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc == 4 && !strcmp(argv[1], "--startup")) return startup_probe(argv[2], atoi(argv[3]));
    for (int ppm : {-5000, 0, 5000}) long_clock(ppm);
    audio::hle::Sample samples[160]{};
    for (int cycle = 0; cycle < 256; cycle++) {
        audio::init();
        for (int frame = 0; frame < 10; frame++) audio::hle::host_output(samples, 160);
        audio::pause(true);
        audio::pause(false);
        audio::shutdown();
        assert(hal::voices == 0);
        audio::hle::host_output(samples, 160);
        audio::pause(true);
        audio::shutdown();
    }
    printf("{\"cycles\":256,\"live_ring_bytes\":%zu,\"peak_ring_bytes\":%zu,\"live_voices\":%zu,\"pass\":%s}\n",
           hal::allocated, hal::peak, hal::voices, hal::allocated == 0 ? "true" : "false");
    if (hal::allocated) return 1;
    underrun_recovery();
    in_flight_shutdown();
    printf("{\"in_flight_shutdown\":\"serialized\",\"live_ring_bytes\":%zu,\"pass\":true}\n", hal::allocated);
    return 0;
}
