#!/usr/bin/env python3
"""Exercise production Cemu shader expansion with allocation ownership checks."""
import argparse
from pathlib import Path
import subprocess
import sys

from test_vertex_cache import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds
    out = args.out or Path(build3ds.WORK) / 'selftest/shader_install'
    out.mkdir(parents=True, exist_ok=True)
    backend = (args.backend or root / 'port/wiiu/gx2_backend.cpp').read_text()
    install = function(backend, 'void install_compiled()')
    (out / 'shader_install_production.inc').write_text(function(install, 'if (in_cemu)'))
    exe = out / 'test_shader_install.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O2', '-Wall', '-Wextra', '-static',
                   '-I' + str(out), '-I' + str(root / 'port/wiiu'),
                   str(Path(__file__).with_suffix('.cpp')), '-lpthread', '-o', str(exe)], check=True)
    return subprocess.run([str(exe)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
