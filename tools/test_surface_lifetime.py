#!/usr/bin/env python3
"""Compile actual surface retirement and key reuse with observable allocator stand-ins."""
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
    production = function(source, 'inline void used(') + '\n' + lookup + '\n' + records
    production += '\n' + function(source, 'bool transfer_source(const Surface &s) {')
    production += '\n' + function(source, 'void forget_unused()')
    (out / 'surface_lifetime_production.inc').write_text(production)
    executable = out / 'test_surface_lifetime.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-Wall', '-Wextra', '-static',
        '-I' + str(root / 'port/wiiu'), '-I' + str(out), str(Path(__file__).with_suffix('.cpp')),
        '-lpthread', '-o', str(executable)], check=True)
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
