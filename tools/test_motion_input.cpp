// Axis/sign contract: the same SDL sensor input through Cemu's VPAD and Azahar's HID paths.
// https://github.com/cemu-project/Cemu/blob/main/src/input/api/SDL/SDLControllerProvider.cpp
// https://github.com/cemu-project/Cemu/blob/main/src/input/motion/MotionSample.h
// https://github.com/azahar-emu/azahar/blob/master/src/input_common/sdl/sdl_impl.cpp
#include <cassert>
#include <cmath>
#include <cstdio>
#include "motion_input.h"

struct Vec3 { float x, y, z; };

int main() {
    int16_t a[3], g[3];
    wiiu::motion_input(Vec3{0, -1, 0}, Vec3{}, a, g);
    assert(a[0] == 0 && a[1] == -512 && a[2] == 0);  // face up, at rest
    assert(g[0] == 0 && g[1] == 0 && g[2] == 0);

    const Vec3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int axis = 0; axis < 3; ++axis) {
        for (float sign : {-1.0f, 1.0f}) {
            const Vec3 v{axes[axis].x * sign, axes[axis].y * sign, axes[axis].z * sign};
            wiiu::motion_input(v, v, a, g);
            for (int i = 0; i < 3; ++i) {
                assert(a[i] == (i == axis ? sign * 512 : 0));
                assert(g[i] == (i == axis ? sign * 5175 : 0));
            }
            assert(std::abs(g[axis] / 14.375f - sign * 360) < 0.001f);
        }
    }
    wiiu::motion_input(Vec3{100, -100, 0.5f}, Vec3{100, -100, 0.25f}, a, g);
    assert(a[0] == 32767 && a[1] == -32767 && a[2] == 256);
    assert(g[0] == 32767 && g[1] == -32767 && g[2] == 1293);
    puts("motion: flat rest, all signed axes, angular units and clipping passed");
}
