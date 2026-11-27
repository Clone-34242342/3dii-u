#!/usr/bin/env python3
"""Differential test of the ARM lowering: random instruction blocks run through the generated C and Unicorn.

usage: python tools/test_cpu.py [count] [seed] [class,...]
Optional TEST_CPU_CC is a JSON compiler argv; default clang. TEST_CPU_WORK and TEST_CPU_REPORT select artifact locations.
Generated C runs on the host, not PowerPC; skipped oracle cases and any full FPSCR mismatch fail.
Memory/VFP use one instruction per block; directed edge cases honor the selected classes.
"""
from collections import Counter
import json
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn import arm_const as A

from tools.recomp import arm, thumb
from tools.recomp.ins import OP, COND_C

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE = 0x100000
SCRATCH = 0x10000
SCRATCH_SIZE = 0x10000

SPECIAL = [0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x80000001, 0x7ffffffe, 31, 32, 33, 0xff, 0x100]
FSPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.25, -7.75, 1e10, -1e-10, 65536.5, 2147483648.0, -2147483904.0,
            4294967296.0, 123456.789, float('inf'), float('-inf')]


def rnd_reg(r):
    if r.random() < 0.4:
        return r.choice(SPECIAL)
    return r.getrandbits(32)


def fbits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def rnd_freg(r):
    if r.random() < 0.5:
        return fbits(r.choice(FSPECIAL))
    if r.random() < 0.5:
        return fbits(r.uniform(-1000, 1000))
    return fbits(r.uniform(-1e6, 1e6))


def cond(r):
    return 14 if r.random() < 0.6 else r.randrange(15)


def reg(r, lo=0, hi=12, avoid=()):
    while True:
        x = r.randint(lo, hi)
        if x not in avoid:
            return x


# ---------------------------------------------------------------- instruction generators
def g_dp_imm(r):
    return (cond(r) << 28) | (1 << 25) | (r.randrange(16) << 21) | (r.randrange(2) << 20) | (reg(r) << 16) | (
        reg(r) << 12) | r.getrandbits(12)


def g_dp_shimm(r):
    return (cond(r) << 28) | (r.randrange(16) << 21) | (r.randrange(2) << 20) | (reg(r) << 16) | (reg(r) << 12) | (
        r.randrange(32) << 7) | (r.randrange(4) << 5) | reg(r)


def g_dp_shreg(r):
    return (cond(r) << 28) | (r.randrange(16) << 21) | (r.randrange(2) << 20) | (reg(r) << 16) | (reg(r) << 12) | (
        reg(r) << 8) | (r.randrange(4) << 5) | 0x10 | reg(r)


def fix_dp(w):
    op = (w >> 21) & 15
    if op in (8, 9, 10, 11):
        w |= 1 << 20  # TST/TEQ/CMP/CMN must set flags (S=0 is misc space)
        w &= ~(15 << 12)  # unused destination must be zero
    if op in (13, 15):
        w &= ~(15 << 16)  # MOV/MVN have no Rn operand
    return w


def g_mul(r):
    op = r.choice([0, 1, 2, 4, 5, 6, 7])
    rd, rn = reg(r), reg(r)
    if op >= 2:
        while rn == rd:
            rn = reg(r)
    if op == 0:
        rn = 0  # MUL has no accumulator
    S = r.randrange(2) if op != 2 else 0
    return (cond(r) << 28) | (op << 21) | (S << 20) | (rd << 16) | (rn << 12) | (reg(r) << 8) | 0x90 | reg(r)


def g_misc(r):
    c = cond(r)
    k = r.randrange(8)
    if k == 0:  # CLZ
        return (c << 28) | 0x016F0F10 | (reg(r) << 12) | reg(r)
    if k == 1:  # QADD family
        return (c << 28) | 0x01000050 | (r.randrange(4) << 21) | (reg(r) << 16) | (reg(r) << 12) | reg(r)
    if k == 2:  # SMLAxy / SMULxy / SMLAW / SMLALxy
        op = r.randrange(4)
        rd, ra = reg(r), reg(r)
        if op == 2:
            while ra == rd:
                ra = reg(r)
        xy = r.randrange(4)
        if op == 3 or (op == 1 and xy & 1):
            ra = 0  # SMULxy/SMULWy have no accumulator
        return (c << 28) | 0x01000080 | (op << 21) | (rd << 16) | (ra << 12) | (reg(r) << 8) | (xy << 5) | reg(r)
    if k == 3:  # MRS
        return (c << 28) | 0x010F0000 | (reg(r) << 12)
    if k == 4:  # MSR cpsr_f reg
        return (c << 28) | 0x0128F000 | reg(r)
    if k == 5:  # MSR cpsr_f imm
        return (c << 28) | 0x0328F000 | r.getrandbits(12)
    if k == 6:  # MSR cpsr_s (GE) + f
        return (c << 28) | 0x012CF000 | reg(r)
    return (c << 28) | 0x016F0F10 | (reg(r) << 12) | reg(r)


