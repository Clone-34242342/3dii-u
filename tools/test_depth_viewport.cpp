#include "depth_viewport.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Named register fields used by the real GX2ViewportReg integration. The test
// starts with GX2InitViewportReg's default [0,1] depth mapping.
struct Viewport {
    uint32_t pa_cl_vport_xscale = 0x43480000, pa_cl_vport_yscale = 0xc2f00000;
    uint32_t pa_cl_vport_zscale = 0x3f000000, pa_cl_vport_zoffset = 0x3f000000;
    uint32_t pa_sc_vport_zmin = 0, pa_sc_vport_zmax = 0x3f800000;
};
static float number(uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof f); return f; }
static float fixed_depth(const Viewport &v, float ndc_depth) {
    return ndc_depth * number(v.pa_cl_vport_zscale) + number(v.pa_cl_vport_zoffset);
}
// Independent PICA software-rasterizer depth rule, raster.cpp:1416-1417.
static float software_depth(float z_over_w, float w, float scale, float offset, bool wbuffer) {
    const float depth = z_over_w * scale + offset;
    return std::clamp(wbuffer ? depth * w : depth, 0.0f, 1.0f);
}
static void near(float a, float b) { assert(std::abs(a - b) < 0.000001f); }
int main() {
    Viewport old, fixed;
    gpu::depth_viewport::pica_range(fixed);
    assert(fixed.pa_cl_vport_xscale == old.pa_cl_vport_xscale && fixed.pa_cl_vport_yscale == old.pa_cl_vport_yscale);
    assert(fixed.pa_sc_vport_zmin == old.pa_sc_vport_zmin && fixed.pa_sc_vport_zmax == old.pa_sc_vport_zmax);
    // Same geometry at W=1 must compare equally across fixed Z and exported W.
    const float z = software_depth(-0.4f, 1, -1, 0, false);
    const float w = software_depth(-0.4f, 1, -1, 0, true);
    near(fixed_depth(old, z), .7f);
    near(fixed_depth(fixed, z), w);
    // Overlapping triangles must preserve the software LESS ordering, in both
    // draw orders, including non-unit W. The old range reverses each comparison.
    for (float clip_w : {0.5f, 1.0f, 2.0f}) {
        const float exported = software_depth(-0.2f, clip_w, -1, 0, true);
        const float front_z = exported - .05f;
        assert(front_z < exported);
        assert(fixed_depth(fixed, front_z) < exported);
        assert(!(exported < fixed_depth(fixed, front_z)));
        assert(fixed_depth(old, front_z) > exported);
    }
    for (float depth : {0.0f, .2f, .4f, .8f, 1.0f}) near(fixed_depth(fixed, depth), depth);
    std::puts("depth viewport: fixed Z and exported W share the PICA depth range");
}
