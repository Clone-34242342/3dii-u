#!/usr/bin/env python3
"""Check production audio queue/lifetime and long DSP state with simulated devices.

Uses the actual Wii U output implementation and extracted frontend thread, plus
the upstream DSP tests. This does not establish real-device audio or performance.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds
    sys.path.insert(0, str(root / 'tools'))
    from test_vertex_cache import function

    out = args.out or Path(build3ds.WORK) / 'selftest/audio_stability'
    out.mkdir(parents=True, exist_ok=True)
    frontend = (root / 'port/wiiu/frontend_wiiu.cpp').read_text()
    cushion = re.search(r'^constexpr int AUDIO_CUSHION = [^;]+;', frontend, re.M)
    if not cushion:
        raise RuntimeError('Production AUDIO_CUSHION declaration was not found')
    thread = function(frontend, 'static int audio_thread(int, void *)')
    (out / 'audio_frontend_thread.inc').write_text(cushion[0] + '\n' + thread + '\n')

    runtime = root / 'third_party/3dsrecomp/runtime'
    queue = out / 'test_audio_queue.exe'
    stability = out / 'test_audio_stability.exe'
    common = ['-std=c++20', '-O1', '-w', '-static']
    queue_command = build3ds.HOST_CXX + common + [
        '-I' + str(here / 'audio_mock'), '-I' + str(out), '-I' + str(root),
        '-I' + str(runtime), str(here / 'test_audio_queue.cpp'), '-lpthread', '-o', str(queue)]
    sources = [str(runtime / 'audio' / (name + '.cpp'))
               for name in ('dsp_core', 'source', 'mixer', 'codec', 'interp')]
    dsp_command = build3ds.HOST_CXX + common + [
        '-I' + str(root / 'port/pc/win'), '-I' + str(root / 'port/pc'),
        '-I' + str(root), '-I' + str(runtime), '-include', str(root / 'port/pc/shim.h'),
        str(here / 'test_audio_stability.cpp'), str(root / 'port/pc/win/win_compat.cpp'),
        *sources, '-lwinmm', '-lpthread', '-o', str(stability)]
    # Compile serially even when another suite owns the second compiler slot.
    for command in (queue_command, dsp_command):
        subprocess.run(command, check=True, timeout=180)
    for failure, index in [('alloc', 1), ('alloc', 2), ('voice', 1), ('voice', 2), ('idempotent', 0)]:
        subprocess.run([str(queue), '--startup', failure, str(index)], check=True, timeout=10)
    subprocess.run([str(queue)], check=True, timeout=60)
    subprocess.run([str(stability)], check=True, timeout=120)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