def g_media(r):
    c = cond(r)
    rd, rn, rm = reg(r), reg(r), reg(r)
    k = r.randrange(11)
    if k == 0:  # parallel add/sub
        pre = r.choice([1, 2, 3, 5, 6, 7])
        op = r.choice([0, 1, 2, 3, 4, 7])
        return (c << 28) | ((0x60 | pre) << 20) | (rn << 16) | (rd << 12) | 0xF00 | (op << 5) | 0x10 | rm
    if k == 1:  # PKH
        return (c << 28) | 0x06800010 | (rn << 16) | (rd << 12) | (r.randrange(32) << 7) | (r.randrange(2) << 6) | rm
    if k == 2:  # extends
        op1 = r.choice([0x68, 0x6A, 0x6B, 0x6C, 0x6E, 0x6F])
        n = 15 if r.random() < 0.5 else rn
        return (c << 28) | (op1 << 20) | (n << 16) | (rd << 12) | (r.randrange(4) << 10) | 0x70 | rm
    if k == 3:  # SEL
        return (c << 28) | 0x06800FB0 | (rn << 16) | (rd << 12) | rm
    if k == 4:  # SSAT/USAT
        op1 = r.choice([0x6A, 0x6E])
        return (c << 28) | (op1 << 20) | (r.randrange(32) << 16) | (rd << 12) | (r.randrange(32) << 7) | (
            r.randrange(2) << 6) | 0x10 | rm
    if k == 5:  # SAT16
        op1 = r.choice([0x6A, 0x6E])
        return (c << 28) | (op1 << 20) | (r.randrange(16) << 16) | (rd << 12) | 0xF30 | rm
    if k == 6:  # REV / REV16 / REVSH
        return r.choice([(c << 28) | 0x06BF0F30, (c << 28) | 0x06BF0FB0, (c << 28) | 0x06FF0FB0]) | (rd << 12) | rm
    if k == 7:  # SMLAD / SMLSD / SMUAD
        ra = 15 if r.random() < 0.3 else reg(r)
        return (c << 28) | 0x07000010 | (rd << 16) | (ra << 12) | (rm << 8) | (r.randrange(4) << 5) | rn
    if k == 8:  # SMLALD
        ra = reg(r, avoid=(rd,))
        return (c << 28) | 0x07400010 | (rd << 16) | (ra << 12) | (rm << 8) | (r.randrange(4) << 5) | rn
    if k == 9:  # SMMUL / SMMLA / SMMLS
        ra = 15 if r.random() < 0.3 else reg(r)
        op2 = r.choice([0, 1, 6, 7])
        if op2 >= 6 and ra == 15:
            ra = 0
        return (c << 28) | 0x07500010 | (rd << 16) | (ra << 12) | (rm << 8) | (op2 << 5) | rn
    ra = 15 if r.random() < 0.5 else reg(r)
    return (c << 28) | 0x07800010 | (rd << 16) | (ra << 12) | (rm << 8) | rn


