#!/usr/bin/env python3
"""Self-check for what tools/dvlb_to_log.py writes: a shader binary made here, with a vertex entry and a geometry entry past the 512 words a
vertex shader's unit takes, gives dvlb.bin the vertex record (the binary's first 512 words) and dvlb.gsl the geometry program whole; a dump
with no geometry program gets an empty dvlb.gsl. Synthetic files, so it runs anywhere: python tools/test_dvlb_to_log.py"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dvlb_to_log  # noqa: E402


def dvlb(code, swz, entries):
    """a shader binary: one DVLP of this code and swizzle table, and a DVLE for each (kind, entry, outputs as (semantic, register, mask))"""
    head = 8 + 4 * len(entries)
    dvlp = b'DVLP' + bytes(4) + struct.pack('<4I', 0x18, len(code), 0x18 + 4 * len(code), len(swz))
    dvlp += struct.pack(f'<{len(code)}I', *code) + b''.join(struct.pack('<II', s, 0) for s in swz)
    les, at = [], head + len(dvlp)
    for kind, entry, outs in entries:
        e = b'DVLE' + struct.pack('<HBB', 0x1002, kind, 0) + struct.pack('<I', entry) + bytes(28) + struct.pack('<II', 0x40, len(outs)) + bytes(16)
        e += b''.join(struct.pack('<HHBxxx', *o) for o in outs)
        les.append(at)
        at += len(e)
        dvlp += e
    return b'DVLB' + struct.pack('<I', len(entries)) + struct.pack(f'<{len(les)}I', *les) + dvlp


def written(binary):
    """dvlb.bin and dvlb.gsl as dvlb_to_log.py writes them for a dump whose code.bin holds this binary"""
    game = tempfile.mkdtemp()
    try:
        os.makedirs(os.path.join(game, 'shader_logs'))
        with open(os.path.join(game, 'code.bin'), 'wb') as f:
            f.write(b'\0' * 64 + binary + b'\0' * 64)
        with open(os.path.join(game, 'romfs.bin'), 'wb') as f:
            f.write(bytes(64))  # no romfs: its header's first word isn't 0x28
        subprocess.run([sys.executable, os.path.join(HERE, 'dvlb_to_log.py'), game], check=True, stdout=subprocess.DEVNULL)
        return tuple(open(os.path.join(game, 'shader_logs', n), 'rb').read() for n in ('dvlb.bin', 'dvlb.gsl'))
    finally:
        shutil.rmtree(game)


code = tuple(range(0x1000, 0x1000 + 600))
swz = [0x11, 0x22, 0x33]
outs = [(0, 0, 0xF), (2, 1, 0xF)]  # position, colour
vs, gs = written(dvlb(code, swz, [(0, 0, outs), (1, 550, [])]))
assert vs == dvlb_to_log.record(0, code[:512], swz, bytes(16), dvlb_to_log.slots_for(outs)), 'the vertex record: the first 512 words'
assert gs == dvlb_to_log.gs_record(550, code, swz), 'the geometry program whole'
assert written(dvlb(code[:100], swz, [(0, 0, outs)]))[1] == b'', 'no geometry program: an empty dvlb.gsl'
print('dvlb_to_log ok: the vertex view and the whole geometry program')
