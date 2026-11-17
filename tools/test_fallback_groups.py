#!/usr/bin/env python3
"""Run Group 1 and Group 2 bounded execution fallback unit tests and benchmarks: test_fallback_groups.py"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds


def main():
    exe = build3ds.WORK + '/selftest/test_fallback_groups.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    flags = [
        *build3ds.HOST_CXX,
        '-std=c++20',
        '-O2',
        '-w',
        f'-I{build3ds.RT}',
        f'-I{build3ds.PORT}/pc/win',
        f'-I{build3ds.PORT}/pc',
        f'-I{build3ds.PORT}/wiiu',
        '-include', f'{build3ds.PORT}/pc/shim.h',
        '-static',
        str(ROOT / 'tools/test_fallback_groups.cpp'),
        '-lpthread',
        '-o', exe
    ]
    subprocess.run(flags, check=True)
    r = subprocess.run([exe], check=True, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if 'fallback benchmark' in line:
            print(line.strip())
    print('fallback groups: ok')


if __name__ == '__main__':
    main()