# memory: r8-r12 point into scratch, r0-r3 are small offsets
def g_mem(r):
    c = cond(r)
    k = r.randrange(5)
    rn = reg(r, 8, 12)
    if k == 0:  # LDR/STR imm
        P, U, B, Wb, L = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
        rd = reg(r, 0, 12, avoid=(rn,))
        imm = r.randrange(0, 0x100) & ~3
        return (c << 28) | (1 << 26) | (P << 24) | (U << 23) | (B << 22) | (Wb << 21) | (L << 20) | (rn << 16) | (
            rd << 12) | imm
    if k == 1:  # reg offset
        P, U, B, Wb, L = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
        rd = reg(r, 0, 12, avoid=(rn,))
        rm = reg(r, 0, 3)
        return (c << 28) | (3 << 25) | (P << 24) | (U << 23) | (B << 22) | (Wb << 21) | (L << 20) | (rn << 16) | (
            rd << 12) | (r.randrange(3) << 7) | rm
    if k == 2:  # halfword/signed/doubleword
        P, U, I, Wb = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
        sh = r.randrange(1, 4)
        L = r.randrange(2)
        rd = reg(r, 0, 12, avoid=(rn,))
        if sh >= 2 and not L:
            rd &= ~1
            if rd in (rn, rn - 1) or rd + 1 == rn:
                rd = 0 if rn != 0 else 2
        if sh == 2 and not L and rd + 1 == rn:
            rd = 4
        imm = r.randrange(0, 0x40) & ~7
        lo = imm & 15 if I else reg(r, 0, 3)
        high = (imm >> 4) & 15 if I else 0
        if sh >= 2 and not L and not P:
            Wb = 0  # doubleword post-index encoding requires W=0
        return (c << 28) | (P << 24) | (U << 23) | (I << 22) | (Wb << 21) | (L << 20) | (rn << 16) | (rd << 12) | (
            high << 8) | 0x90 | (sh << 5) | lo
    if k == 3:  # LDM/STM
        P, U, Wb, L = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
        lst = r.getrandbits(13) | (1 << r.randrange(13))
        if Wb:
            lst &= ~(1 << rn)
        if not lst:
            lst = 1 if rn != 0 else 2
        return (c << 28) | (4 << 25) | (P << 24) | (U << 23) | (Wb << 21) | (L << 20) | (rn << 16) | lst
    # SWP / LDREX/STREX
    rd = reg(r, 0, 7)
    rm = reg(r, 0, 7)
    if r.random() < 0.5:
        return (c << 28) | 0x01000090 | (r.randrange(2) << 22) | (rn << 16) | (rd << 12) | rm
    return (c << 28) | 0x01900F9F | (rn << 16) | (rd << 12)


