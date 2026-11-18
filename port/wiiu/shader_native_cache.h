// Bounds checked cache envelopes for CafeGLSL's GFD writer and WUT's unsized reader.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "latte_program.h"

namespace shader_native_cache {
constexpr uint32_t kVersion = 1;
// Bump when the GX2 layout, compiler patches (fit_program), or cache interpretation changes.
constexpr uint32_t kAbi = 1;
constexpr size_t kHeader = 72, kMaxFile = 2u << 20;
constexpr uint64_t kDiskBudget = 16u << 20;
inline bool fits_budget(uint64_t used, uint64_t incoming) { return used <= kDiskBudget && incoming <= kDiskBudget - used; }
struct Identity {
    uint32_t generator;
    uint64_t set, compiler, key, source;
    bool vs;
    uint32_t inputs;
};
inline uint32_t word(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t wide(const uint8_t *p) { return uint64_t(word(p)) << 32 | word(p + 4); }
inline void put(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; } }
inline void put64(uint8_t *p, uint64_t v) { put(p, uint32_t(v >> 32)); put(p + 4, uint32_t(v)); }
inline uint64_t hash(const void *data, size_t size) {
    auto p = static_cast<const uint8_t *>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
inline bool range(size_t at, size_t count, size_t size) { return at <= size && count <= size - at; }

// Bound the writer before it creates its temporary file. The later parser also
// checks the actual bytes; this conservative estimate includes names and patches.
template<class Shader> bool writer_fits(const Shader &s) {
    if (!s.program || s.size < 8 || s.size > (1u << 20) || s.size % 8) return false;
    uint64_t header = sizeof(Shader) + 40;
    const auto array = [&](uint32_t count, const auto *vars) {
        if (count > 4096 || (count && !vars)) return false;
        header += 4 + uint64_t(count) * sizeof(*vars);
        if (header > 65536) return false;
        if constexpr (requires { vars[0].name; }) for (uint32_t i = 0; i < count; ++i) {
            if (!vars[i].name) return false;
            const size_t n = strnlen(vars[i].name, 65536);
            header += n + 8;  // terminator, alignment and name relocation
            if (header > 65536) return false;
        }
        return true;
    };
    if (!array(s.uniformBlockCount, s.uniformBlocks) || !array(s.uniformVarCount, s.uniformVars) ||
        !array(s.initialValueCount, s.initialValues) || !array(s.loopVarCount, s.loopVars) || !array(s.samplerVarCount, s.samplerVars)) return false;
    if constexpr (requires { s.attribVarCount; }) if (!array(s.attribVarCount, s.attribVars)) return false;
    return header + s.size + 128 <= kMaxFile - kHeader;
}

// Only the single-shader layout WriteGFD emits is accepted. In particular every
// relocation must name an actual pointer field, with bounded arrays and strings.
inline bool shader_header(const uint8_t *p, size_t n, bool vs, uint32_t program_size) {
    const uint32_t fixed = vs ? 0x134 : 0xe8, size_at = vs ? 0xd0 : 0xa4;
    if (n < fixed + 40 || n > 65536 || n % 4 || word(p + size_at) != program_size || word(p + size_at + 4) || word(p + size_at + 8) != 1) return false;
    const uint32_t regs = word(p) & 255;
    if (regs > gpu::shader_budget::kOrdinary) return false;
    if (vs ? (word(p + 12) > 10 || word(p + 64) > 32 || word(p + 0x110)) : word(p + 16) > 32) return false;
    const uint8_t *r = p + n - 40;
    const uint32_t patch_count = word(r + 32), patch_tag = word(r + 36), patch_at = patch_tag & 0xfffff;
    if (word(r) != 0x7d424c4b || word(r + 4) != 40 || (patch_tag & 0xfff00000) != 0xd0600000 || patch_at < fixed || patch_at % 4 || patch_count > (n - 40) / 4 ||
        !range(patch_at, patch_count * 4, n - 40) || patch_at + patch_count * 4 != n - 40) return false;
    std::vector<uint32_t> expected;
    const auto pointer = [&](uint32_t at, uint32_t tag, uint32_t &offset) {
        const uint32_t v = word(p + at);
        offset = v & 0xfffff;
        if ((v & 0xfff00000) != tag || offset < fixed || offset >= patch_at) return false;
        expected.push_back(at);
        return true;
    };
    const uint32_t widths[] = {12, 20, 20, 8, 12, 16};
    const bool named[] = {true, true, false, false, true, true};
    for (uint32_t a = 0; a < (vs ? 6u : 5u); ++a) {
        const uint32_t count_at = size_at + 12 + a * 8, count = word(p + count_at), ptr_at = count_at + 4;
        if (!count) { if (word(p + ptr_at)) return false; continue; }
        uint32_t offset;
        if (count > 4096 || !pointer(ptr_at, 0xd0600000, offset) || offset % 4 || !range(offset, uint64_t(count) * widths[a], patch_at)) return false;
        if (named[a]) for (uint32_t i = 0; i < count; ++i) {
            uint32_t text;
            if (!pointer(offset + i * widths[a], 0xca700000, text) || !memchr(p + text, 0, patch_at - text)) return false;
        }
    }
    // The writer stores no host pointers in program or GX2R buffer fields.
    for (uint32_t at = fixed - 16; at < fixed; at += 4) if (word(p + at)) return false;
    std::sort(expected.begin(), expected.end());
    if (std::adjacent_find(expected.begin(), expected.end()) != expected.end() || expected.size() != patch_count) return false;
    std::vector<uint32_t> actual;
    for (uint32_t i = 0; i < patch_count; ++i) {
        const uint32_t v = word(p + patch_at + i * 4), tag = v & 0xfff00000, at = v & 0xfffff;
        if ((tag != 0xd0600000 && tag != 0xca700000) || at % 4 || !range(at, 4, patch_at)) return false;
        actual.push_back(at);
    }
    std::sort(actual.begin(), actual.end());
    return actual == expected;
}

inline bool valid_gfd(const uint8_t *p, size_t n, bool vs) {
    if (n < 128 || n > kMaxFile - kHeader || word(p) != 0x47667832 || word(p + 4) != 32 || word(p + 8) != 7 || word(p + 12) != 1) return false;
    const uint8_t *header = nullptr, *program = nullptr;
    size_t header_size = 0;
    uint32_t program_size = 0;
    bool ended = false;
    for (size_t at = 32; at < n;) {
        if (!range(at, 32, n) || word(p + at) != 0x424c4b7b || word(p + at + 4) != 32 || word(p + at + 8) != 1) return false;
        const uint32_t kind = word(p + at + 16), size = word(p + at + 20);
        at += 32;
        if (size % 4 || !range(at, size, n)) return false;
        if (kind == 1) { if (size || at != n) return false; ended = true; break; }
        if (kind == (vs ? 3u : 6u)) {
            if (header || program_size || size % 4) return false;
            header = p + at; header_size = size;
        } else if (kind == (vs ? 5u : 7u)) {
            if (!header || program_size || size < 8 || size % 8 || size > (1u << 20)) return false;
            if (!vs && gpu::latte::hollow_pixel_program(p + at, size)) return false;
            program = p + at; program_size = size;
        } else if (kind != 2) return false;  // optional alignment padding only
        at += size;
    }
    if (!ended || !header || !program_size || !shader_header(header, header_size, vs, program_size)) return false;
    // Zero metadata is only valid when the production scanner proves no ordinary references or repairs.
    uint32_t required;
    return (word(header) & 255) || (gpu::program_registers(program, program_size, 0, required) && !required);
}

inline bool decode(const std::vector<uint8_t> &file, const Identity &id, const uint8_t *&gfd, size_t &size) {
    gfd = nullptr; size = 0;
    if (file.size() < kHeader || file.size() > kMaxFile) return false;
    const uint8_t *p = file.data();
    if (word(p) != 0x4e534843 || word(p + 4) != kVersion || word(p + 8) != id.generator || word(p + 12) != kAbi ||
        wide(p + 16) != id.set || wide(p + 24) != id.compiler || wide(p + 32) != id.key || wide(p + 40) != id.source ||
        word(p + 48) != uint32_t(id.vs) || word(p + 52) != id.inputs || word(p + 60) || word(p + 56) != file.size() - kHeader) return false;
    size = file.size() - kHeader;
    if (wide(p + 64) != hash(p + kHeader, size) || !valid_gfd(p + kHeader, size, id.vs)) { size = 0; return false; }
    gfd = p + kHeader;
    return true;
}

inline std::vector<uint8_t> encode(const std::vector<uint8_t> &gfd, const Identity &id) {
    if (!valid_gfd(gfd.data(), gfd.size(), id.vs)) return {};
    std::vector<uint8_t> file(kHeader + gfd.size());
    uint8_t *p = file.data();
    put(p, 0x4e534843); put(p + 4, kVersion); put(p + 8, id.generator); put(p + 12, kAbi);
    put64(p + 16, id.set); put64(p + 24, id.compiler); put64(p + 32, id.key); put64(p + 40, id.source);
    put(p + 48, id.vs); put(p + 52, id.inputs); put(p + 56, uint32_t(gfd.size()));
    put64(p + 64, hash(gfd.data(), gfd.size()));
    memcpy(p + kHeader, gfd.data(), gfd.size());
    return file;
}
}  // namespace shader_native_cache
