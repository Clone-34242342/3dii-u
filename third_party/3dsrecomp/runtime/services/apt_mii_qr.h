#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace apt_mii_qr {

constexpr std::size_t plain_size = 96;
constexpr std::size_t wrapped_size = 112;
constexpr std::size_t nonce_offset = 12;
constexpr std::size_t nonce_size = 8;
using Block = std::array<std::uint8_t, 16>;

template<class Encrypt>
bool transform(bool seal, const std::uint8_t *src, std::uint8_t *dst, Encrypt encrypt) {
    std::array<std::uint8_t, plain_size - nonce_size> body{};
    std::array<std::uint8_t, wrapped_size> wrapped{};
    if (seal) {
        std::memcpy(wrapped.data(), src + nonce_offset, nonce_size);
        std::memcpy(body.data(), src, nonce_offset);
        std::memcpy(body.data() + nonce_offset, src + nonce_offset + nonce_size, body.size() - nonce_offset);
    } else {
        std::memcpy(wrapped.data(), src, wrapped.size());
    }

    // CCM uses a 12-byte nonce, a 16-byte tag and a three-byte counter.
    // The 3DS authenticates the rounded length, including the last block's zero padding.
    Block counter{0x02};
    std::memcpy(counter.data() + 1, wrapped.data(), nonce_size);
    Block mac = counter;
    mac[0] = 0x3a;
    mac[15] = (body.size() + 15) & ~std::size_t{15};
    encrypt(mac);
    Block tag_mask = counter;
    encrypt(tag_mask);
    for (std::size_t offset = 0; offset < body.size(); offset += mac.size()) {
        ++counter[15];
        Block stream = counter;
        encrypt(stream);
        for (std::size_t i = 0; i < mac.size() && offset + i < body.size(); ++i) {
            auto &plain = body[offset + i];
            auto &cipher = wrapped[nonce_size + offset + i];
            if (seal) cipher = plain ^ stream[i];
            else plain = cipher ^ stream[i];
            mac[i] ^= plain;
        }
        encrypt(mac);
    }

    std::uint8_t mismatch = 0;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        const auto tag_byte = mac[i] ^ tag_mask[i];
        auto &stored = wrapped[nonce_size + body.size() + i];
        if (seal) stored = tag_byte;
        else mismatch |= stored ^ tag_byte;
    }
    if (mismatch) return false;
    if (seal) {
        std::memcpy(dst, wrapped.data(), wrapped.size());
    } else {
        std::memcpy(dst, body.data(), nonce_offset);
        std::memcpy(dst + nonce_offset, wrapped.data(), nonce_size);
        std::memcpy(dst + nonce_offset + nonce_size, body.data() + nonce_offset, body.size() - nonce_offset);
    }
    return true;
}

bool convert(bool seal, const std::uint8_t *src, std::uint8_t *dst);

}  // namespace apt_mii_qr
