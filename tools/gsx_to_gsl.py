#!/usr/bin/env python3
"""The geometry shaders a game's shader logs hold with the vertex shader before them ("GSX " records, port/wiiu/gx2_backend.cpp's
gx2_gsx_ready), as the "GS  " records tools/pica_to_c.py makes native code of: gsx_to_gsl.py <out.gsl> <shader log>...

A draw of a vertex and a geometry shader goes to the GPU when the pack has a shader for its own bools and uniforms; any other set of them
runs the geometry shader on the CPU, in the interpreter unless the build has native code for the program. A GSX record is logged for one set,
but the native code (one function per program and entry) takes the bools as it runs, so it covers the sets no run logged too: Luigi's Mansion
2's rain takes another way through the same program on a console than in Cemu. Each program is written once. A log is read as
tools/shadergen_main.cpp reads it, up to a record cut short. Exit code 0 with nothing written if the logs hold none."""
import pathlib
import struct
import sys

from dvlb_to_log import log_spans


def gs_programs(d):
    """the geometry shader of each GSX record of a shader log: (entry, code word count, swizzle word count, code and swizzle words as bytes)"""
    for tag, p, _ in log_spans(d):
        if tag == b'GSX ':
            _vs_entry, vnc, vns, entry, nc, ns = struct.unpack_from('<6I', d, p + 8)
            at = p + 8 + 44 + (vnc + vns) * 4
            yield entry, nc, ns, d[at:at + (nc + ns) * 4]


def gs_records(logs):
    """the "GS  " records of the geometry shaders in the logs' GSX records, each program once, and how many programs"""
    seen = dict.fromkeys(g for d in logs for g in gs_programs(d))
    return b''.join(b'GS  ' + struct.pack('<III', *g[:3]) + g[3] for g in seen), len(seen)


def self_check():
    def gsx(entry, code, vs_code=(1, 2)):
        body = struct.pack('<11I', 7, len(vs_code), 1, entry, len(code), 2, 0, 0, 0, 0, 0)
        body += struct.pack(f'<{len(vs_code) + len(code) + 3}I', *vs_code, 0xAA, *code, 0xB1, 0xB2) + bytes(88)
        return b'GSX ' + struct.pack('<I', len(body)) + body
    frag, fs = struct.pack('<I', 8) + bytes(8), b'FS  ' + struct.pack('<I', 4) + bytes(4)
    vs = b'VS  ' + struct.pack('<III', 0, 1, 1) + bytes(8 + 56)
    one = frag + gsx(3, (10, 11)) + fs + vs + gsx(3, (10, 11), vs_code=(5, 6))
    two = gsx(4, (10, 11)) + frag + gsx(5, (12,))[:-1]  # the last record cut short, as when a run stops while writing it
    data, n = gs_records([one, two])
    # one program after two vertex shaders is written once, another entry into it is another, and the record cut short is left out
    assert n == 2, n
    assert data == b''.join(b'GS  ' + struct.pack('<7I', e, 2, 2, 10, 11, 0xB1, 0xB2) for e in (3, 4)), data
    print('gsx_to_gsl self-check: ok')


def main():
    if sys.argv[1:] == ['--self-check']:
        return self_check()
    out, logs = sys.argv[1], sys.argv[2:]
    data, n = gs_records(pathlib.Path(path).read_bytes() for path in logs)
    if data:
        pathlib.Path(out).write_bytes(data)
    print(f'{n} geometry shader programs from the logs\' GSX records')


if __name__ == '__main__':
    main()
