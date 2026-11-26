#pragma once
#include <cstdint>

namespace gx2_copy {
struct Span {
    std::uint32_t first, last;
    std::uint32_t size() const { return last - first; }
};

// Adjacent rectangles share a scaled edge even when the scale is fractional.
inline Span span(std::uint32_t first, std::uint32_t count, float scale) {
    return {static_cast<std::uint32_t>(first * scale),
            static_cast<std::uint32_t>((first + count) * scale)};
}
}
