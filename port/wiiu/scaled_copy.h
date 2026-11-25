#pragma once
#include <cstdint>

namespace gx2_copy {
struct Span {
    std::uint32_t first, last;
    std::uint32_t size() const { return last - first; }
};
// Adjacent guest rectangles must share their scaled boundary. Scaling the
// width separately can leave holes: floor(a*s)+floor(b*s) != floor((a+b)*s).
inline Span span(std::uint32_t first, std::uint32_t count, float scale) {
    return {static_cast<std::uint32_t>(first * scale),
            static_cast<std::uint32_t>((first + count) * scale)};
}
// Padding and allocated dimensions have already been rounded by apply_scale.
// Logical tex_h ratios do not identify those actual pixel boundaries.
inline float uv(std::uint32_t pixel, std::uint32_t extent) {
    return static_cast<float>(pixel) / extent;
}
}
