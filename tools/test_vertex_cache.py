#!/usr/bin/env python3
"""Compile the production vertex cache and list boundary with host GPU stubs."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def function(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--bench', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds
    out = args.out or Path(build3ds.WORK) / 'selftest/vertex_cache'
    out.mkdir(parents=True, exist_ok=True)
    source = (args.backend or root / 'port/wiiu/gx2_backend.cpp').read_text()
    cache = source[source.index('struct VtxEntry {'):source.index('// A target the game draws at twice')]
    hash_source = (root / 'third_party/3dsrecomp/runtime/gpu/gpu.cpp').read_text()
    production = function(hash_source, 'u64 hash_bytes(').replace('u64 hash_bytes(', 'u64 production_hash_bytes(', 1)
    production += '\n' + cache + '\n' + function(source, 'void gx2_finish_list()')
    if 'u32 vtx_list;' in cache:
        production = '#define VTX_LIST_GENERATION\n' + production
    (out / 'vertex_cache_production.inc').write_text(production)
    exe = out / 'test_vertex_cache.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O2', '-Wall', '-Wextra', '-static', '-I' + str(out),
                   str(Path(__file__).with_suffix('.cpp')), '-lpthread', '-o', str(exe)], check=True)
    return subprocess.run([str(exe)] + (['--bench'] if args.bench else [])).returncode


if __name__ == '__main__':
    raise SystemExit(main())
