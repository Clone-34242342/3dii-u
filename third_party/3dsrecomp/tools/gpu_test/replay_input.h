// Strict input checks shared by both replay passes. The capture remains the existing native V1 format.
#pragma once
#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace replay {
enum Kind { Cmdlist, Fill, Transfer, Copy, Present };
struct Op { Kind kind; std::array<u32, 6> p{}; };
inline void require(bool good, const std::string &why) {
    if (!good) throw std::runtime_error(why);
}
inline u32 number(const std::string &s, int base) {
    const char *first = s.data(), *last = first + s.size();
    if (base == 16 && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) first += 2;
    u32 value = 0;
    auto parsed = std::from_chars(first, last, value, base);
    require(first != last && parsed.ec == std::errc{} && parsed.ptr == last, "invalid integer: " + s);
    return value;
}
inline bool range(u32 address, u64 size) {
    if (!size) return false;
    for (auto region : {std::pair<u32, u32>{mem::FCRAM_PADDR, mem::LINEAR_SIZE},
                        std::pair<u32, u32>{mem::VRAM_PADDR, mem::VRAM_SIZE}})
        if (address >= region.first && (u64)address < (u64)region.first + region.second &&
            size <= (u64)region.first + region.second - address) return true;
    return false;
}
inline void exact_size(const std::string &path, size_t expected) {
    require(std::filesystem::file_size(path) == expected, path + ": wrong byte count");
}
inline void load_exact(const std::string &path, u8 *dst, size_t size) {
    exact_size(path, size);
    FILE *f = fopen(path.c_str(), "rb");
    require(f != nullptr, "cannot open " + path);
    bool ok = fread(dst, 1, size, f) == size;
    ok = fgetc(f) == EOF && !ferror(f) && ok;
    ok = fclose(f) == 0 && ok;
    require(ok, "short, changed or unreadable file: " + path);
    fprintf(stderr, "loaded %s (%zu bytes)\n", path.c_str(), size);
}
inline u64 image_bytes(u32 w, u32 h, u32 bpp, bool tiled) {
    require(w && h && bpp, "zero image dimension");
    // tiled_offset's last partial8x8 tile can extend beyond width*height.
    return tiled ? ((u64)((h - 1) & ~7u) * w + ((w - 1) & ~7u) * 8ull + 64) * bpp
                 : (u64)w * h * bpp;
}
inline void validate(const Op &op) {
    const auto &p = op.p;
    switch (op.kind) {
    case Cmdlist:
        require(!(p[0] & 3) && p[1] >= 8 && !(p[1] & 7) && range(p[0], p[1]), "invalid cmdlist range/alignment");
        break;
    case Fill:
        require(p[1] > p[0] && range(p[0], (u64)p[1] - p[0]), "invalid fill range");
        break;
    case Transfer: {
        const u32 iw = p[2] & 0xffff, ih = p[2] >> 16, ow0 = p[3] & 0xffff, oh0 = p[3] >> 16;
        const u32 inf = p[4] >> 8 & 7, outf = p[4] >> 12 & 7, scale = p[4] >> 24 & 3;
        require(!(p[4] & 8) && inf <= 4 && outf <= 4 && scale <= 2, "unsupported transfer flags");
        require(iw && ih && ow0 && oh0 && ow0 <= iw && oh0 <= ih, "invalid transfer dimensions");
        const u32 ow = ow0 >> (scale != 0), oh = oh0 >> (scale == 2);
        const bool linear = p[4] & 2, no_swizzle = p[4] & 0x20;
        const u64 src = image_bytes(iw, ih, gpu::pixel_bytes(inf), !linear);
        const u64 dst = image_bytes(ow, oh, gpu::pixel_bytes(outf), linear ? !no_swizzle : no_swizzle);
        const u64 offset = (p[4] & 5) == 5 ? (u64)(iw - ow0) * (oh0 - 1) * gpu::pixel_bytes(outf) : 0;
        require(range(p[0], src) && (u64)p[1] + offset <= UINT32_MAX && range(p[1] + (u32)offset, dst), "invalid transfer memory range");
        break;
    }
    case Copy:
        require(p[2] && !(p[2] & 15), "invalid copy size");
        for (int i = 0; i < 2; ++i) {
            const u64 gap = (p[3 + i] >> 16) * 16ull;
            const u64 width = gap ? (p[3 + i] & 0xffff) * 16ull : p[2];
            require(width != 0, "zero copy row width");
            const u64 bytes = p[2] + ((p[2] - 1) / width) * gap;
            require(bytes <= UINT32_MAX && range(p[i], bytes), "invalid copy memory range");
        }
        break;
    case Present: {
        const u32 fmt = p[4] & 7;
        require(fmt <= 4 && p[1] && p[2] && (u64)p[1] * p[2] <= mem::LINEAR_SIZE / sizeof(u32), "invalid present dimensions/format");
        const u64 row = (u64)p[1] * gpu::pixel_bytes(fmt);
        require(p[3] >= row && range(p[0], (u64)(p[2] - 1) * p[3] + row), "invalid present stride/range");
        break;
    }
    }
}
template<class F> void text_lines(const std::string &path, F consume) {
    require(std::filesystem::file_size(path) <= 16u * 1024 * 1024, path + ": text input too large");
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot open " + path);
    std::string line;
    size_t row = 0;
    while (std::getline(input, line)) {
        ++row;
        try {
            require(line.size() <= 511, "line too long");
            std::istringstream split(line);
            std::vector<std::string> words;
            for (std::string word; split >> word;) words.push_back(word);
            if (!words.empty()) consume(words);
        } catch (const std::exception &e) { throw std::runtime_error(path + ":" + std::to_string(row) + ": " + e.what()); }
    }
    require(input.eof() && !input.bad(), "cannot read " + path);
}
inline std::vector<Op> operations(const std::string &path) {
    std::vector<Op> result;
    size_t presents = 0;
    text_lines(path, [&](const std::vector<std::string> &w) {
        Op op;
        size_t count = 0;
        if (w[0] == "cmdlist") op.kind = Cmdlist, count = 2;
        else if (w[0] == "fill") op.kind = Fill, count = 4;
        else if (w[0] == "transfer") op.kind = Transfer, count = 5;
        else if (w[0] == "copy") op.kind = Copy, count = 6;
        else if (w[0] == "present") op.kind = Present, count = 5;
        else require(false, "unknown operation: " + w[0]);
        require(w.size() == count + 1, "wrong operation argument count");
        for (size_t i = 0; i < count; ++i) op.p[i] = number(w[i + 1], op.kind == Present && (i == 1 || i == 2) ? 10 : 16);
        validate(op);
        presents += op.kind == Present;
        result.push_back(op);
    });
    require(presents != 0, "ops.txt has no present output");
    return result;
}
inline std::vector<u32> presets(const std::string &path) {
    std::vector<u32> result;
    text_lines(path, [&](const std::vector<std::string> &w) {
        require(w.size() == 3 && w[0] == "reg", "expected reg <id> <value>");
        const u32 id = number(w[1], 16), value = number(w[2], 16);
        require(id < 0x300 && result.size() < 0x100000 / sizeof(u32), "preset register/scratch overflow");
        result.push_back(value); result.push_back(id | 0xfu << 16);
    });
    return result;
}
}  // namespace replay
