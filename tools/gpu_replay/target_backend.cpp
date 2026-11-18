// Diagnostic wrapper: production implementation is included without changing its source.
#include "../../port/wiiu/gx2_backend.cpp"

static bool replay_default_480;
void replay_backend_init() {
    wiiu::gfx_thread_init();
    replay_default_480 = wiiu::debug_flag("default480");
    // Diagnostic 1x baseline only. A default480 marker retains the production setting
    // and production transfer downsampling; the fixture and shaders are identical.
    if (!replay_default_480) gpu::top_rows = 240;
    gpu::prof_laps = false;
}

bool replay_target_counters(FILE *file) {
    u64 cpu_vertices = 0, cpu_draws = 0;
    for (u32 i = 0; i < gpu::VCPU_COUNT; i++) {
        cpu_draws += gpu::vtx_cpu.draws[i];
        cpu_vertices += gpu::vtx_cpu.verts[i];
    }
    u64 cpu_route = 0;
    for (u64 count : gpu::g_cpu_route) cpu_route += count;
    const bool okay = gpu::st_draws == 6 && !gpu::st_cpu_draws && !cpu_draws &&
        !cpu_vertices && !cpu_route && !gpu::st_ps_misses && gpu::miss_buf.empty();
    fprintf(file, "{\"backend\":\"productionGX2\",\"resolution\":%u,\"diagnostic_1x\":%s,"
        "\"native_draws\":%llu,\"cpu_batches\":%llu,\"cpu_draws\":%llu,\"cpu_vertices\":%llu,"
        "\"cpu_route\":%llu,\"pixel_misses\":%llu,\"shader_miss_bytes\":%u,\"pass\":%s}",
        gpu::top_rows, replay_default_480 ? "false" : "true", (unsigned long long)gpu::st_draws,
        (unsigned long long)gpu::st_cpu_draws, (unsigned long long)cpu_draws,
        (unsigned long long)cpu_vertices, (unsigned long long)cpu_route,
        (unsigned long long)gpu::st_ps_misses, (u32)gpu::miss_buf.size(), okay ? "true" : "false");
    return okay;
}
