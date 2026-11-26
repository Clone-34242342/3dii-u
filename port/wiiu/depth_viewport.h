#pragma once

namespace gpu::depth_viewport {
// Game vertices already contain PICA depth in [0,1]. halfZ controls clipping;
// the default GX2 viewport still remaps that depth to [.5,1], unlike PS exports.
template <class Viewport> inline void pica_range(Viewport &viewport) {
    viewport.pa_cl_vport_zscale = 0x3f800000;  // IEEE float 1
    viewport.pa_cl_vport_zoffset = 0;         // IEEE float 0
}
}  // namespace gpu::depth_viewport