def g_vfp(r):
    c = cond(r)
    k = r.randrange(9)
    dbl = r.random() < 0.25
    def sr():
        return r.randrange(32)
    def enc_s(x, lo_pos, hi_pos):
        return ((x >> 1) << hi_pos) | ((x & 1) << lo_pos)
    if k <= 2:  # arithmetic
        opc1 = r.choice([0, 1, 2, 3, 8])
        op = r.randrange(2) if opc1 != 8 else 0
        if dbl:
            d, n, m = r.randrange(16), r.randrange(16), r.randrange(16)
            return (c << 28) | 0x0E000B00 | (opc1 << 20) | (n << 16) | (d << 12) | (op << 6) | m
        d, n, m = sr(), sr(), sr()
        return (c << 28) | 0x0E000A00 | (opc1 << 20) | enc_s(d, 22, 12) | enc_s(n, 7, 16) | (op << 6) | enc_s(m, 5, 0)
    if k == 3:  # other unary / compare / cvt
        choices = [(0, 1), (0, 3), (1, 1), (1, 3), (4, 1), (4, 3), (5, 1), (7, 3), (8, 1), (8, 3), (12, 1), (12, 3),
                   (13, 1), (13, 3)]
        opc2, opc3 = r.choice(choices)
        sz = 1 if dbl else 0
        d, m = sr(), sr()
        if sz:
            d16, m16 = r.randrange(16), r.randrange(16)
            if opc2 in (12, 13):
                return (c << 28) | 0x0EB00B40 | (opc2 << 16) | enc_s(d, 22, 12) | (opc3 << 6) | m16
            if opc2 == 8:
                return (c << 28) | 0x0EB00B40 | (opc2 << 16) | (d16 << 12) | (opc3 << 6) | enc_s(m, 5, 0)
            if opc2 == 7:
                return (c << 28) | 0x0EB00B40 | (opc2 << 16) | enc_s(d, 22, 12) | (opc3 << 6) | m16
            return (c << 28) | 0x0EB00B40 | (opc2 << 16) | (d16 << 12) | (opc3 << 6) | m16
        if opc2 == 7:
            d16 = r.randrange(16)
            return (c << 28) | 0x0EB00A40 | (opc2 << 16) | (d16 << 12) | (opc3 << 6) | enc_s(m, 5, 0)
        return (c << 28) | 0x0EB00A40 | (opc2 << 16) | enc_s(d, 22, 12) | (opc3 << 6) | enc_s(m, 5, 0)
    if k == 4:  # vmrs apsr_nzcv after a compare
        return (c << 28) | 0x0EF1FA10
    if k == 5:  # vmov s <-> r
        L = r.randrange(2)
        return (c << 28) | 0x0E000A10 | (L << 20) | enc_s(sr(), 7, 16) | (reg(r) << 12)
    if k == 6:  # vmov 64
        L = r.randrange(2)
        rt, rt2 = reg(r), reg(r)
        while L and rt2 == rt:
            rt2 = reg(r)
        if dbl:
            return (c << 28) | 0x0C400B10 | (L << 20) | (rt2 << 16) | (rt << 12) | r.randrange(16)
        return (c << 28) | 0x0C400A10 | (L << 20) | (rt2 << 16) | (rt << 12) | enc_s(r.randrange(31), 5, 0)
    if k == 7:  # vldr / vstr
        L, U = r.randrange(2), r.randrange(2)
        rn = reg(r, 8, 12)
        imm = r.randrange(0, 0x20)
        if dbl:
            return (c << 28) | 0x0D000B00 | (U << 23) | (L << 20) | (rn << 16) | (r.randrange(16) << 12) | imm
        return (c << 28) | 0x0D000A00 | (U << 23) | (L << 20) | (rn << 16) | enc_s(sr(), 22, 12) | imm
    # vldm / vstm
    L = r.randrange(2)
    rn = reg(r, 8, 12)
    mode = r.choice(['ia', 'ia!', 'db!'])
    P, U, Wb = {'ia': (0, 1, 0), 'ia!': (0, 1, 1), 'db!': (1, 0, 1)}[mode]
    if dbl:
        first = r.randrange(16)
        n = r.randint(1, 16 - first)
        return (c << 28) | 0x0C000B00 | (P << 24) | (U << 23) | (Wb << 21) | (L << 20) | (rn << 16) | (first << 12) | (2 * n)
    first = sr()
    n = r.randint(1, 32 - first)
    return (c << 28) | 0x0C000A00 | (P << 24) | (U << 23) | (Wb << 21) | (L << 20) | (rn << 16) | enc_s(first, 22, 12) | n


def g_thumb(r):
    """A random Thumb-1 non-branch instruction (16 bits)."""
    while True:
        hw = r.getrandbits(16)
        top = hw >> 11
        if top in (0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F):  # cond branch/svc, b, bl
            continue
        if hw >> 10 == 0x11 and (hw >> 8) & 3 == 3:  # bx/blx
            continue
        if hw >> 10 == 0x11:
            rd = ((hw >> 4) & 8) | (hw & 7)
            rm = (hw >> 3) & 15
            if rd >= 13 or rm >= 13:  # sp/lr/pc in hi register ops
                continue
        if top in (0x10, 0x11, 0x12, 0x13, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19, 0x09):  # memory (tested via arm)
            continue
        if hw >> 12 == 0x5 or hw >> 12 == 0xB or hw >> 12 == 0xA:  # reg ldst, misc, add pc/sp
            if hw >> 8 == 0xB2 or hw >> 8 == 0xBA:  # extend / rev are fine
                return hw
            continue
        return hw


GENS = {
    'dp': lambda r: fix_dp(r.choice([g_dp_imm, g_dp_shimm, g_dp_shreg])(r)),
    'mul': g_mul,
    'misc': g_misc,
    'media': g_media,
    'mem': g_mem,
    'vfp': g_vfp,
    'thumb': g_thumb,
}


def acceptable(w, i):
    if i.kind != OP:
        return False
    if any('tpc' in c for c in i.code):
        return False
    if 'c->tls' in ' '.join(i.code):
        return False
    return True


