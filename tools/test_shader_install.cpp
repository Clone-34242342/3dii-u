#include "latte_program.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using gpu::unburst;
constexpr size_t GX2_SHADER_PROGRAM_ALIGNMENT = 256;
static std::set<void *> owned;
static bool fail_allocation;
static unsigned allocations, frees;
static void *tracked_alloc(size_t, size_t size) {
    if (fail_allocation) return nullptr;
    void *p = std::malloc(size);
    assert(p && owned.insert(p).second);
    ++allocations;
    return p;
}
static void tracked_free(void *p) {
    assert(owned.erase(p) == 1);
    ++frees;
    std::free(p);
}
struct Shader { void *program; u32 size; };
struct Entry { Shader *shader; };
struct Done { bool from_gfd; };

static void expand(Entry &v, const Done &d, bool in_cemu) {
    (void)d;
#define memalign tracked_alloc
#define free tracked_free
#include "shader_install_production.inc"
#undef free
#undef memalign
}

int main() {
    unsigned cases = 0;
    for (bool cemu : {false, true})
    for (bool cached : {false, true})
    for (bool burst : {false, true})
    for (bool failed : {false, true}) {
        // A burst export followed by END_PROGRAM; the normal form is unchanged.
        std::vector<u8> program(16);
        gpu::latte::set_word(program.data(), 0, 0xc0000000);
        gpu::latte::set_word(program.data(), 1, 0x94200ffc | (burst ? 1u << 17 : 0));
        gpu::latte::set_word(program.data(), 2, 0);
        gpu::latte::set_word(program.data(), 3, 0);
        const u32 words = unburst(program.data(), (u32)program.size() / 4, nullptr);
        assert((words != 0) == burst);
        std::vector<u8> expected(words * 4);
        if (words) unburst(program.data(), (u32)program.size() / 4, expected.data());
        void *const original = tracked_alloc(256, program.size());
        std::memcpy(original, program.data(), program.size());
        Shader shader{original, (u32)program.size()};
        Entry v{&shader};
        const unsigned prior_frees = frees;
        fail_allocation = failed;
        expand(v, Done{cached}, cemu);
        fail_allocation = false;
        const bool replaced = cemu && burst && !failed;
        assert((shader.program != original) == replaced);
        assert(frees == prior_frees + (replaced && cached));
        if (replaced) {
            assert(shader.size == expected.size());
            assert(!std::memcmp(shader.program, expected.data(), expected.size()));
        } else {
            assert(shader.size == program.size());
            assert(!std::memcmp(shader.program, program.data(), program.size()));
        }
        tracked_free(shader.program);
        if (replaced && !cached) tracked_free(original);  // compiler owner's teardown
        assert(owned.empty());
        ++cases;
    }
    assert(allocations == frees);
    std::printf("shader install: %u ownership/failure/platform cases passed\n", cases);
}
