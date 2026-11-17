// The host supplies padded NV12; the guest receives packed YUYV or little-endian RGB565.
#pragma once
#include <algorithm>
#include <cstdint>

namespace camera_frame {
constexpr uint32_t HOST_WIDTH = 640, HOST_HEIGHT = 480, HOST_PITCH = 768;
constexpr uint32_t HOST_Y_SIZE = HOST_PITCH * HOST_HEIGHT;
constexpr uint32_t HOST_SIZE = HOST_Y_SIZE * 3 / 2;
constexpr uint32_t MAX_OUTPUT_SIZE = HOST_WIDTH * HOST_HEIGHT * 2;

struct Resolution {
    uint32_t width, height, x0, y0, x1, y1;  // sensor crop endpoints are inclusive
};
constexpr Resolution PRESETS[] = {
    {640, 480, 0, 0, 639, 479}, {320, 240, 0, 0, 639, 479},
    {160, 120, 0, 0, 639, 479}, {352, 288, 26, 0, 613, 479},
    {176, 144, 26, 0, 613, 479}, {256, 192, 0, 0, 639, 479},
    {512, 384, 0, 0, 639, 479}, {400, 240, 0, 48, 639, 431},
};
struct Context {
    Resolution resolution = PRESETS[5];
    uint32_t flip = 0, format = 0;
};
struct Trim {
    bool enabled = false;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // output crop endpoints are exclusive
};

inline bool valid(const Resolution &r) {
    return r.width && r.width <= HOST_WIDTH && r.height && r.height <= HOST_HEIGHT &&
           r.x0 <= r.x1 && r.x1 < HOST_WIDTH && r.y0 <= r.y1 && r.y1 < HOST_HEIGHT;
}

// Zero means invalid; callers must validate the entire destination before conversion writes anything.
inline uint32_t output_size(const Context &c, const Trim &t) {
    const auto &r = c.resolution;
    if (!valid(r) || c.flip > 3 || c.format > 1) return 0;
    if (!t.enabled) return !c.format && (r.width & 1) ? 0 : r.width * r.height * 2;
    if (t.x0 >= t.x1 || t.y0 >= t.y1 || t.x1 > r.width || t.y1 > r.height ||
        (!c.format && ((t.x0 | (t.x1 - t.x0)) & 1))) return 0;
    return (t.x1 - t.x0) * (t.y1 - t.y0) * 2;
}

inline bool convert(const uint8_t *src, uint32_t source_size, const Context &c, const Trim &t,
                    uint8_t *dst, uint32_t capacity) {
    const uint32_t size = output_size(c, t);
    if (!src || !dst || source_size < HOST_SIZE || !size || !capacity) return false;
    uint32_t remaining = std::min(size, capacity);
    auto put = [&](uint8_t value) { *dst++ = value; return --remaining == 0; };
    const auto &r = c.resolution;
    const uint32_t x0 = t.enabled ? t.x0 : 0, y0 = t.enabled ? t.y0 : 0;
    const uint32_t width = t.enabled ? t.x1 - t.x0 : r.width;
    const uint32_t height = t.enabled ? t.y1 - t.y0 : r.height;
    uint32_t xmap[HOST_WIDTH];
    for (uint32_t x = 0; x < width; x++) {
        const uint32_t sx = c.flip & 1 ? r.width - 1 - (x0 + x) : x0 + x;
        xmap[x] = r.x0 + sx * (r.x1 - r.x0 + 1) / r.width;
    }
    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = c.flip & 2 ? r.height - 1 - (y0 + y) : y0 + y;
        sy = r.y0 + sy * (r.y1 - r.y0 + 1) / r.height;
        const uint8_t *luma_row = src + sy * HOST_PITCH;
        const uint8_t *uv_row = src + HOST_Y_SIZE + (sy / 2) * HOST_PITCH;
        auto sample = [&](uint32_t x, int &luma, int &u, int &v) {
            const uint32_t sx = xmap[x];
            luma = luma_row[sx];
            u = uv_row[sx & ~1u];
            v = uv_row[(sx & ~1u) + 1];
        };
        for (uint32_t x = 0; x < width; x += c.format ? 1 : 2) {
            int luma, u, v;
            sample(x, luma, u, v);
            if (!c.format) {
                int luma1, u1, v1;
                sample(x + 1, luma1, u1, v1);
                if (put(static_cast<uint8_t>(luma))) return true;
                if (put(static_cast<uint8_t>((u + u1 + 1) / 2))) return true;
                if (put(static_cast<uint8_t>(luma1))) return true;
                if (put(static_cast<uint8_t>((v + v1 + 1) / 2))) return true;
            } else {
                const int yv = 298 * (luma - 16), uv = u - 128, vv = v - 128;
                const int red = std::clamp((yv + 409 * vv + 128) >> 8, 0, 255);
                const int green = std::clamp((yv - 100 * uv - 208 * vv + 128) >> 8, 0, 255);
                const int blue = std::clamp((yv + 516 * uv + 128) >> 8, 0, 255);
                const uint16_t rgb = (red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3);
                if (put(static_cast<uint8_t>(rgb))) return true;
                if (put(static_cast<uint8_t>(rgb >> 8))) return true;
            }
        }
    }
    return true;
}
}  // namespace camera_frame