# ---------------------------------------------------------------- reference: unicorn
def run_unicorn(words, regs, flags, vregs, fpscr, mem, is_thumb=False):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    from unicorn.arm_const import UC_CPU_ARM_1176
    uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    uc.mem_map(CODE, 0x10000)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    code = b''.join(struct.pack('<H' if is_thumb else '<I', w) for w in words)
    uc.mem_write(CODE, code)
    uc.mem_write(SCRATCH, bytes(mem))
    cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
    cpsr = (cpsr & 0x07F0FE00) | ((flags & 0x1F) << 27) | 0x10
    cpsr = (cpsr & ~0x000F0000) | ((flags >> 5) << 16)  # GE in flags bits 5..8 (see below)
    uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
    for k in range(13):
        uc.reg_write(A.UC_ARM_REG_R0 + k, regs[k])
    uc.reg_write(A.UC_ARM_REG_SP, regs[13])
    uc.reg_write(A.UC_ARM_REG_LR, regs[14])
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    for k in range(16):
        lo, hi = vregs[2 * k], vregs[2 * k + 1]
        uc.reg_write(A.UC_ARM_REG_D0 + k, lo | (hi << 32))
    uc.reg_write(A.UC_ARM_REG_FPSCR, fpscr)
    if is_thumb:
        uc.emu_start(CODE | 1, CODE + 2 * len(words))
    else:
        uc.emu_start(CODE, CODE + 4 * len(words))
    out_regs = [uc.reg_read(A.UC_ARM_REG_R0 + k) for k in range(13)] + [uc.reg_read(A.UC_ARM_REG_SP), uc.reg_read(A.UC_ARM_REG_LR)]
    cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
    out_flags = (cpsr >> 27) & 0x1F
    ge = (cpsr >> 16) & 15
    vr = []
    for k in range(16):
        d = uc.reg_read(A.UC_ARM_REG_D0 + k)
        vr += [d & 0xFFFFFFFF, d >> 32]
    fps = uc.reg_read(A.UC_ARM_REG_FPSCR)
    m = uc.mem_read(SCRATCH, SCRATCH_SIZE)
    return out_regs, out_flags, ge, vr, fps, bytes(m)


# ---------------------------------------------------------------- C side
def c_function(name, insts):
    lines = ['static void %s(CPU *c) {' % name,
             '  u32 ' + ', '.join('r%d = c->r[%d]' % (k, k) for k in range(15)) + ';',
             '  u32 nf = c->nf, zf = c->zf, cf = c->cf, vf = c->vf, qf = c->qf, ge = c->ge, fpsr = c->fpscr, tpc = 0;',
             '  float ' + ', '.join('S%d = c->s[%d]' % (k, k) for k in range(32)) + ';',
             '  u8 *const M = MEMBASE; (void)tpc;']
    for i in insts:
        s = ' '.join(i.code)
        cc = COND_C[i.cond]
        lines.append(('  if (%s) { %s }' % (cc, s)) if cc else '  ' + s)
    lines.append('  ' + ' '.join('c->r[%d] = r%d;' % (k, k) for k in range(15)))
    lines.append('  c->nf = nf; c->zf = zf; c->cf = cf; c->vf = vf; c->qf = qf; c->ge = ge; c->fpscr = fpsr;')
    lines.append('  ' + ' '.join('c->s[%d] = S%d;' % (k, k) for k in range(32)))
    lines.append('}')
    return '\n'.join(lines)


