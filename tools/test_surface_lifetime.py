#!/usr/bin/env python3
"""Compile production surface lifetime and relocation with observable allocator stand-ins."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root / 'tools'))
    from test_vertex_cache import function
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds
    out = args.out or Path(build3ds.WORK) / 'selftest/surface_lifetime'
    out.mkdir(parents=True, exist_ok=True)
    source = (args.backend or root / 'port/wiiu/gx2_backend.cpp').read_text()
    records = source[source.index('struct LastTransfer {'):source.index('namespace {', source.index('struct LastTransfer {'))]
    lookup = function(source, 'Surface *get_surface(')
    lookup = lookup[:lookup.index('    auto s = std::make_unique<Surface>();')] + '    return nullptr;\n}\n'
    copy = function(source, 'bool gx2_texture_copy(')
    layout = copy[copy.index('    dst->gpu_chain = true;'):copy.index('    flush_surface(*src);')]
    copy = (copy[:copy.index('    Surface *dst =')] + '    Surface *dst = &copy_target;\n' + layout
            + '    copy_size = {w, h};\n    return true;\n}\n')
    production = function(source, 'inline void used(') + '\n'
    production += function(source, 'void flush_surface(Surface &s) {') + '\n'
    production += function(source, 'constexpr bool color_has_alpha(') + '\n' + function(source, 'constexpr u32 surface_comp_map(')
    production += '\n' + lookup + '\n' + records
    production += '\ntemplate <class Match>\n' + function(source, 'Surface *find_surface(u32 addr, bool depth, Match match)')
    production += '\n' + copy
    production += '\n' + source[source.index('constexpr u32 SLICES ='):source.index('// A word every 128 bytes')]
    production += '\n' + function(source, 'static int screen_of(')
    production += '\n' + source[source.index('bool gx2_skip_pass;'):source.index('void gx2_cpu_will_read(')]
    production += '\n' + function(source, 'static void show_bottom_lcd(')
    production += '\n' + function(source, 'bool transfer_source(const Surface &s) {')
    production += '\n' + function(source, 'void forget_unused()')
    production += '\n' + '\n'.join(function(source, signature) for signature in (
        'bool share_depth_block(', 'void apply_scale(', 'GX2Surface depth_storage_layout(',
        'void take_depth_block(', 'static void relieve_mem1() {',
    ))
    (out / 'surface_lifetime_production.inc').write_text(production)
    executable = out / 'test_surface_lifetime.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-Wall', '-Wextra', '-static',
        '-I' + str(root / 'port/wiiu'), '-I' + str(out), str(Path(__file__).with_suffix('.cpp')),
        '-lpthread', '-o', str(executable)], check=True)
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
