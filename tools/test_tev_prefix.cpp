// Exercise shared shader normalization against the production scalar TEV evaluator.
#include <cstdio>
#include <cstring>
#include <string>

#include "shadergen.h"

using namespace gpu;

static unsigned failures, comparisons;
static u32 rng = 0x17a5c031;
static u32 random32() { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static void expect(bool ok, const char *message) {
    if (!ok && failures++ < 12) std::fprintf(stderr, "FAILED: %s\n", message);
}
static void emit(FILE *f, const DrawState &d) {
    const std::string raw = fs_key(d);
    const DrawState n = fs_normal(d);
    const std::string normal = fs_key(n);
    expect(fs_key(d) == raw, "normalization leaves caller unchanged");
    expect(fs_key(fs_normal(n)) == normal, "normalization is idempotent");
    expect(n.tev_count == d.tev_count && !std::memcmp(n.tev_list, d.tev_list, d.tev_count), "preserve physical stages and buffer advances");
    for (const std::string *key : {&raw, &normal}) {
        const u32 size = (u32)key->size();
        if (std::fwrite(&size, sizeof size, 1, f) != 1 || std::fwrite(key->data(), size, 1, f) != 1) std::abort();
    }
}
static bool differs(DrawState a, DrawState b, unsigned count) {
    bool different = false;
    for (unsigned i = 0; i < count; ++i) {
        a.tev_init_out = b.tev_init_out = random32();
        a.tev_init_buf = b.tev_init_buf = random32();
        a.tev_init_next = b.tev_init_next = random32();
        for (int j = 0; j < 6; ++j) a.tev[j].konst = b.tev[j].konst = random32();
        const u32 tex[4] = {random32(), random32(), random32(), random32()};
        const u32 primary = random32(), pri = random32(), sec = random32();
        different |= tev_combine(a, primary, pri, sec, tex) != tev_combine(b, primary, pri, sec, tex);
        ++comparisons;
    }
    return different;
}
static DrawState chain() {
    DrawState d{};
    d.need_shade = d.need_tex[0] = true;
    d.logic_op = 3;
    d.tev_count = 4;
    d.tev_list[0] = 0; d.tev_list[1] = 3; d.tev_list[2] = 4; d.tev_list[3] = 5;
    for (u32 i = 0; i < d.tev_count; ++i) {
        TevStageState &t = d.tev[d.tev_list[i]];
        t.cmul = t.amul = 1;
        t.csrc[0] = t.asrc[0] = i ? 15 : 3;
    }
    return d;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *records = std::fopen(argv[1], "wb"), *configs = std::fopen(argv[2], "rb");
    if (!records || !configs) return 2;
    // Two captured misses differ only in hardware blend factors; these contain FS state, no game code.
    u32 captured[] = {
        0x000006c3, 0x00004001, 0, 0, 4, 0, 0x33, 0, 0xee, 0x1211,
        3, 0xee, 0x1133, 0xee, 0x1111, 4, 0xee, 0x33, 0xff, 0x1188,
        5, 0, 0xff, 0xff, 0x1111, 0, 0, 0x00767601, 0xffffffff,
    };
    const u32 shipped[] = {
        0x403, 1, 0, 0, 4, 0, 0x33, 0, 0, 0x1100,
        3, 0xee, 0x1133, 0, 0x1111, 4, 0xee, 0x33, 0xff, 0x1188,
        5, 0, 0xff, 0, 0x1111, 0, 0, 0x03000000, 0,
    };
    const DrawState existing = decode_key(std::string((const char *)shipped, sizeof shipped));
    for (u64 hash : {0x384fa8907a63cf11ull, 0xf1659ffbd1d1df51ull}) {
        const std::string key((const char *)captured, sizeof captured);
        expect(fnv64(key.data(), key.size()) == hash, "fixture matches captured shader key");
        const DrawState d = decode_key(key), n = fs_normal(d);
        emit(records, d);
        expect(fs_key(n) == fs_key(existing), "dead arithmetic reuses shipped shader 32 key");
        expect(gen_fs_glsl(n) == gen_fs_glsl(existing), "dead arithmetic reuses shipped shader 32 source");
        expect(!differs(d, n, 5000), "captured production pixel outputs match");
        captured[27] = 0x00161601;
    }
    for (unsigned i = 0; i < 1000; ++i) {
        DrawState d = chain();
        TevStageState &first = d.tev[0], &next = d.tev[3];
        const u8 ordinary[] = {0, 1, 2, 3, 4, 5, 6, 8, 9};
        first.cop = ordinary[random32() % 9]; first.aop = ordinary[random32() % 9];
        if (first.aop == 6) first.aop = 5;
        first.cmul = 1 << (random32() % 3); first.amul = 1 << (random32() % 3);
        for (int j = 0; j < 3; ++j) {
            first.csrc[j] = random32() % 16; first.asrc[j] = random32() % 16;
            first.cmod[j] = random32() % 14; first.amod[j] = random32() % 8;
            const u8 external[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14};
            next.csrc[j] = external[random32() % 14]; next.asrc[j] = external[random32() % 14];
            next.cmod[j] = random32() % 14; next.amod[j] = random32() % 8;
        }
        next.cop = random32() % 10; next.aop = random32() % 10;
        next.cmul = 1 << (random32() % 3); next.amul = 1 << (random32() % 3);
        const DrawState n = fs_normal(d);
        emit(records, d);
        expect(n.tev[0].cop == 0 && n.tev[0].aop == 0 && n.tev[0].cmul == 1 && n.tev[0].amul == 1, "canonicalize dead first arithmetic");
        expect(!differs(d, n, 128), "random production pixel outputs match");
    }
    for (int kind = 0; kind < 12; ++kind) {
        DrawState a = chain();
        a.tev[3].csrc[0] = a.tev[3].asrc[0] = 3;
        if (kind == 0) a.tev[3].csrc[0] = 15;
        if (kind == 1) a.tev[3].asrc[0] = 15;
        if (kind == 2) a.tev[3].csrc[0] = 15, a.tev[3].cmod[0] = 2;
        if (kind == 3) a.tev[0].upd_rgb = true, a.tev[4].csrc[0] = 13;
        if (kind == 4) a.tev[0].upd_a = true, a.tev[4].asrc[0] = 13;
        if (kind == 5) a.tev[3].csrc[0] = 13;
        if (kind == 6) a.tev_count = 1;
        if (kind == 7) a.tev[0].cop = 7;
        if (kind == 8) a.tev[0].aop = 6;
        if (kind == 9) a.tev[0].aop = 7;
        if (kind == 10) a.tev[3].upd_rgb = true;
        if (kind == 11) a.tev[5].upd_a = true;
        DrawState b = a;
        b.tev[0].cop = b.tev[0].aop = 1;
        b.tev[0].csrc[1] = b.tev[0].asrc[1] = 0;
        emit(records, a); emit(records, b);
        expect(fs_key(fs_normal(a)) != fs_key(fs_normal(b)), "previous/buffer/write/single/special-alpha gates keep distinct keys");
        if (kind < 7 && kind != 5) expect(differs(a, b, 256), "active negative control changes actual pixels");
        expect(!differs(a, fs_normal(a), 128) && !differs(b, fs_normal(b), 128), "negative-control normalization preserves pixels");
    }
    // Dependencies can live in ADD's second operand or INTERPOLATE's third, in either channel.
    for (u8 dependency : {13, 15})
        for (int operand : {1, 2})
            for (bool alpha : {false, true}) {
                DrawState a = chain();
                TevStageState &next = a.tev[3];
                next.csrc[0] = next.asrc[0] = 3;
                next.csrc[1] = next.asrc[1] = 0;
                if (alpha) next.aop = operand == 1 ? 2 : 4, next.asrc[operand] = dependency;
                else next.cop = operand == 1 ? 2 : 4, next.csrc[operand] = dependency;
                DrawState b = a;
                b.tev[0].cop = b.tev[0].aop = 1;
                b.tev[0].csrc[1] = b.tev[0].asrc[1] = 0;
                emit(records, a); emit(records, b);
                expect(fs_key(fs_normal(a)) != fs_key(fs_normal(b)), "consumed operand 1/2 dependency keeps distinct keys");
                if (dependency == 15) expect(differs(a, b, 256), "later previous operand changes actual pixels");
                expect(!differs(a, fs_normal(a), 128) && !differs(b, fs_normal(b), 128), "later-operand normalization preserves pixels");
            }
    u32 size;
    while (std::fread(&size, sizeof size, 1, configs) == 1) {
        if (!size || size > FS_KEY_WORDS * 4 || size % 4) return 2;
        std::string key(size, '\0');
        if (std::fread(key.data(), size, 1, configs) != 1) return 2;
        emit(records, decode_key(key));
    }
    if (std::fclose(records) || std::fclose(configs)) return 2;
    std::printf("TEV prefix: %u pixel comparisons, 20 negative controls, %u failures\n", comparisons, failures);
    return failures ? 1 : 0;
}
