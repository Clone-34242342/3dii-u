// The Wii U microphone resampler must keep speech intact across short reads and ring wrap.
#include <array>
#include <cassert>
#include <cstdio>

#include "../port/wiiu/mic_stream.h"

static int16_t sample(uint32_t index) {
    return (int16_t)((index * 197u) % 65536u - 32768);
}

static void playback(uint32_t rate) {
    constexpr uint32_t input_rate = 32000, capacity = 0x2800;
    std::array<int16_t, capacity> ring{};
    uint32_t first = capacity - 52, available = 0, written = 0, produced = 0, remainder = 0;
    MicStream stream;
    for (uint32_t tick = 1; tick <= 1000; tick++) {
        // The source arrives in 3 ms packets, with a late poll every eight ticks.
        const uint32_t captured = (tick * 10 + (tick % 8 == 0 ? 6 : 0)) / 3 * 96;
        while (written < captured) {
            assert(available < capacity);
            ring[(first + available++) % capacity] = sample(written++);
        }
        std::array<int16_t, 512> out;
        out.fill(12345);
        const uint32_t limit = (remainder + rate) / 100;
        remainder = (remainder + rate) % 100;
        const auto got = stream.read(ring.data(), capacity, first, available, input_rate, out.data(), limit, rate);
        assert(got.produced <= limit && got.consumed <= available);
        assert(out[got.produced] == 12345);
        for (uint32_t i = 0; i < got.produced; i++) {
            const uint64_t position = (uint64_t)produced++ * input_rate;
            const uint32_t at = position / rate, fraction = position % rate;
            const int64_t expected = (int64_t)sample(at) * (rate - fraction) + (int64_t)sample(at + 1) * fraction;
            assert(out[i] == expected / rate);
        }
        first = (first + got.consumed) % capacity;
        available -= got.consumed;
    }
    // A short final packet can defer a few samples to the next poll.
    assert(produced <= 10 * rate && produced >= 10 * rate - rate / 50);
}

int main() {
    for (uint32_t rate : {32728u, 16364u, 10909u, 8182u}) playback(rate);
    puts("GamePad microphone: continuous PCM across packet jitter, late polls, and ring wrap");
}
