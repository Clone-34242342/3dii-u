// The GamePad's microphone for mic:u. Holding the right stick in counts as blowing into it, for when the mic isn't practical.
// mic.rpl comes from OSDynLoad, as the sockets do in net_console.cpp: only a game that asks for the mic loads it, and without it blowing still works. A big build importing it hung before main.
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <malloc.h>
#include <mic/mic.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "mic_stream.h"
#include "services.h"
#include "wiiu.h"

namespace {
constexpr u32 SAMPLES = 0x2800;
constexpr u32 DEFAULT_HOST_RATE = 32000;
MICWorkMemory work;
MICHandle handle = -1;
bool opened;
std::atomic<bool> blowing;
MicStream stream;
u32 host_rate = DEFAULT_HOST_RATE;
u32 noise = 12345;
decltype(&MICInit) mic_init;
decltype(&MICOpen) mic_open;
decltype(&MICGetState) mic_get_state;
decltype(&MICGetStatus) mic_get_status;
decltype(&MICSetDataConsumed) mic_set_data_consumed;
decltype(&MICClose) mic_close;

bool load_mic() {
    OSDynLoad_Module m;
    if (OSDynLoad_Acquire("mic.rpl", &m) != OS_DYNLOAD_OK) return false;
    const struct {
        const char *name;
        void **where;
    } wanted[] = {{"MICInit", (void **)&mic_init},
                  {"MICOpen", (void **)&mic_open},
                  {"MICGetStatus", (void **)&mic_get_status},
                  {"MICSetDataConsumed", (void **)&mic_set_data_consumed},
                  {"MICClose", (void **)&mic_close}};
    for (const auto &w : wanted)
        if (OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, w.name, w.where) != OS_DYNLOAD_OK || !*w.where) return false;
    OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "MICGetState", (void **)&mic_get_state);
    return true;
}
}  // namespace

void wiiu::mic_blow(bool on) { blowing = on; }

void mic_host_start() {
    wiiu::note("starting the microphone");
    if (handle < 0) {
        if (!load_mic()) {
            LOG("[mic] mic.rpl didn't load\n");
            return;
        }
        work.sampleMaxCount = SAMPLES;
        work.sampleBuffer = memalign(0x40, SAMPLES * 2);
        MICError err = MIC_ERROR_INIT;
        if (work.sampleBuffer) handle = mic_init(MIC_INSTANCE_0, 0, &work, &err);
        if (err != MIC_ERROR_OK) {
            LOG("[mic] no GamePad microphone (error %d)\n", (int)err);
            free(work.sampleBuffer);
            work.sampleBuffer = nullptr;
            handle = -1;
            return;
        }
    }
    const MICError err = mic_open(handle);
    opened = err == MIC_ERROR_OK;
    stream = {};
    host_rate = DEFAULT_HOST_RATE;
    if (opened) {
        u32 rate = 0;
        if (mic_get_state && mic_get_state(handle, 0, &rate) == MIC_ERROR_OK && rate >= 8000 && rate <= 96000)
            host_rate = rate;
        // The driver can retain samples from the previous recording across close/open.
        MICStatus st{};
        if (mic_get_status(handle, &st) == MIC_ERROR_OK && st.availableData > 0)
            mic_set_data_consumed(handle, std::min(st.availableData, (int)SAMPLES));
        LOG("[mic] GamePad capture: %u Hz\n", host_rate);
    } else LOG("[mic] could not open the GamePad microphone (error %d)\n", (int)err);
    wiiu::note("microphone started: %d", (int)err);
}

void mic_host_stop() {
    if (opened) mic_close(handle);
    opened = false;
}

u32 mic_host_read_available(s16 *out, u32 n, u32 rate) {
    u32 produced = 0;
    MICStatus st{};
    bool failed = !opened || mic_get_status(handle, &st) != MIC_ERROR_OK ||
                  st.availableData < 0 || (st.availableData > 0 && (u32)st.bufferPos >= SAMPLES);
    if (!failed && st.availableData > 0) {
        const u32 available = std::min((u32)st.availableData, SAMPLES);
        // After a stall, bound the delay to the guest sampling timer's catch-up window.
        const u32 overlap = std::min(available, host_rate * 11 / 100);
        const u32 skip = available - overlap;
        DCInvalidateRange(work.sampleBuffer, SAMPLES * 2);
        MicStream next = skip ? MicStream{} : stream;
        const auto result = next.read((const s16 *)work.sampleBuffer, SAMPLES, (st.bufferPos + skip) % SAMPLES,
                                      overlap, host_rate, out, n, rate);
        const u32 consumed = skip + result.consumed;
        failed = consumed && mic_set_data_consumed(handle, (int)consumed) != MIC_ERROR_OK;
        if (!failed) {
            stream = next;
            produced = result.produced;
        }
    }
    if (failed) {
        stream = {};
        memset(out, 0, n * sizeof(*out));
        produced = n;
    }
    if (blowing) {
        for (u32 i = 0; i < n; i++) {
            noise = noise * 1664525u + 1013904223u;
            out[i] = (s16)(noise >> 16);
        }
        return n;
    }
    return produced;
}
