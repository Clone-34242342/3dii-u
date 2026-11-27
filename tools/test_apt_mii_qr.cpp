#include "services/apt_mii_qr.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <numeric>
#include <windows.h>
#include <bcrypt.h>

int main() {
    using namespace apt_mii_qr;

    Block key_material;
    std::iota(key_material.begin(), key_material.end(), 0);
    std::array<std::uint8_t, plain_size> plain;
    std::iota(plain.begin(), plain.end(), 0);

    // PyCryptodome AES-CCM uses plain[12:20] + four zeros as nonce and authenticates the body padded with eight zeros.
    const std::array<std::uint8_t, wrapped_size> expected = {
        0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
        0x54, 0xcb, 0xe4, 0x8d, 0x1a, 0x5a, 0x0a, 0x3d,
        0x93, 0x62, 0x3c, 0x70, 0xdd, 0x84, 0x1f, 0x46,
        0x83, 0xfc, 0x8f, 0xbc, 0xb0, 0x4d, 0x86, 0xa2,
        0x2d, 0x45, 0x9b, 0xbe, 0xc5, 0xca, 0x4b, 0x08,
        0x35, 0x7d, 0xca, 0xa8, 0x89, 0xd3, 0xc9, 0x56,
        0x96, 0x14, 0xdf, 0x31, 0x23, 0x56, 0x35, 0x94,
        0x34, 0xc1, 0xa6, 0x1f, 0x25, 0xeb, 0x82, 0x9d,
        0x70, 0x8a, 0x58, 0x4a, 0x4b, 0xaa, 0x51, 0x78,
        0x1f, 0xdc, 0xaa, 0x1a, 0x27, 0x6b, 0xb4, 0xc9,
        0xd4, 0xfe, 0x74, 0xb9, 0x76, 0x86, 0x29, 0x40,
        0x63, 0x80, 0x20, 0x55, 0xbb, 0x81, 0xa8, 0xf4,
        0xe2, 0xec, 0xf2, 0xa7, 0xb2, 0x5f, 0x02, 0x81,
        0x18, 0xbf, 0x73, 0x1f, 0x79, 0xb1, 0x39, 0x6e,
    };

    BCRYPT_ALG_HANDLE aes = nullptr;
    assert(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
    wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
    assert(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0) == 0);
    BCRYPT_KEY_HANDLE key = nullptr;
    assert(BCryptGenerateSymmetricKey(aes, &key, nullptr, 0, key_material.data(), key_material.size(), 0) == 0);
    const auto encrypt = [&](Block& block) {
        ULONG written = 0;
        assert(BCryptEncrypt(key, block.data(), block.size(), nullptr, nullptr, 0,
                            block.data(), block.size(), &written, 0) == 0);
        assert(written == block.size());
    };

    std::array<std::uint8_t, wrapped_size> wrapped{};
    std::copy(plain.begin(), plain.end(), wrapped.begin());
    assert(transform(true, wrapped.data(), wrapped.data(), encrypt));
    assert(wrapped == expected);
    assert(transform(false, wrapped.data(), wrapped.data(), encrypt));
    assert(std::equal(plain.begin(), plain.end(), wrapped.begin()));

    std::array<std::uint8_t, plain_size> output;
    output.fill(0x55);
    const auto sentinel = output;
    wrapped = expected;
    wrapped.back() ^= 1;
    assert(!transform(false, wrapped.data(), output.data(), encrypt));
    assert(output == sentinel);

    assert(BCryptDestroyKey(key) == 0);
    key_material.front() ^= 1;
    assert(BCryptGenerateSymmetricKey(aes, &key, nullptr, 0, key_material.data(), key_material.size(), 0) == 0);
    assert(!transform(false, expected.data(), output.data(), encrypt));
    assert(output == sentinel);
    assert(BCryptDestroyKey(key) == 0);
    assert(BCryptCloseAlgorithmProvider(aes, 0) == 0);

    puts("APT Mii QR: AES-CCM vector, in-place round trip, and authentication failures passed");
}
