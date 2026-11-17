#pragma once
#include <cstdint>

namespace wiiu {
// Protected by the backend's frame mutex; a reading surface cannot return to the DMA queue.
struct CameraQueue {
    enum State { FREE, QUEUED, READY, READING };
    static constexpr int COUNT = 2;
    State state[COUNT] = {};
    uint32_t sequence[COUNT] = {}, latest = 0;

    void clear() {
        for (auto &s : state) s = FREE;
    }
    bool submit(int i) {
        if (state[i] != FREE) return false;
        state[i] = QUEUED;
        return true;
    }
    void complete(int i, bool failed) {
        if (state[i] != QUEUED) return;
        state[i] = failed ? FREE : READY;
        if (!failed) sequence[i] = ++latest;
    }
    int acquire() {
        int best = -1;
        for (int i = 0; i < COUNT; i++)
            if (state[i] == READY && (best < 0 || latest - sequence[i] < latest - sequence[best])) best = i;
        if (best >= 0) {
            state[best] = READING;
            for (auto &s : state) if (s == READY) s = FREE;
        }
        return best;
    }
    void release(int i) {
        if (state[i] == READING) state[i] = FREE;
    }
};
}  // namespace wiiu
