#pragma once
#include <algorithm>
#include <cstdint>

namespace wiiu {
// VPAD and the 3DS HID history use the same axis order; rotating y/z makes flat rest look like a 90-degree tilt.
template <class Vec3>
inline void motion_input(const Vec3 &acc, const Vec3 &gyro, int16_t (&accel)[3], int16_t (&angular)[3]) {
    auto raw = [](float value, float scale) { return (int16_t)std::clamp(value * scale, -32767.0f, 32767.0f); };
    accel[0] = raw(acc.x, 512.0f);
    accel[1] = raw(acc.y, 512.0f);
    accel[2] = raw(acc.z, 512.0f);
    // VPAD: turns/second. HID: 14.375 raw units per degree/second.
    angular[0] = raw(gyro.x, 360.0f * 14.375f);
    angular[1] = raw(gyro.y, 360.0f * 14.375f);
    angular[2] = raw(gyro.z, 360.0f * 14.375f);
}
}  // namespace wiiu
