// Ordinary GPRs exclude R124..R127, the four clause temporaries. The VS/PS pool reserves four per stage.
#pragma once
#include <cstdint>
namespace gpu::shader_budget {
constexpr uint32_t kOrdinary = 124, kPool = 248, kDefaultVertex = 48;
inline bool valid(uint32_t registers) { return registers && registers <= kOrdinary; }
inline uint32_t vertex(uint32_t current, uint32_t required) {
    if (!valid(current) || required > kOrdinary) return 0;
    const uint32_t next = current > required ? current : required;
    return next > kDefaultVertex ? next : kDefaultVertex;
}
inline uint32_t pixel(uint32_t vertex) { return kPool - vertex; }
}  // namespace gpu::shader_budget
