// Executes the production lookup prefix and complete forget_unused body.
// Allocator/GPU operations are observable stand-ins; this is descriptor ownership, not a GPU test.
#include "surface_readback.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

using u32 = uint32_t;
using u64 = uint64_t;
struct DrawState {};
struct Surface {
    u32 addr = 0, w = 400, h = 240, fmt = 0;
    bool depth = true, mem1 = true, shared_depth = true, in_list = false;
    float base_sx = 1, base_sy = 1;
    u64 last_use = 0, drawn = 0, written_after = 0;
    bool gpu_newer = false, guest_newer = false;
    surface_readback::Spans written;
    void *backing = nullptr;
    u32 bytes() const { return 16; }
    void *image() { return backing; }
};
std::vector<std::unique_ptr<Surface>> surfaces;
Surface *cur_color, *cur_depth, *depth_owner;
u64 use_counter;
bool state_sent_valid, state_lost;
unsigned heap_frees, mem1_frees, shared_frees, writebacks, allocations;
unsigned failures;
std::array<uint8_t, 64> depth_storage;

static bool contains(const Surface *s) {
    return std::any_of(surfaces.begin(), surfaces.end(), [&](const auto &p) { return p.get() == s; });
}
static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
static void heap_free(void *p) {
    ++heap_frees;
    if (p == depth_storage.data()) ++shared_frees;
}
static void mem1_free(void *p) {
    ++mem1_frees;
    if (p == depth_storage.data()) ++shared_frees;
}
void (*GfxHeapFreeMEM1)(void *) = mem1_free;
static void write_back(Surface &s, const surface_readback::Spans &) {
    ++writebacks;
    s.gpu_newer = false;
}

#define free heap_free
#include "surface_lifetime_production.inc"
#undef free

static void reset() {
    surfaces.clear();
    cur_color = cur_depth = depth_owner = nullptr;
    for (auto &t : last_transfer) t = {};
    use_counter = 0;
    state_sent_valid = true;
    state_lost = false;
    heap_frees = mem1_frees = shared_frees = writebacks = allocations = 0;
}
static Surface *append(u32 key, bool depth = true, bool shared = true, bool mem1 = true) {
    auto p = std::make_unique<Surface>();
    p->addr = 0x18000000 + key * 0x1000;
    p->depth = depth;
    p->shared_depth = shared;
    p->mem1 = mem1;
    p->backing = shared ? depth_storage.data() : reinterpret_cast<void *>(uintptr_t(key) * 64 + 0x10000);
    used(*p);
    Surface *s = p.get();
    surfaces.push_back(std::move(p));
    ++allocations;
    return s;
}
static Surface *reuse_or_append(u32 key) {
    // Only allocation is a fixture stand-in; matching and marking use are production code.
    if (auto *s = get_surface(0x18000000 + key * 0x1000, 400, 240, 0, true)) return s;
    return append(key);
}

static void churn() {
    reset();
    size_t peak = 0;
    for (unsigned frame = 0; frame < 6000; ++frame) {
        use_counter = frame;
        depth_owner = append(frame + 1);
        forget_unused();
        peak = std::max(peak, surfaces.size());
    }
    printf("distinct shared-depth keys=6000, retained=%zu, peak_after_retire=%zu\n", surfaces.size(), peak);
    check(surfaces.size() == 1800, "shared descriptor churn must obey the existing 1800-present age");
    check(peak == 1800, "shared descriptor churn must plateau at one new descriptor per present");
    for (unsigned i = 0; i < 2000; ++i) {
        ++use_counter;
        forget_unused();
    }
    check(surfaces.size() == 1 && surfaces.front().get() == depth_owner, "only the current shared-block owner remains pinned");
    check(heap_frees == 0 && mem1_frees == 0 && shared_frees == 0, "retiring borrowed descriptors must never free backing storage");
    check(writebacks == 0, "shared depth retirement must not introduce image copies or readbacks");
    check(!state_sent_valid && state_lost, "retirement invalidates the sent GPU-state record");
}
static void pins_and_ownership() {
    reset();
    auto *owner = append(1);
    auto *depth = append(2);
    auto *color = append(3, false, false);
    auto *in_list = append(4, false, false);
    auto *transfer = append(5, false, false);
    auto *recent = append(6);
    auto *stale = append(7);
    depth_owner = owner;
    cur_depth = depth;
    cur_color = color;
    in_list->in_list = true;
    last_transfer[0].src = transfer;
    use_counter = 10000;
    recent->last_use = 9000;
    forget_unused();
    check(!contains(stale), "an inactive borrowed descriptor is retired");
    for (auto *p : {owner, depth, color, in_list, transfer, recent}) check(contains(p), "all active/current/list/transfer/recent pins remain");
    check(mem1_frees == 0 && heap_frees == 0, "first borrowed retirement does not free storage");
    cur_color = cur_depth = nullptr;
    in_list->in_list = false;
    for (auto &t : last_transfer) t = {};
    use_counter += 1800;
    for (unsigned i = 0; i < 10; ++i) forget_unused();
    check(surfaces.size() == 1 && contains(owner), "released pins retire while the shared owner remains valid");
    check(mem1_frees == 3 && heap_frees == 0 && shared_frees == 0, "only the three owned color allocations are freed once");

    reset();
    append(1, false, false, false)->gpu_newer = true;
    append(2, false, false, true);
    use_counter = 1800;
    forget_unused();
    check(surfaces.size() == 1 && heap_frees == 1 && writebacks == 1, "ordinary retirement preserves readback and one-per-present behavior");
    forget_unused();
    check(surfaces.empty() && mem1_frees == 1 && shared_frees == 0, "ordinary MEM1 ownership is freed once");
}
static void repeated_keys() {
    reset();
    for (unsigned frame = 0; frame < 6000; ++frame) {
        use_counter = frame;
        depth_owner = reuse_or_append(1 + frame % 8);
        forget_unused();
    }
    printf("repeated keys=8, frames=6000, descriptors=%zu, allocations=%u\n", surfaces.size(), allocations);
    check(surfaces.size() == 8 && allocations == 8, "production matching reuses repeated keys without descriptor growth");
    check(mem1_frees == 0 && heap_frees == 0 && writebacks == 0, "warm key reuse needs no retirement/free/readback");
}
int main() {
    churn();
    pins_and_ownership();
    repeated_keys();
    printf("surface descriptor lifetime: failures=%u\n", failures);
    return failures ? 1 : 0;
}
