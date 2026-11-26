#!/usr/bin/env python3
"""Play a game on this PC to log its shaders for the Wii U build: play_pc.py <game name, as for build3ds.py> [--speed N]

Builds the PC version (build3ds.py trial) and opens it in a window (RECOMP_WINDOW, port/pc/frontend_pc.cpp): arrows the circle pad, A/S/Z/X
the A/B/X/Y buttons, Q/W L and R, Enter Start, Backspace Select, I/J/K/L the d-pad, the mouse the stylus on the bottom screen, or an
XInput controller. The save goes on from one session to the next (build_win/run_<game>_play/save). Close the window to stop: the vertex
programs and fragment states the session met (vs_log.bin, fs_log.bin) are then added to games/<game>/shader_logs, without repeats, which
the next build (the Builder, or build3ds.py all) compiles ahead. No sound: the PC version plays none."""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from dvlb_to_log import log_spans  # noqa: E402


def merge(src, dst):
    """src's records not in dst yet, added to it; how many"""
    if not os.path.isfile(src):
        return 0
    with open(src, 'rb') as f:
        source = f.read()
    with open(dst, 'a+b') as f:
        f.seek(0)
        saved = f.read()
        have = set()
        end = 0
        for _, start, end in log_spans(saved):
            have.add(saved[start:end])
        if end < len(saved):
            f.truncate(end)  # the next append must start at a complete record boundary
        added = 0
        for _, start, end in log_spans(source):
            record = source[start:end]
            if record not in have:
                f.write(record)
                have.add(record)
                added += 1
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game')
    ap.add_argument('--speed', type=int, help="percent of a 3DS's speed (default 100)")
    a = ap.parse_args()
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'win', 'build3ds.py'), 'trial', a.game, '10000000', '--run', 'play', '--keep-save', '--autoplay', '0',
           '--shot-every', '1000000000', '--seconds', '86400', '--env', 'RECOMP_WINDOW=1']
    if a.speed:
        cmd += ['--speed', str(a.speed)]
    print('building and starting: close the window to stop', flush=True)
    subprocess.run(cmd, cwd=ROOT)
    run = os.path.join(ROOT, 'build_win', f'run_{a.game}_play')
    logs = os.path.join(ROOT, 'games', a.game, 'shader_logs')
    os.makedirs(logs, exist_ok=True)
    vs = merge(os.path.join(run, 'vs_log.bin'), os.path.join(logs, 'vs_log.bin'))
    fs = merge(os.path.join(run, 'fs_log.bin'), os.path.join(logs, 'fs_log.bin'))
    print(f'shaders for the next Wii U build: {vs} new vertex records and {fs} new fragment states in {logs}')


if __name__ == '__main__':
    main()