def nan_equivalent(words, insts, actual, expected, register):
    """Allow payload/sign variation only in a typed arithmetic NaN destination.

    VFP cases contain one instruction, so raw moves, integer conversions and
    finite double halves can never be mistaken for single-precision NaNs.
    """
    if len(words) != 1:
        return False
    note, word = insts[0].note, words[0]
    arithmetic = {'vadd', 'vsub', 'vmul', 'vnmul', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vsqrt'}
    conversions = {'vcvt.f32.f64', 'vcvt.f64.f32'}
    if note.split('.')[0] not in arithmetic and note not in conversions:
        return False
    double = (note.endswith('.f64') and note != 'vcvt.f32.f64') or note == 'vcvt.f64.f32'
    if double:
        low = 2 * ((word >> 12) & 15)
        if register not in (low, low + 1):
            return False
        def is_nan(values):
            lo, hi = values[low:low + 2]
            return (hi & 0x7ff00000) == 0x7ff00000 and bool((hi & 0xfffff) or lo)
    else:
        low = 2 * ((word >> 12) & 15) + ((word >> 22) & 1)
        if register != low:
            return False
        def is_nan(values):
            bits = values[low]
            return (bits & 0x7f800000) == 0x7f800000 and bool(bits & 0x7fffff)
    return is_nan(actual) and is_nan(expected)


def add_edges(cases, classes, ctx):
    """Small deterministic defined cases, restricted to the selected classes."""
    words = {
        'dp': [0xe0910002, 0xe0b10002, 0xe0510002, 0xe0d10002, 0xe1b00211, 0xe1b00231, 0xe1b00251, 0xe1b00271],
        'mul': [0xe0100291, 0xe0910392, 0xe0b10392, 0xe0d10392, 0xe0f10392],
        'misc': [0xe16f0f11, 0xe1020051, 0xe1220051, 0xe1420051, 0xe1620051],
        'mem': [0xe5980000, 0xe5880004, 0xe1d800b0, 0xe1c800b0, 0xe1d800f0, 0xe1c800d0],
        'thumb': [0x1888, 0x1a88, 0x4148, 0x4188, 0x4088, 0x40c8, 0x4108, 0x41c8, 0x4348],
    }
    rng = random.Random(314159)
    for cls in ('media', 'vfp'):
        if cls not in classes:
            continue
        words[cls], notes = [], set()
        while len(words[cls]) < 6:
            word = GENS[cls](rng)
            ins = arm.decode(CODE, word, ctx)
            if acceptable(word, ins) and ins.note not in notes:
                words[cls].append(word)
                notes.add(ins.note)
    values = [0, 1, 31, 32, 33, 255, 256, 0x7fffffff, 0x80000000, 0xffffffff, 0x80008000, 0x7fff7fff]
    count = 0
    for cls, instructions in words.items():
        if cls not in classes:
            continue
        for word in instructions:
            ins = thumb.decode(CODE, word, 0, ctx) if cls == 'thumb' else arm.decode(CODE, word, ctx)
            assert acceptable(word, ins), (cls, hex(word), ins.note)
            for index, value in enumerate(values):
                regs = [values[(index + k) % len(values)] for k in range(15)]
                regs[0] = regs[1] = value
                regs[2], regs[3] = values[(index + 1) % len(values)], 0xffffffff
                for k in range(8, 13):
                    regs[k] = SCRATCH + 0x1000 * (k - 7)
                regs[13] = SCRATCH + 0x8000
                vregs = [fbits(FSPECIAL[(index + k) % len(FSPECIAL)]) for k in range(32)]
                mem = bytearray((k + index) & 255 for k in range(256)) * (SCRATCH_SIZE // 256)
                cases.append((cls, [word], [ins], regs, (index * 73) & 511, vregs, 0, mem))
                count += 1
    return count


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    classes = sys.argv[3].split(',') if len(sys.argv) > 3 else list(GENS)
    if count < 1 or not classes or any(c not in GENS for c in classes):
        raise SystemExit("positive count and known instruction classes required")
    r = random.Random(seed)
    ctx = arm.Ctx(None)
    cases = []
    while len(cases) < count:
        cls = r.choice(classes)
        n = 1 if cls in ('mem', 'vfp') else r.randint(1, 4)
        words, insts = [], []
        while len(words) < n:
            w = GENS[cls](r)
            if cls == 'thumb':
                i = thumb.decode(CODE + 2 * len(words), w, 0, ctx)
            else:
                i = arm.decode(CODE + 4 * len(words), w, ctx)
            if not acceptable(w, i):
                continue
            words.append(w)
            insts.append(i)
        mem_test = cls in ('mem', 'vfp')
        regs = [rnd_reg(r) for _ in range(15)]
        if mem_test:
            for k in range(4):
                regs[k] = r.randrange(0, 0x40) & ~7
            for k in range(8, 13):
                regs[k] = SCRATCH + 0x1000 * (k - 7) + (r.randrange(0, 0x100) & ~7)
        regs[13] = SCRATCH + 0x8000
        flags = r.getrandbits(5)  # N Z C V Q -> bits 4..0
        ge = r.getrandbits(4)
        vregs = [rnd_freg(r) for _ in range(32)]
        fpscr = 0
        mem = bytearray(r.getrandbits(8) for _ in range(256)) * (SCRATCH_SIZE // 256)
        cases.append((cls, words, insts, regs, flags | (ge << 5), vregs, fpscr, mem))

    edge_count = add_edges(cases, classes, ctx)
    tmp = tempfile.mkdtemp(prefix='cpu_', dir=os.environ.get('TEST_CPU_WORK'))
    src = ['#include "cpu.h"', '#include <stdio.h>', '#include <stdlib.h>', '#ifdef _WIN32', '#include <windows.h>', '#else', '#include <sys/mman.h>', '#ifndef MAP_NORESERVE', '#define MAP_NORESERVE 0', '#endif', '#endif', 'u8 *g_mem;',
           'void rt_dispatch(CPU *c, u32 u) {} void rt_svc(CPU *c, u32 i) {} void rt_undefined(CPU *c, u32 a, u32 w) {}']
    for k, (cls, words, insts, regs, flags, vregs, fpscr, mem) in enumerate(cases):
        src.append(c_function('t%d' % k, insts))
    src.append('typedef void (*TF)(CPU *); static TF tests[] = {%s};' % ','.join('t%d' % k for k in range(len(cases))))
    src.append(r'''
int main(int argc, char **argv) {
    if (argc != 3) return 20;
#ifdef _WIN32
    g_mem = (u8 *)VirtualAlloc(0, (1ull << 32) + 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_mem || !VirtualAlloc(g_mem + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return 21;
#else
    g_mem = mmap(0, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) return 21;
#endif
    FILE *in = fopen(argv[1], "rb"); FILE *out = fopen(argv[2], "wb");
    if (!in || !out) return 22;
    for (int k = 0; k < (int)(sizeof tests / sizeof *tests); k++) {
        CPU c; memset(&c, 0, sizeof c);
        u32 buf[15 + 1 + 32 + 1];
        if (fread(buf, 4, 49, in) != 49) return 23;
        for (int i = 0; i < 15; i++) c.r[i] = buf[i];
        u32 f = buf[15];
        c.nf = (f >> 4) & 1; c.zf = (f >> 3) & 1; c.cf = (f >> 2) & 1; c.vf = (f >> 1) & 1; c.qf = f & 1; c.ge = (f >> 5) & 15;
        memcpy(c.s, buf + 16, 128);
        c.fpscr = buf[48];
        if (fread(g_mem + 0x10000, 1, 0x10000, in) != 0x10000) return 23;
        tests[k](&c);
        for (int i = 0; i < 15; i++) buf[i] = c.r[i];
        buf[15] = (c.nf << 4) | (c.zf << 3) | (c.cf << 2) | (c.vf << 1) | c.qf | (c.ge << 5);
        memcpy(buf + 16, c.s, 128);
        buf[48] = c.fpscr;
        if (fwrite(buf, 4, 49, out) != 49) return 24;
        if (fwrite(g_mem + 0x10000, 1, 0x10000, out) != 0x10000) return 24;
    }
    fclose(in);
    if (fclose(out)) return 24;
    return 0;
}''')
    cpath = os.path.join(tmp, 't.c')
    with open(cpath, 'w') as f:
        f.write('\n'.join(src))
    exe = os.path.join(tmp, 't.exe' if os.name == 'nt' else 't')
    cc = json.loads(os.environ['TEST_CPU_CC']) if os.environ.get('TEST_CPU_CC') else ['clang']
    if not isinstance(cc, list) or not cc or not all(isinstance(a, str) for a in cc):
        raise SystemExit('TEST_CPU_CC must be a nonempty JSON list of compiler arguments')
    includes = ['-I', os.path.join(ROOT, 'runtime')]
    port = os.path.abspath(os.path.join(ROOT, '..', '..', 'port'))
    if os.path.isfile(os.path.join(port, 'cpu.h')):
        includes = ['-I', port] + includes
    subprocess.check_call(cc + ['-O1', '-w', '-ffp-contract=off', '-fno-strict-aliasing',
                               '-MD', '-MF', os.path.join(tmp, 'dependencies.d')] + includes + [cpath, '-o', exe])
    inp = os.path.join(tmp, 'in.bin')
    outp = os.path.join(tmp, 'out.bin')
    with open(inp, 'wb') as f:
        for cls, words, insts, regs, flags, vregs, fpscr, mem in cases:
            f.write(struct.pack('<15I', *regs))
            f.write(struct.pack('<I', flags))
            f.write(struct.pack('<32I', *vregs))
            f.write(struct.pack('<I', fpscr))
            f.write(mem)
    subprocess.check_call([exe, inp, outp])
    data = open(outp, 'rb').read()
    rec = 49 * 4 + SCRATCH_SIZE
    if len(data) != rec * len(cases):
        raise RuntimeError('native output size mismatch: %d, expected %d' % (len(data), rec * len(cases)))
    fails = 0
    generated = Counter(case[0] for case in cases)
    executed, skipped, reasons, failcls = Counter(), Counter(), Counter(), Counter()
    nan_cases = nan_words = 0
    failures = []
    for k, (cls, words, insts, regs, flags, vregs, fpscr, mem) in enumerate(cases):
        d = data[k * rec:(k + 1) * rec]
        c_regs = list(struct.unpack_from('<15I', d, 0))
        c_flags = struct.unpack_from('<I', d, 60)[0]
        c_v = list(struct.unpack_from('<32I', d, 64))
        c_fps = struct.unpack_from('<I', d, 192)[0]
        c_mem = d[196:]
        try:
            u_regs, u_flags, u_ge, u_v, u_fps, u_mem = run_unicorn(words, regs, flags, vregs, fpscr, mem, cls == 'thumb')
        except Exception as e:
            skipped[cls] += 1
            reasons[type(e).__name__ + ': ' + str(e)] += 1
            continue
        executed[cls] += 1
        diffs = []
        for x in range(15):
            if c_regs[x] != u_regs[x]:
                diffs.append('r%d c=%08x uc=%08x' % (x, c_regs[x], u_regs[x]))
        if (c_flags & 0x1F) != u_flags:
            diffs.append('nzcvq c=%s uc=%s' % (format(c_flags & 0x1F, '05b'), format(u_flags, '05b')))
        if (c_flags >> 5) != u_ge:
            diffs.append('ge c=%x uc=%x' % (c_flags >> 5, u_ge))
        waived = False
        for x in range(32):
            if c_v[x] != u_v[x] and nan_equivalent(words, insts, c_v, u_v, x):
                nan_words += 1
                waived = True
            elif c_v[x] != u_v[x]:
                a, b = c_v[x], u_v[x]
                diffs.append('s%d c=%08x uc=%08x' % (x, a, b))
        nan_cases += int(waived)
        if c_fps != u_fps:
            diffs.append('fpscr c=%08x uc=%08x' % (c_fps, u_fps))
        if c_mem != u_mem:
            off = next(j for j in range(len(c_mem)) if c_mem[j] != u_mem[j])
            diffs.append('mem @%x' % (SCRATCH + off))
        if diffs:
            fails += 1
            failcls[cls] += 1
            if fails <= 25:
                failures.append(dict(case=k, cls=cls, words=words, notes=[i.note for i in insts],
                                     registers=regs, flags=flags, vfp=vregs, fpscr=fpscr, differences=diffs))
                print('FAIL %s:' % cls)
                for w, i in zip(words, insts):
                    print('   %08x  %s' % (w, i.note))
                print('   in: ' + ' '.join('%08x' % x for x in regs) + ' flags=%s' % format(flags, '09b'))
                for dd in diffs[:8]:
                    print('   ' + dd)
    report = dict(seed=seed, classes=classes, requested_random_blocks=count, edge_cases=edge_count,
                  generated=dict(generated), executed=dict(executed), skipped=dict(skipped), skip_reasons=dict(reasons),
                  mismatches=fails, mismatch_classes=dict(failcls), typed_nan_cases=nan_cases, typed_nan_words=nan_words,
                  fpscr_compared_mask='0xffffffff', failures=failures, temporary_case_dir=tmp,
                  memory_and_VFP_single_instruction=True, native_output_record_bytes=rec)
    report_path = os.environ.get('TEST_CPU_REPORT', os.path.join(tmp, 'report.json'))
    with open(report_path, 'w', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps(report, sort_keys=True))
    return 1 if fails or skipped else 0


if __name__ == '__main__':
    sys.exit(main())
