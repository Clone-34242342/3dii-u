#!/usr/bin/env python3
"""Run analytical scenes through the actual software GPU; no games or Cemu needed."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds
import gpu_replay_verify as oracle


def main():
    work = Path(build3ds.WORK) / 'selftest'
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='graphics-replay-', dir=work) as temporary:
        folder = Path(temporary)
        runtime, port = ROOT / 'third_party/3dsrecomp/runtime', ROOT / 'port'
        sources = [runtime / 'gpu' / (name + '.cpp') for name in
                   ('gpu', 'fragment', 'raster', 'shader', 'texture', 'transfer', 'vertex')]
        sources += [port / 'pc/stubs.cpp', port / 'pc/win/win_compat.cpp', runtime / 'gpu_weak.cpp',
                    runtime.parent / 'tools/gpu_test/stub.cpp', ROOT / 'tools/test_gpu_render.cpp']
        executable = folder / 'graphics_replay.exe'
        command = build3ds.HOST_CXX + ['-std=c++20', '-O1', '-ffp-contract=off', '-static',
            '-I' + str(port / 'pc/win'), '-I' + str(port / 'pc'), '-I' + str(port / 'wiiu'),
            '-I' + str(runtime), '-I' + str(runtime / 'gpu'), '-include', str(port / 'pc/shim.h'),
            *map(str, sources), '-lpthread', '-lwinmm', '-lgdi32', '-o', str(executable)]
        subprocess.run(command, check=True, timeout=300)
        output = folder / 'raw'
        output.mkdir()
        subprocess.run([str(executable), str(output)], cwd=output, check=True, timeout=30)
        reference = folder / 'analytical'
        oracle.make_analytical(reference)
        proof = oracle.verify(output, 'software', reference, folder / 'png')
        assert proof['match'], json.dumps(proof)
        print('Production software GPU: 6 analytical frames, exact RGBA match, no masked pixels')


if __name__ == '__main__':
    main()
