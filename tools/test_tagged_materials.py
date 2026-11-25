#!/usr/bin/env python3
"""Synthetic format tests; no game assets in fixtures."""
import struct
from types import SimpleNamespace
import dvlb_to_log as dvlb
from tagged_materials import decode, shader_records

shader = bytearray(100)
shader[:4] = b'DVLB'
struct.pack_into('<II', shader, 4, 1, 12)
struct.pack_into('<II', shader, 12+24, 64, 1)
struct.pack_into('<HHI', shader, 12+64, 0, 14, 1)
tool = SimpleNamespace(parse=lambda b: ([0x22 << 26, 0x21 << 26, 0x22 << 26], [15], [(0, 0, [])]),
                       labels=lambda b: [(0, 0, 1, 'base'), (0, 1, 1, 'helper'), (0, 2, 1, 'pass')],
                       slots_for=lambda o: list(range(24)), record=dvlb.record)

def fixture(entry=2, reference=4):
    descriptors = [(0x1401b400, len(shader), 0), (0x9201b320, 1, 3),
                   (0x9201b330, 1, reference), (0x0201b325, 4, 0), (0x0201b331, 4, 4)]
    head = b''.join(struct.pack('<3I', *r) for r in descriptors)
    head += struct.pack('<4H', 0, 0xffff, entry, 0xffff)
    return head + bytes((-len(head)) % 16) + shader

rows = list(dvlb.vertex_records(b''.join(shader_records(fixture(), tool))))
assert [r[0] for r in rows] == [0, 2]  # helper label is not a material selection
raw = shader_records(fixture(), tool)[0]
tail = raw[-56:]
assert tail[14] == 1 and tail[:14] == bytes(14) and tail[16:32] == bytes(16)
assert [r[0] for r in dvlb.vertex_records(b''.join(shader_records(fixture(0x202), tool)))] == [0]
assert decode(b'ordinary asset') is None
for data in (fixture(99), fixture(reference=100), fixture()[:-8], fixture()[:50]):
    try:
        shader_records(data, tool)
    except (ValueError, struct.error):
        pass
    else:
        raise AssertionError('invalid material accepted')
print('PASS: material/pass selection, helper exclusion, aligned arenas, DVLE booleans, default token and malformed input rejection')
