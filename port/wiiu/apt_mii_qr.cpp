#include <charconv>
#include <memory>
#include <string_view>

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

#include "rt.h"
#include "services/apt_mii_qr.h"

namespace apt_mii_qr {
namespace {

using Cipher = std::unique_ptr<AVAES, decltype(&av_free)>;

Cipher load_cipher() {
    Cipher cipher(nullptr, av_free);
    FILE *file = fopen("fs:/vol/external01/3dii-u/mii_qr.key", "rb");
    if (!file) {
        LOG("[apt] Mii QR needs the local key file at 3dii-u/mii_qr.key on the SD card\n");
        return cipher;
    }
    char text[64];
    const auto length = fread(text, 1, sizeof text, file);
    const bool complete = length < sizeof text && !ferror(file);
    fclose(file);
    std::string_view hex(text, length);
    constexpr std::string_view prefix = "slot0x31KeyN=";
    if (hex.starts_with(prefix)) hex.remove_prefix(prefix.size());
    const auto last = hex.find_last_not_of("\r\n \t");
    hex = hex.substr(0, last == std::string_view::npos ? 0 : last + 1);
    Block key{};
    if (complete && hex.size() == 2 * key.size()) {
        for (std::size_t i = 0; i < key.size(); ++i) {
            unsigned value = 0;
            const char *first = hex.data() + 2 * i;
            const auto parsed = std::from_chars(first, first + 2, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != first + 2) {
                LOG("[apt] Invalid local Mii QR key format\n");
                return cipher;
            }
            key[i] = value;
        }
        cipher.reset(av_aes_alloc());
        if (cipher && av_aes_init(cipher.get(), key.data(), 128, 0) >= 0) return cipher;
        cipher.reset();
        LOG("[apt] Could not initialize the Mii QR cipher\n");
    } else {
        LOG("[apt] Invalid local Mii QR key format\n");
    }
    return cipher;
}

}  // namespace

bool convert(bool seal, const std::uint8_t *src, std::uint8_t *dst) {
    static const auto cipher = load_cipher();
    return cipher && transform(seal, src, dst, [&](Block &block) {
        av_aes_crypt(cipher.get(), block.data(), block.data(), 1, nullptr, 0);
    });
}

}  // namespace apt_mii_qr
