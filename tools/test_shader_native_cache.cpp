#include "shader_native_cache.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace nc = shader_native_cache;
using Bytes = std::vector<uint8_t>;
struct Named { const char *name; uint32_t value; };
struct Plain { uint32_t value; };
struct Shader {
    void *program; uint32_t size;
    uint32_t uniformBlockCount = 0, uniformVarCount = 0, initialValueCount = 0, loopVarCount = 0, samplerVarCount = 0, attribVarCount = 0;
    Named *uniformBlocks = nullptr, *uniformVars = nullptr, *samplerVars = nullptr, *attribVars = nullptr;
    Plain *initialValues = nullptr, *loopVars = nullptr;
};

// Minimal writer-compatible shader, with a named uniform block and relocation
// table. Machine instructions are checked separately by the existing latte test.
static Bytes fixture(bool vs) {
    const uint32_t fixed = vs ? 0x134 : 0xe8, size_at = vs ? 0xd0 : 0xa4;
    Bytes header(fixed + 12 + 4 + 8 + 40);
    nc::put(header.data(), 4);
    nc::put(header.data() + size_at, 24);
    nc::put(header.data() + size_at + 8, 1);
    nc::put(header.data() + size_at + 12, 1);
    nc::put(header.data() + size_at + 16, 0xd0600000 | fixed);
    nc::put(header.data() + fixed, 0xca700000 | (fixed + 12));
    header[fixed + 12] = 'u';
    const uint32_t patch = fixed + 16;
    nc::put(header.data() + patch, 0xd0600000 | (size_at + 16));
    nc::put(header.data() + patch + 4, 0xca700000 | fixed);
    auto r = header.data() + header.size() - 40;
    nc::put(r, 0x7d424c4b); nc::put(r + 4, 40);
    nc::put(r + 32, 2); nc::put(r + 36, 0xd0600000 | patch);
    Bytes gfd(32);
    nc::put(gfd.data(), 0x47667832); nc::put(gfd.data() + 4, 32);
    nc::put(gfd.data() + 8, 7); nc::put(gfd.data() + 12, 1);
    auto block = [&](uint32_t kind, const Bytes &data) {
        const size_t at = gfd.size(); gfd.resize(at + 32 + data.size());
        auto p = gfd.data() + at;
        nc::put(p, 0x424c4b7b); nc::put(p + 4, 32); nc::put(p + 8, 1);
        nc::put(p + 16, kind); nc::put(p + 20, uint32_t(data.size()));
        if (!data.empty()) memcpy(p + 32, data.data(), data.size());
    };
    block(vs ? 3 : 6, header); block(vs ? 5 : 7, Bytes(24)); block(1, {});
    return gfd;
}

