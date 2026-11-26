"""Self-check of the recompiler's reading of RVCT's Thumb switch through a helper (gen.py, switch8): a `blx` to
__ARM_common_switch8_thumb with the case table right after it is a jump to the cases, and the table isn't code. Synthetic code, so it
runs anywhere. Run: python tools/test_recomp_switch.py"""
import json
import os
import shutil
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'third_party', '3dsrecomp'))
from tools.recomp import gen, image  # noqa: E402
from tools.recomp.ins import ICALL, JMP  # noqa: E402

BASE = 0x100000
# 100000 (Thumb): movs r3, r0; push {lr}; blx 100020; then the table at 100008: 2 cases, then offsets 2, 4 and 6 (the last for any
# index of 2 or more), in halfwords from 100009; 10000c: movs r0, #1; pop {pc}; 100010: movs r0, #2; pop {pc}; 100014: movs r0, #3;
# pop {pc}; then padding. 100020 (ARM): the helper, its index in r3.
thumb = struct.pack('<4H', 0x0003, 0xB500, 0xF000, 0xE80C) + bytes([2, 2, 4, 6]) + struct.pack('<8H', 0x2001, 0xBD00, 0x2002, 0xBD00,
                                                                                               0x2003, 0xBD00, 0, 0)
helper = struct.pack('<6I', 0xE55EC001, 0xE153000C, 0x27DE300C, 0x37DE3003, 0xE08EC083, 0xE12FFF1C)
code = thumb + bytes(0x20 - len(thumb)) + helper + bytes(0x40)

