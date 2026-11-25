#!/usr/bin/env python3
"""Check actual immediate-input batching, parser barriers and final GPU reporting."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root / 'tools'))
    from test_vertex_cache import function
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds

    out = args.out or Path(build3ds.WORK) / 'selftest/native_immediate'
    out.mkdir(parents=True, exist_ok=True)
    runtime = root / 'third_party/3dsrecomp/runtime'
    vertex = (runtime / 'gpu/vertex.cpp').read_text()
    gpu = (runtime / 'gpu/gpu.cpp').read_text()
    backend = (root / 'port/wiiu/gx2_backend.cpp').read_text()
    shader = (runtime / 'gpu/shader.cpp').read_text()
    frontend = (root / 'port/wiiu/frontend_wiiu.cpp').read_text()
    (out / 'shader_init.inc').write_text('\n'.join(function(shader, signature) for signature in
        ['void shader_copy_uniforms(', 'void shader_unit_init(']) + '\n')
    production = '\n'.join(function(vertex, signature) for signature in ['void setup_outmap(', 'inline void map_vertex(',
        'inline u32 gather_outputs(', 'inline void load_inputs(', 'void submit_triangle(', 'void pa_submit(',
        'void pa_reset()', 'void vtx_reset_immediate()'])
    production += '\nu64 vtx_immediate_cpu_runs;\n' + function(vertex, 'static void immediate_cpu_vertex(') + '\n'
    start = vertex.index('#ifdef __WIIU__\nnamespace {\nV4 immediate_inputs')
    production += vertex[start:vertex.index('void vtx_flush_immediate()', start)]
    production += function(vertex, 'void vtx_flush_immediate()') + '\n' + function(vertex, 'void vtx_immediate_vertex()')
    (out / 'vertex_production.inc').write_text(production + '\n')
    start = backend.index('s32 reg_attr[16];', backend.index('bool gx2_draw('))
    (out / 'native_mapping.inc').write_text(backend[start:backend.index('FetchKey fk;', start)])
    (out / 'native_stream.inc').write_text(function(backend, 'if (immediate) {') + '\n')
    write = function(gpu, 'struct CmdList {') + ' cl;\nbool stop_requested;\n'
    write += function(gpu, 'constexpr u32 expand_mask[16]') + ';\n'
    write += '\n'.join(function(gpu, signature) for signature in ['float float_from_raw(', 'static V4 unpack_f24(',
        'static int upload_float(', 'static void upload_code(', 'static void upload_swizzle(', 'static inline bool mirror_vs_to_gs()',
        'inline void vs_uniform_word(', 'inline void vs_code_word(', 'inline void vs_swizzle_word(', 'static void submit_immediate(',
        'inline u64 lut_mix(', 'inline void write_reg(', 'void write_run(', 'void init_special()', 'void set_list(', 'void cmd_run('])
    write += '\n' + function((root / 'tools/test_gpu_render.cpp').read_text(), 'u32 f24_bits(')
    (out / 'write_production.inc').write_text(write + '\n')
    thread = function(frontend, 'static int gpu_thread(')
    start = thread.index('    if (!gpu_parked) { wiiu::gfx_shutdown(); drained = true; }')
    end = thread.index('    gpu_parked = true;', start) + len('    gpu_parked = true;')
    (out / 'gpu_completion.inc').write_text(thread[start:end] + '\n')
    assert thread.count('wiiu::gfx_report_final();') == 1
    assert thread.index('bool drained = false;') > thread.index('setjmp(gpu_restart);')
    final = function(backend, 'void gfx_report_final()')
    assert 'menu_wants_quit' not in final and not any(name in final for name in ('GX2DrawDone(', 'vtx_finish_jobs(', 'vtx_flush_immediate(', 'gx2_flush_cpu_batch('))
    assert 'route_quit = true;' in function(frontend, 'void frontend_live_key(')
    process = function(gpu, 'void process_command_list(')
    assert process.index('cmd_run(') < process.index('vtx_flush_immediate();') < process.index('raster_flush();')
    exe = out / 'test_native_immediate.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', '-D__WIIU__', '-DRECOMP_MEASURE',
                   '-I' + str(out), '-I' + str(runtime), '-I' + str(root / 'port/pc/win'), '-I' + str(root / 'port/pc'),
                   '-include', str(root / 'port/pc/shim.h'), str(Path(__file__).with_suffix('.cpp')),
                   '-lpthread', '-o', str(exe)], check=True, timeout=25)
    return subprocess.run([str(exe)], timeout=5).returncode


if __name__ == '__main__':
    raise SystemExit(main())
