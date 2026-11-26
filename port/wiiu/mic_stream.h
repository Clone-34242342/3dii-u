#pragma once

#include <algorithm>
#include <cstdint>

struct MicStream {
    struct Read {
        uint32_t produced = 0, consumed = 0;
    };

    uint32_t position = 0, output_rate = 0;

    Read read(const int16_t *ring, uint32_t capacity, uint32_t first, uint32_t available,
              uint32_t input_rate, int16_t *out, uint32_t limit, uint32_t rate) {
        if (output_rate && output_rate != rate)
            position = (uint64_t)position * rate / output_rate;
        output_rate = rate;
        Read result;
        while (result.produced < limit) {
            const uint32_t at = position / rate, fraction = position % rate;
            if (at >= available || (fraction && at + 1 == available)) break;
            const int32_t first_sample = ring[(first + at) % capacity];
            const int32_t next_sample = fraction ? ring[(first + at + 1) % capacity] : first_sample;
            out[result.produced++] = (int16_t)(((int64_t)first_sample * (rate - fraction) + (int64_t)next_sample * fraction) / rate);
            position += input_rate;
        }
        result.consumed = std::min(available, position / rate);
        position -= result.consumed * rate;
        return result;
    }
};
