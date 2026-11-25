"""Shader selections in tagged 0x1b3/0x1b4 material assets, not game-specific lists.

12-byte descriptors precede primitive data and a 16-byte-aligned blob arena.
References index descriptors. Only actual material/pass selections become
vertex records; arbitrary helper labels do not. Fragment decoding is separate.
"""
import struct

MAGIC = struct.pack('<I', 0x1401b400)


def decode(data):
    if not data.startswith(MAGIC):
        return None
    if len(data) > 64 << 20:
        raise ValueError('tagged material exceeds size limit')
    records, p = [], 0
    while p + 12 <= len(data):
        tag, size, at = struct.unpack_from('<3I', data, p)
        if (tag >> 8) & 0xffff not in (0x1b3, 0x1b4):
            break
        if tag >> 24 not in (2, 0x12, 0x14, 0x92, 0x93):
            raise ValueError('unknown tagged material descriptor')
        records.append((tag, size, at))
        p += 12
    end = max((at + size for tag, size, at in records if tag >> 24 == 2), default=0)
    blob = (p + end + 15) & ~15
    if blob > len(data):
        raise ValueError('truncated primitive arena')
    for tag, size, at in records:
        kind = tag >> 24
        if kind in (0x92, 0x93):
            if at > len(records) or size > len(records) - at:
                raise ValueError('reference outside descriptor table')
        else:
            base = p if kind == 2 else blob
            if at > len(data) - base or size > len(data) - base - at:
                raise ValueError('data outside material arena')
    return records, p, blob


def shader_records(data, tool):
    parsed = decode(data)
    if parsed is None:
        return []
    records, primitive, blob = parsed
    _, size, at = records[0]
    shader = data[blob + at:blob + at + size]
    if not shader.startswith(b'DVLB'):
        raise ValueError('material root does not contain a DVLB')
    code, swz, stages = tool.parse(shader)
    vertex = [s for s in stages if s[0] == 0]
    if len(vertex) != 1:
        raise ValueError('material needs one shared vertex program')
    declared = {entry for stage, entry, length, name in tool.labels(shader) if stage == 0}
    slots = tool.slots_for(vertex[0][2])
    bools, ints = bytearray(16), bytearray(16)
    dvle = next(at for at in struct.unpack_from(f'<{len(stages)}I', shader, 8) if shader[at + 6] == 0)
    const_at, count = struct.unpack_from('<II', shader, dvle + 24)
    if dvle + const_at + count * 20 > len(shader):
        raise ValueError('truncated material shader constants')
    for i in range(count):
        kind, register, value = struct.unpack_from('<HHI', shader, dvle + const_at + i * 20)
        if kind == 0 and register < 16:
            bools[register] = bool(value)
        elif kind == 1 and register < 4:
            ints[register * 4:register * 4 + 4] = struct.pack('<I', value)
    selected = set()
    for tag, count, first in records:
        field = 0x25 if tag == 0x9201b320 else 0x31 if tag == 0x9201b330 else None
        if field is None:
            continue
        matches = [(n, at) for t, n, at in records[first:first + count] if t == (0x0201b300 | field)]
        if len(matches) != 1 or matches[0][0] < 4:
            raise ValueError('missing or ambiguous material shader header')
        entry = struct.unpack_from('<H', data, primitive + matches[0][1])[0]
        # Default-pass token, observed in this schema; not an instruction address.
        if field == 0x31 and entry == 0x202:
            continue
        if entry not in declared or entry >= min(512, len(code)):
            raise ValueError('material selection is not a declared vertex entry')
        selected.add(entry)
    return [tool.record(entry, code[:512], swz, bools, slots, ints) for entry in sorted(selected)]
