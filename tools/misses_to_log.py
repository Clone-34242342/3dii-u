#!/usr/bin/env python3
"""Turns the console's shader_misses.bin into a log the shader generator reads: misses_to_log.py <shader_misses.bin> <out.bin> [fragment only.bin].
Geometry shaders the console ran on its CPU go to <out>.gsl beside it, for tools/pica_to_c.py.
Records are already little-endian: this drops the tags and repeats that the generator does not expect in fs_log.bin.
Fragment shaders depend on render state, not on the game, so other games' builds take the fragment-only file too (build3ds.prepare_logs)."""
import sys

from dvlb_to_log import log_spans

d = open(sys.argv[1], 'rb').read()
known = set()
out = bytearray()
only_fs = bytearray()
fs = vs = gs = 0
gs_out = bytearray()
for tag, p, end in log_spans(d):
    if d[p:p + 4] != tag:
        sys.exit(f'unknown record at {p}: {d[p:p + 4]!r}')
    if tag == b'FS  ':
        key = d[p + 8:end]
        if key not in known:
            known.add(key)
            out += d[p + 4:end]
            only_fs += d[p + 4:end]
            fs += 1
    else:
        rec = d[p:end]
        if rec not in known:
            known.add(rec)
            if tag == b'GS  ':  # a geometry shader the CPU ran, for tools/pica_to_c.py
                gs_out += rec
                gs += 1
            else:
                out += rec
                vs += 1
open(sys.argv[2], 'wb').write(out)
if gs_out:
    open(sys.argv[2][:-4] + '.gsl' if sys.argv[2].endswith('.bin') else sys.argv[2] + '.gsl', 'wb').write(gs_out)
if len(sys.argv) > 3:
    open(sys.argv[3], 'wb').write(only_fs)
print(f'{fs} fragment and {vs} vertex records, {gs} geometry shaders')