d = tempfile.mkdtemp(prefix='switch_test_')
try:
    with open(os.path.join(d, 'code.bin'), 'wb') as f:
        f.write(code)
    with open(os.path.join(d, 'info.json'), 'w') as f:
        json.dump({'text': {'addr': BASE, 'size': len(code)}, 'rodata': {'addr': BASE + 0x1000, 'size': 0},
                   'data': {'addr': BASE + 0x2000, 'size': 0}}, f)
    an = gen.Analyzer(image.Image(d), log=lambda *a: None)
    i = an.decode(0x100004, 1)
    assert i.kind == JMP and i.jt == [0x10000D, 0x100011, 0x100015], f'the call is a jump to the three cases: {i}'
    c = i.code[0]
    # as the helper: the offset of index r3, or of the last entry for an index of 2 or more, into r3; the target into ip; lr as the call left it
    for part in ('u32 b = 2u;', 'cf = a >= b;', 'r3 = RD8(0x100009u + (a < b ? a : b));', 'r12 = 0x100009u + (r3 << 1);', 'r14 = 0x100009u;', 'tpc = r12;'):
        assert part in c, f'the helper\'s effects: {part} in {c}'
    insts, ok = an.explore(0x100001)
    assert ok and {0x10000C, 0x100010, 0x100014} <= set(insts), 'every case is explored as code'
    assert 0x100008 not in insts and 0x10000A not in insts, 'the table is not decoded as code'
    out = gen.Emitter(an, d).emit_function(0x100001)
    for case, label in ((0x10000D, 0x10000C), (0x100011, 0x100010), (0x100015, 0x100014)):
        assert f'case 0x{case:x}u: goto L_{label:x};' in out and f'L_{label:x}:' in out, f'case {case:x} is a label of the function'
    # anything else at the target stays a call
    j = gen.Analyzer(image.Image(d), log=lambda *a: None)
    j.img.text.data = j.img.text.data[:0x24] + struct.pack('<I', 0xE1A00000) + j.img.text.data[0x28:]
    assert j.decode(0x100004, 1).kind != JMP, 'a call to another function stays a call'

    relative = bytearray(len(code))
    struct.pack_into('<I', relative, 0, 0xE12FFF1E)  # first case: bx lr
    struct.pack_into('<iiI', relative, 0x10, -0x10, 0x10, 0xE12FFF1E)
    struct.pack_into('<I', relative, 0x20, 0xE12FFF1E)  # second case
    struct.pack_into('<5I', relative, 0x40,
                     0xE24F6038, 0xE3500001, 0xE7965100, 0xE1A0E008, 0xE085F006)
    start, jump = BASE + 0x40, BASE + 0x50

    def relative_analyzer(*, abi=False, mixed_abi=False):
        img = image.Image(d)
        img.text.data = bytes(relative)
        return gen.Analyzer(img, log=lambda *a: None, abi=abi, mixed_abi=mixed_abi)

    an = relative_analyzer()
    insts, ok = an.explore(start)
    assert ok and insts[jump].jt == [BASE, BASE + 0x20]
    assert {BASE, BASE + 0x20} <= insts.keys() and BASE + 0x10 not in insts
    emitter = gen.Emitter(an, d)
    emitted = emitter.emit_function(start)
    assert 'case 0x100020u: goto L_100020; default: break;' in emitted
    assert 'return rt_dispatch(c, 0);' in emitted
    assert (BASE + 0x20, gen.fname(start), 2) in emitter.entries, 'a case can also be entered by dispatch'
    mixed, ok = relative_analyzer(mixed_abi=True).explore(start)
    assert ok and mixed[jump].jt is None

    struct.pack_into('<3I', relative, 0x48, 0xE7965000, 0xE086F005, 0xE12FFF1E)
    insts, ok = relative_analyzer().explore(start)
    assert ok and insts[jump - 4].jt == [BASE, BASE + 0x20]  # byte index, reversed ADD operands

    struct.pack_into('<I', relative, 0x44, 0xE3A06001)  # base clobbered after ADR
    insts, ok = relative_analyzer().explore(start)
    assert ok and insts[jump - 4].jt is None

    # adr lr, pc+0; add lr, lr, #0x18; relative table jump; its return goes to 0x100058.
    struct.pack_into('<5I', relative, 0x38, 0xE28FE000, 0xE28EE018, 0xE24F6038, 0xE7965100, 0xE086F005)
    struct.pack_into('<I', relative, 0x58, 0xE12FFF1E)
    an = relative_analyzer()
    an.img.entry = BASE + 0x38
    assert BASE + 0x58 in an.discover() and an.decode(BASE + 0x48, 0).link == BASE + 0x58
    abi = relative_analyzer(abi=True)
    jump_ins = abi.decode(BASE + 0x48, 0)
    assert abi.jump_table(jump_ins, 0) is None and jump_ins.link is None

    # adr r4, pc+0x10; sub r4, r4, #4; cmp r0, #1; movle pc, r4; two return paths
    struct.pack_into('<6I', relative, 0x60, 0xE28F4010, 0xE2444004, 0xE3500001, 0xD1A0F004, 0xE12FFF1E, 0xE12FFF1E)
    insts, ok = relative_analyzer().explore(BASE + 0x60)
    assert ok and insts[BASE + 0x6c].jt == [BASE + 0x74] and {BASE + 0x70, BASE + 0x74} <= insts.keys()

    # The relative table can live after the dispatch instead of before it.
    struct.pack_into('<3I', relative, 0, 0xE28F6008, 0xE7965100, 0xE086F005)
    struct.pack_into('<iiI', relative, 0x10, 0x10, 0x18, 0xE12FFF1E)
    struct.pack_into('<I', relative, 0x28, 0xE12FFF1E)
    insts, ok = relative_analyzer().explore(BASE)
    assert ok and insts[BASE + 8].jt == [BASE + 0x20, BASE + 0x28]
    struct.pack_into('<5I', relative, 0x10, 0x18, 0, 0, 0x1c, 0xE12FFF1E)
    struct.pack_into('<2I', relative, 0x28, 0xE12FFF1E, 0xE12FFF1E)
    insts, ok = relative_analyzer().explore(BASE)
    assert ok and insts[BASE + 8].jt == [BASE + 0x28, BASE + 0x2c]

    # The same relative table can dispatch a subroutine with an explicit LR.
    struct.pack_into('<6I', relative, 0, 0xE28F6028, 0xE28FE008, 0xE1A03000, 0xE7965100, 0xE086F005, 0xE12FFF1E)
    struct.pack_into('<3I', relative, 0x30, 0x20, 0x24, 0xE12FFF1E)
    struct.pack_into('<2I', relative, 0x50, 0xE12FFF1E, 0xE12FFF1E)
    an = relative_analyzer()
    assert {BASE + 0x50, BASE + 0x54} <= set(an.discover())
    i = an.decode(BASE + 0x10, 0)
    assert i.kind == ICALL and i.link == BASE + 0x14 and i.jt == [BASE + 0x50, BASE + 0x54]
    print('armcc switch: ok')
finally:
    shutil.rmtree(d, ignore_errors=True)
