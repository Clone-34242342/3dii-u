#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
#define LOG(...) ((void)0)
namespace kernel {
#include "mii_id.inc"
#include "mii_console.inc"
}
#include "mii_reader.inc"

static bool load(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    const bool ok = read_db(f);
    fclose(f);
    return ok;
}

int main() {
    using kernel::g_host_miis;
    assert(load("fixtures/valid.dat"));
    const auto fixture = g_host_miis;
    assert(fixture.size() == 3);
    assert(fixture[0][0x0F] == 3);
    assert(fixture[1][0x0F] == 0 && fixture[1][0x11] == 1 && fixture[1][0x2E] == 128); // special Mii
    std::array<u8, 0x60> converted;
    assert(kernel::mii_3ds(fixture[1], converted));
    assert(fixture[2][0x0F] == 4 && fixture[2][0] == 3); // camera-created Mii
    assert(fixture[2][0x1A] == 'M' && fixture[2][0x1B] == 0);

    for (const char *bad : {"fixtures/truncated.dat", "fixtures/extra.dat", "fixtures/bad-crc.dat", "fixtures/bad-magic.dat"}) {
        assert(!load(bad));
        assert(g_host_miis == fixture);
    }
    assert(load("fixtures/empty.dat") && g_host_miis == fixture);
    g_host_miis.clear();
    assert(load("fixtures/full.dat") && g_host_miis.size() == 3000);
    const auto &last = g_host_miis.back();
    assert(last[0x2E] == 17 && last[0x2F] == 42); // 8-bit fields never swap
    const unsigned birthday_and_color = last[0x18] | last[0x19] << 8;
    assert(((birthday_and_color >> 1) & 15) == 12 && ((birthday_and_color >> 10) & 15) == 11);
    assert(last[0x1A] == 0xC5 && last[0x1C] == 0x30 && last[0x1D] == 0x57);
    assert(last[0x1E] == 0x3D && last[0x1F] == 0xD8 && last[0x20] == 0 && last[0x21] == 0xDE);
    assert(kernel::mii_crc16(last.data(), last.size()) == 0);

    // The account's Mii may also be in the library. Its original identity is used to deduplicate it.
    auto account = last;
    account[0x30] ^= 1; // The current account has an edited version of the same Mii.
    const u16 crc = kernel::mii_crc16(account.data(), 0x5E);
    account[0x5E] = (u8)(crc >> 8);
    account[0x5F] = (u8)crc;
    g_host_miis.push_back(account);
    kernel::g_host_my_mii = 3000;
    int personal = -1;
    const auto &list = kernel::console_miis(&personal);
    assert(list.size() == 3000 && personal == 2999);
    assert(((list[personal][3] >> 4) & 7) == 3 && (list[personal][1] & 1));
    assert(list[personal][0x30] == account[0x30]);
    assert(kernel::mii_crc16(list[personal].data(), list[personal].size()) == 0);
    puts("FFL_ODB validation, sparse/full enumeration, byte order and personal dedup passed");
}