int main(int argc, char **argv) {
    for (bool vs : {false, true}) {
        const auto gfd = fixture(vs);
        nc::Identity id{2, 0x1122334455667788ull, 0x2233445566778899ull, 7, 9, vs, 3};
        const auto file = nc::encode(gfd, id);
        const uint8_t *payload = nullptr; size_t size = 0;
        assert(!file.empty() && nc::decode(file, id, payload, size));
        assert(size == gfd.size() && !memcmp(payload, gfd.data(), size));
        for (size_t n = 0; n < file.size(); ++n) {
            const Bytes short_file(file.begin(), file.begin() + n);
            assert(!nc::decode(short_file, id, payload, size));
        }
        // Every envelope field and payload byte is protected, including high
        // halves of 64-bit keys and source/compiler identities.
        for (size_t i = 0; i < file.size(); ++i) {
            auto bad = file; bad[i] ^= 0x80;
            assert(!nc::decode(bad, id, payload, size));
        }
        auto wrong = id; wrong.source++;
        assert(!nc::decode(file, wrong, payload, size));
        wrong = id; wrong.vs = !vs;
        assert(!nc::decode(file, wrong, payload, size));
        auto bad = file; bad.push_back(0);
        assert(!nc::decode(bad, id, payload, size));
        assert(!nc::decode(Bytes(nc::kMaxFile + 1), id, payload, size));

        // These malformed GFDs must fail structural validation even with a
        // matching outer hash: an unsized WHB reader must never see them.
        const uint32_t fixed = vs ? 0x134 : 0xe8, size_at = vs ? 0xd0 : 0xa4;
        const size_t header = 64, patch = header + fixed + 16;
        const size_t footer = patch + 8, program_block = footer + 40;
        const std::pair<size_t, uint32_t> corruptions[] = {
            {4, 0xffffffff}, {8, 8}, {32 + 20, 0xfffffffc},
            {header + size_at, 32}, {header + size_at + 4, 1}, {header + size_at + 8, 0},
            {header, 0}, {header, 255}, {header + fixed - 4, 1},
            {header + size_at + 12, 0xffffffff}, {header + size_at + 16, 0xd06ffffc},
            {header + fixed, 0xca7fffff}, {header + fixed, 0xd0600000 | (fixed + 12)},
            {patch, 0xd0600000 | (size_at + 4)}, {patch + 4, 0xd0600000 | (size_at + 16)},
            {footer, 0}, {footer + 4, 0}, {footer + 32, 0x40000002}, {footer + 36, 0xd06ffffc},
            {program_block + 16, vs ? 7u : 5u}, {program_block + 20, 0xfffffffc},
            {program_block + 32 + 24 + 16, 2},
        };
        for (const auto &c : corruptions) {
            auto broken = gfd; nc::put(broken.data() + c.first, c.second);
            assert(!nc::valid_gfd(broken.data(), broken.size(), vs));
            assert(nc::encode(broken, id).empty());
            bad = file; memcpy(bad.data() + nc::kHeader, broken.data(), broken.size());
            nc::put64(bad.data() + 64, nc::hash(broken.data(), broken.size()));
            assert(!nc::decode(bad, id, payload, size));
        }
        auto unterminated = gfd;
        memset(unterminated.data() + header + fixed + 12, 'x', 4);
        assert(!nc::valid_gfd(unterminated.data(), unterminated.size(), vs));
    }
    assert(nc::fits_budget(0, nc::kDiskBudget));
    assert(nc::fits_budget(nc::kDiskBudget, 0));
    assert(!nc::fits_budget(nc::kDiskBudget, 1));
    assert(!nc::fits_budget(UINT64_MAX, 1));
    assert(!nc::fits_budget(1, UINT64_MAX));
    {
        // An old cache can be intact and correctly hashed yet hold the compiler's
        // masked-export failure. Reject before GFD loading; valid tiny PS survive.
        auto gfd = fixture(false);
        const size_t header_size = nc::word(gfd.data() + 32 + 20);
        const size_t program_block = 64 + header_size, program = program_block + 32;
        nc::put(gfd.data() + 64 + 0xa4, 8);
        nc::put(gfd.data() + program_block + 20, 8);
        gfd.erase(gfd.begin() + program + 8, gfd.begin() + program + 24);
        gpu::latte::set_word(gfd.data() + program, 0, 0xc0000000);
        gpu::latte::set_word(gfd.data() + program, 1, 0x94200ffc);
        const nc::Identity id{2, 1, 2, 3, 4, false, 0};
        auto file = nc::encode(gfd, id);
        const uint8_t *payload = nullptr; size_t size = 0;
        assert(!file.empty() && nc::decode(file, id, payload, size));
        gpu::latte::set_word(gfd.data() + program, 1, 0x94200fff);
        assert(nc::encode(gfd, id).empty());  // not persisted by native_save
        memcpy(file.data() + nc::kHeader, gfd.data(), gfd.size());
        nc::put64(file.data() + 64, nc::hash(gfd.data(), gfd.size()));
        assert(!nc::decode(file, id, payload, size));  // native_load falls through to compile
    }
    Named named{"uniform", 0};
    Shader shader{&named, 24};
    shader.uniformBlocks = &named; shader.uniformBlockCount = 1;
    assert(nc::writer_fits(shader));
    shader.size = (1u << 20) + 8; assert(!nc::writer_fits(shader));
    shader.size = 24; shader.uniformBlockCount = UINT32_MAX; assert(!nc::writer_fits(shader));
    shader.uniformBlockCount = 1; shader.uniformBlocks = nullptr; assert(!nc::writer_fits(shader));
    shader.uniformBlocks = &named;
    std::string long_name(65536, 'x'); named.name = long_name.c_str(); assert(!nc::writer_fits(shader));
    // Optional local writer-output smoke check; no game shader data is committed.
    if (argc == 3) {
        std::ifstream in(argv[1], std::ios::binary);
        Bytes gfd((std::istreambuf_iterator<char>(in)), {});
        assert(nc::valid_gfd(gfd.data(), gfd.size(), argv[2][0] == 'v'));
    }
    puts("native shader cache: envelope, GFD relocation bounds, truncation and disk budget passed");
}
