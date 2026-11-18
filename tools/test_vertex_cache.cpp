// Exercise the cache and list boundary extracted from gx2_backend.cpp, not a model of them.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
constexpr bool kMeasure = true;
bool use_vtx_cache = true, vtx_drop_all, fail_alloc;
u64 st_flips, hash_calls, hash_reads, flush_bytes, fill_calls;
std::vector<u8> ring(64u << 20);
u32 ring_used;
void *ring_alloc(u32 size, u32 align) {
    ring_used = (ring_used + align - 1) & ~(align - 1);
    assert(ring_used + size <= ring.size());
    void *p = ring.data() + ring_used; ring_used += size; return p;
}
void *memalign(u32, u32 size) { return fail_alloc ? nullptr : malloc(size); }
void DCFlushRange(void *, u32 size) { flush_bytes += size; }
u16 le16(u16 x) { return x; }
u64 production_hash_bytes(const u8 *, size_t);
u64 hash_bytes(const u8 *src, size_t size) { hash_calls++; hash_reads += size; return production_hash_bytes(src, size); }

struct Surface { bool in_list, was_sampled; u32 addr; u32 bytes() const { return 0; } };
std::vector<Surface *> read_targets;
namespace surface_readback { int owned(const Surface &, int) { return 0; } }
int surfaces;
void write_back(Surface &, int) {}
void mark_textures(u32, u32, bool) {}
void GX2Flush() {}

#include "vertex_cache_production.inc"

void clear() {
    for (auto &entry : vtx_cache) free(entry.second.buf);
    for (auto &entry : vtx_retired) free(entry.first);
    vtx_cache.clear(); vtx_retired.clear(); ring_used = 0;
    vtx_cache_size = st_vtx_hits = st_vtx_misses = 0;
    hash_calls = hash_reads = flush_bytes = fill_calls = 0;
    fail_alloc = false; use_vtx_cache = true;
}

const u8 *copy(u32 addr, const u8 *src, u32 count = 4, u32 stride = 16) {
    return (const u8 *)vtx_cached(addr, src, stride, count, 0x80, count * stride,
        [=](void *out, const u8 *from) { fill_calls++; memcpy(out, from, count * stride); }, count * stride);
}

int main(int argc, char **) {
    std::vector<u8> src(128);
    for (size_t i = 0; i < src.size(); i++) src[i] = (u8)i;
    const u8 *old = copy(0x20001000, src.data());
    assert(memcmp(old, src.data(), 64) == 0);
    const u64 hashes = hash_calls, fills = fill_calls;
    for (int draw = 0; draw < 10; draw++) assert(copy(0x20001000, src.data()) == old);
    assert(hash_calls == hashes && fill_calls == fills);  // repeated draws do not scan or upload again
    gx2_finish_list();
    src[31] ^= 0xff;  // ordinary guest store, no cache maintenance, no flip
    const u8 *next = copy(0x20001000, src.data());
    assert(next != old && memcmp(next, src.data(), 64) == 0);
    assert(old[31] == 31);  // a pending GPU draw retains its immutable copy
    use_vtx_cache = false;
    assert(memcmp(next, copy(0x20001000, src.data()), 64) == 0);
    use_vtx_cache = true;
    gx2_finish_list();
    const u64 before_static = hash_calls;
    assert(copy(0x20001000, src.data()) == next);
    assert(hash_calls == before_static + 1 && fill_calls == fills + 2);  // hash once, no new conversion
    vtx_invalidate(0x20001040, 16);  // adjacent range
    assert(copy(0x20001000, src.data()) == next);
    src[63] ^= 0x80;
    vtx_invalidate(0x2000103f, 1);  // last byte of source
    assert(memcmp(copy(0x20001000, src.data()), src.data(), 64) == 0);
    assert(next[63] == 63);
    // Explicit flush/DMA notifications also invalidate after the first draw in the same list.
    const u8 *flushed = copy(0x20001000, src.data());
    src[0] ^= 0x40; vtx_invalidate(0x20001000, 1);
    assert(copy(0x20001000, src.data())[0] == src[0] && flushed[0] == 0);
    assert(!vtx_retired.empty());
    st_flips += 3; vtx_cache_flip();
    assert(!vtx_retired.empty());
    st_flips++; vtx_cache_flip();
    assert(vtx_retired.empty());

    clear();
#ifdef VTX_LIST_GENERATION
    vtx_list = 0;
    const u8 *wrap_old = copy(0x20001000, src.data());
    copy(0x20002000, src.data() + 64);
    vtx_list = UINT32_MAX;
    assert(copy(0x20001000, src.data()) == wrap_old);  // another entry still has the ancient generation zero
    gx2_finish_list();
    assert(vtx_list == 0 && vtx_cache.empty());  // a wrap cannot mistake that ancient entry for a current check
    src[1] ^= 0x80; src[65] ^= 0x40;
    assert(copy(0x20001000, src.data())[1] == src[1] && wrap_old[1] != src[1]);
    assert(copy(0x20002000, src.data() + 64)[1] == src[65]);
    clear();
#endif
    u32 lo, hi;
    u8 indices[48] = {2, 0, 3, 1, 0, 2};
    const u16 *indices_old = draw_indices(0x20002000, indices, 6, false, lo, hi);
    assert(lo == 0 && hi == 3 && indices_old[2] == 3);
    gx2_finish_list(); indices[2] = 7;
    const u16 *indices_new = draw_indices(0x20002000, indices, 6, false, lo, hi);
    assert(lo == 0 && hi == 7 && indices_new[2] == 7 && indices_old[2] == 3);
    use_vtx_cache = false;
    const u16 *indices_off = draw_indices(0x20002000, indices, 6, false, lo, hi);
    assert(memcmp(indices_new, indices_off, 16) == 0 && lo == 0 && hi == 7);
    clear();
    u16 wide[24] = {5, 9, 2, 9};
    draw_indices(0x20003000, (const u8 *)wide, 4, true, lo, hi);
    assert(lo == 2 && hi == 9);
    gx2_finish_list(); wide[2] = 12;
    draw_indices(0x20003000, (const u8 *)wide, 4, true, lo, hi);
    assert(lo == 5 && hi == 12);
    clear();
    fail_alloc = true;
    assert(memcmp(copy(0x20001000, src.data()), src.data(), 64) == 0 && vtx_cache.empty());
    fail_alloc = false;
    copy(0x20001000, src.data());
    vtx_drop_all = true; vtx_cache_flip();
    assert(vtx_cache.empty() && !vtx_retired.empty());
    clear();
    puts("vertex cache: list reuse, unannounced writes, indices, overlap, retirement, fallback and cache off agree");

    if (argc > 1) {
        // 256 arrays of 3000 16-byte vertices, four draws each: 46.9 MiB copied per list with cache off.
        constexpr u32 arrays = 256, count = 3000, stride = 16, lists = 20;
        std::vector<u8> geometry(arrays * count * stride + 16, 0x53);
        for (bool enabled : {false, true}) {
            clear(); use_vtx_cache = enabled;
            const auto begin = std::chrono::steady_clock::now();
            for (u32 list = 0; list < lists; list++) {
                for (u32 a = 0; a < arrays; a++)
                    for (u32 draw = 0; draw < 4; draw++) {
                        const u32 offset = a * count * stride;
                        assert(copy(0x20000000 + offset, geometry.data() + offset, count, stride)[17] == 0x53);
                    }
                gx2_finish_list(); st_flips++; vtx_cache_flip();
                ring_used = 0;
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / lists;
            printf("static geometry cache %s: %.3f ms/list, %.3f MiB hashed/list, %.3f MiB copied+flushed/list, %.1f%% hits\n",
                enabled ? "on" : "off", ms, hash_reads / (1048576.0 * lists), flush_bytes / (1048576.0 * lists),
                100.0 * st_vtx_hits / std::max<u64>(1, st_vtx_hits + st_vtx_misses));
        }
        for (bool enabled : {false, true}) {
            clear(); use_vtx_cache = enabled;
            constexpr u32 icon_lists = 2000;
            const auto begin = std::chrono::steady_clock::now();
            for (u32 list = 0; list < icon_lists; list++) {
                src[31]++;
                for (u32 draw = 0; draw < 100; draw++) assert(copy(0x20001000, src.data())[31] == src[31]);
                gx2_finish_list(); st_flips++; vtx_cache_flip(); ring_used = 0;
            }
            printf("mutating UI cache %s: %.3f us/list, %.0f bytes hashed/list, %.0f bytes copied+flushed/list\n",
                enabled ? "on" : "off", std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / icon_lists,
                (double)hash_reads / icon_lists, (double)flush_bytes / icon_lists);
        }
        clear();
    }
}
