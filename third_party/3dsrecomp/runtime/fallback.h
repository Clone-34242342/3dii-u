#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include "cpu.h"
#include "rt.h"

namespace fallback {

constexpr u32 kMaxSteps = 50000;
constexpr u32 kLinearMax = 0x14000000u;
constexpr u32 kCroBase = 0x1C000000u;
constexpr u32 kCroEnd = 0x20000000u;
constexpr u32 kMagicCallReturn = 0xFFFF0004u;

inline bool is_valid_code_address(u32 addr) {
    const u32 a = addr & ~1u;
    return (a >= mem::CODE_BASE && a < kLinearMax) ||
           (a >= kCroBase && a < kCroEnd);
}

inline bool eval_cond(const CPU *c, u32 cond) {
    const bool n = c->nf, z = c->zf, cv = c->cf, v = c->vf;
    switch (cond) {
        case 0: return z;                   // EQ
        case 1: return !z;                  // NE
        case 2: return cv;                  // CS / HS
        case 3: return !cv;                 // CC / LO
        case 4: return n;                   // MI
        case 5: return !n;                  // PL
        case 6: return v;                   // VS
        case 7: return !v;                  // VC
        case 8: return cv && !z;            // HI
        case 9: return !cv || z;            // LS
        case 10: return n == v;             // GE
        case 11: return n != v;             // LT
        case 12: return !z && (n == v);     // GT
        case 13: return z || (n != v);      // LE
        case 14: return true;               // AL
        default: return false;              // NV
    }
}

inline void set_nz(CPU *c, u32 val) {
    c->nf = (val >> 31) & 1;
    c->zf = (val == 0);
}

inline void set_add_flags(CPU *c, u32 a, u32 b, u32 res) {
    set_nz(c, res);
    c->cf = res < a;
    c->vf = ((~(a ^ b) & (a ^ res)) >> 31) & 1;
}

inline void set_sub_flags(CPU *c, u32 a, u32 b, u32 res) {
    set_nz(c, res);
    c->cf = a >= b;  // ARM carry on borrow: NOT borrow
    c->vf = (((a ^ b) & (a ^ res)) >> 31) & 1;
}

inline u32 ror32_val(u32 val, u32 rot) {
    rot &= 31;
    return rot ? (val >> rot) | (val << (32 - rot)) : val;
}

// A call out of interpreted code into compiled code, back at return_pc: the callee keeps r4-r11 and sp (AAPCS), restored here all the same.
inline void call_compiled(CPU *c, u32 target, u32 return_pc) {
    u32 saved[9];
    for (int i = 0; i < 8; i++) saved[i] = c->r[4 + i];
    saved[8] = c->r[13];
    c->r[14] = return_pc;
    c->pc = target;
    rt_dispatch(c, 0);
    assert(c->pc == return_pc);
    for (int i = 0; i < 8; i++) c->r[4 + i] = saved[i];
    c->r[13] = saved[8];
}

// A call to code that may or may not be compiled: compiled code runs as such, the rest is interpreted here, one function deep.
inline void call_any(CPU *c, u32 target, u32 return_pc) {
    if (dispatch_lookup(target) || dispatch_lookup(target ^ 1u)) {
        call_compiled(c, target, return_pc);
        return;
    }
    c->r[14] = return_pc;
    c->pc = target;
}

// The ARM data-processing operand 2 and the shifter's carry out (carry in: the C flag, which an unshifted operand keeps).
// Register-specified shifts (bit 4) take the low byte of Rs; an immediate shift of 0 means LSR #32, ASR #32 or RRX for types 1 to 3.
inline u32 shifter_operand(const CPU *c, u32 op, u32 at, bool &carry) {
    carry = c->cf;
    if (op & 0x02000000u) {
        const u32 rot = ((op >> 8) & 0xFu) * 2;
        const u32 val = ror32_val(op & 0xFFu, rot);
        if (rot) carry = val >> 31;
        return val;
    }
    const u32 rm = op & 0xFu;
    u32 val = rm == 15 ? at + 8 : c->r[rm];
    const u32 type = (op >> 5) & 3u;
    u32 n;
    if (op & 0x10u) {  // by register
        if (rm == 15) val = at + 12;
        n = c->r[(op >> 8) & 0xFu] & 0xFFu;
        if (n == 0) return val;
    } else {
        n = (op >> 7) & 0x1Fu;
        if (n == 0) {
            if (type == 0) return val;
            if (type == 3) {  // RRX
                carry = val & 1;
                return (u32)c->cf << 31 | val >> 1;
            }
            n = 32;
        }
    }
    switch (type) {
    case 0:  // LSL
        if (n >= 32) { carry = n == 32 ? val & 1 : 0; return 0; }
        carry = (val >> (32 - n)) & 1;
        return val << n;
    case 1:  // LSR
        if (n >= 32) { carry = n == 32 ? val >> 31 : 0; return 0; }
        carry = (val >> (n - 1)) & 1;
        return val >> n;
    case 2:  // ASR
        if (n >= 32) { carry = val >> 31; return (u32)((s32)val >> 31); }
        carry = (val >> (n - 1)) & 1;
        return (u32)((s32)val >> n);
    default:  // ROR
        n &= 31;
        if (n == 0) { carry = val >> 31; return val; }
        carry = (val >> (n - 1)) & 1;
        return ror32_val(val, n);
    }
}

// Bounded ARM/Thumb fallback execution engine for missed code
inline void execute(CPU *c, u32 entry_pc) {
    u8 *M = g_mem;
    const u32 stop_pc = c->r[14];
    u32 steps = 0;
    LOG("[fallback] executed missed function at %08x (lr=%08x)\n", entry_pc, stop_pc);

    while (steps++ < kMaxSteps) {
        if (c->pc == stop_pc || c->pc == kMagicCallReturn) {
            return;
        }

        const bool thumb = c->pc & 1;
        const u32 at = c->pc & ~1u;

        if (!is_valid_code_address(at)) {
            FATAL("fallback interpreter jump to invalid code address 0x%08x (lr=0x%08x)", c->pc, c->r[14]);
        }

        const u32 next_pc = (at + (thumb ? 2 : 4)) | thumb;

        if (!thumb) {
            // ARM mode (32-bit)
            const u32 op = RD32(at);
            const u32 cond = op >> 28;

            if (cond == 15) {  // ARMv5+: the unconditional space, not "never"
                if ((op & 0xFE000000u) == 0xFA000000u) {  // BLX <imm24>: to Thumb, the H bit the halfword
                    s32 offset = op & 0x00FFFFFFu;
                    if (offset & 0x00800000) offset |= 0xFF000000;
                    const u32 target = (at + 8 + (u32)(offset << 2) + ((op >> 23) & 2u)) | 1u;
                    call_any(c, target, next_pc);
                    continue;
                }
                FATAL("fallback interpreter unsupported unconditional ARM instruction 0x%08x at 0x%08x", op, at);
            }
            if (cond != 14 && !eval_cond(c, cond)) {
                c->pc = next_pc;
                continue;
            }

            // BX / BLX Rm
            if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
                const u32 rm = op & 0xFu;
                c->pc = c->r[rm];
                if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                continue;
            }
            if ((op & 0x0FFFFFF0u) == 0x012FFF30u) {
                call_compiled(c, c->r[op & 0xFu], next_pc);
                continue;
            }

            // PUSH / POP (STMDB sp!, {regs} / LDMIA sp!, {regs})
            if ((op & 0x0FFF0000u) == 0x092D0000u) {
                // STMDB sp!, {reg_list}
                const u16 list = op & 0xFFFFu;
                int count = 0;
                for (int i = 0; i < 16; i++) if (list & (1 << i)) count++;
                c->r[13] -= count * 4;
                u32 sp = c->r[13];
                for (int i = 0; i < 16; i++) {
                    if (list & (1 << i)) {
                        WR32(sp, c->r[i]);
                        sp += 4;
                    }
                }
                c->pc = next_pc;
                continue;
            }
            if ((op & 0x0FFF0000u) == 0x08BD0000u) {
                // LDMIA sp!, {reg_list}
                const u16 list = op & 0xFFFFu;
                u32 sp = c->r[13];
                for (int i = 0; i < 16; i++) {
                    if (list & (1 << i)) {
                        c->r[i] = RD32(sp);
                        sp += 4;
                    }
                }
                c->r[13] = sp;
                if (list & (1 << 15)) {
                    // Loaded PC
                    c->pc = c->r[15];
                    if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                } else {
                    c->pc = next_pc;
                }
                continue;
            }

            // B / BL <imm24>: a BL into compiled code runs it compiled (call_any); the rest of a BL's callee is interpreted
            if ((op & 0x0E000000u) == 0x0A000000u) {
                const bool link = (op & 0x01000000u) != 0;
                s32 offset = op & 0x00FFFFFFu;
                if (offset & 0x00800000) offset |= 0xFF000000;
                const u32 target = at + 8 + (u32)(offset << 2);
                if (link) call_any(c, target, next_pc);
                else c->pc = target;
                continue;
            }

            // Multiplies share the halfword transfers' shape (bits 7:4 = 1001) with bits 6:5 = 00: MUL and MLA here, the rest stop
            if ((op & 0x0FC000F0u) == 0x00000090u) {
                const u32 rd = (op >> 16) & 0xFu, rn = (op >> 12) & 0xFu, rs = (op >> 8) & 0xFu, rm = op & 0xFu;
                const u32 res = c->r[rm] * c->r[rs] + ((op & 0x00200000u) ? c->r[rn] : 0);
                c->r[rd] = res;
                if (op & 0x00100000u) set_nz(c, res);
                c->pc = next_pc;
                continue;
            }
            if ((op & 0x0E000090u) == 0x00000090u && !(op & 0x60u)) {  // long multiplies, swaps
                FATAL("fallback interpreter unsupported ARM opcode 0x%08x at 0x%08x", op, at);
            }

            // Halfword and Signed Data Transfer (LDRH, STRH, LDRSB, LDRSH), and the doubleword pair (LDRD, STRD)
            if ((op & 0x0E000090u) == 0x00000090u) {
                const bool p = (op & 0x01000000u) != 0;
                const bool u = (op & 0x00800000u) != 0;
                const bool imm = (op & 0x00400000u) != 0;
                const bool w = (op & 0x00200000u) != 0;
                const bool l = (op & 0x00100000u) != 0;
                const bool s = (op & 0x00000040u) != 0;
                const bool h = (op & 0x00000020u) != 0;
                const u32 rn = (op >> 16) & 0xFu;
                const u32 rd = (op >> 12) & 0xFu;
                const u32 offset = imm ? (((op >> 4) & 0xF0u) | (op & 0x0Fu)) : c->r[op & 0xFu];

                const u32 base = (rn == 15) ? (at + 8) : c->r[rn];
                const u32 addr = p ? (u ? (base + offset) : (base - offset)) : base;
                const u32 new_base = u ? (base + offset) : (base - offset);

                if (!s && h) {
                    if (l) c->r[rd] = RD16(addr);
                    else WR16(addr, c->r[rd]);
                } else if (s && !h && l) {
                    c->r[rd] = (s32)(s8)RD8(addr);
                } else if (s && h && l) {
                    c->r[rd] = (s32)(s16)RD16(addr);
                } else if (s && !h) {  // LDRD: Rd and Rd+1 (Rd even)
                    c->r[rd] = RD32(addr);
                    c->r[rd + 1] = RD32(addr + 4);
                } else {  // STRD
                    WR32(addr, c->r[rd]);
                    WR32(addr + 4, c->r[rd + 1]);
                }

                if (!p || w) {
                    if (!l || rn != rd) c->r[rn] = new_base;
                }

                if (l && rd == 15) {
                    c->pc = c->r[15];
                    if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                } else {
                    c->pc = next_pc;
                }
                continue;
            }

            // Data processing. Rn = 15 reads PC+8 (ADR: add r0, pc, #n); Rd = 15 branches (mov pc, lr; add pc, pc, r0, lsl #2).
            // The MSR/MRS and BX space (bits 24:23 = 10 with S = 0) is not data processing.
            if ((op & 0x0C000000u) == 0x00000000u && (op & 0x01900000u) != 0x01000000u) {
                const u32 opcode = (op >> 21) & 0xFu;
                const bool s = (op & 0x00100000u) != 0;
                const u32 rn = (op >> 16) & 0xFu;
                const u32 rd = (op >> 12) & 0xFu;
                bool carry;
                const u32 op2 = shifter_operand(c, op, at, carry);
                const u32 a = rn == 15 ? at + 8 : c->r[rn];
                const u32 cin = c->cf;
                u32 res = 0;
                bool logical = false;
                switch (opcode) {
                    case 0x0: res = a & op2; logical = true; break;                                                   // AND
                    case 0x1: res = a ^ op2; logical = true; break;                                                   // EOR
                    case 0x2: res = a - op2; if (s) set_sub_flags(c, a, op2, res); break;                             // SUB
                    case 0x3: res = op2 - a; if (s) set_sub_flags(c, op2, a, res); break;                             // RSB
                    case 0x4: res = a + op2; if (s) set_add_flags(c, a, op2, res); break;                             // ADD
                    case 0x5: {                                                                                      // ADC
                        const u64 sum = (u64)a + op2 + cin;
                        res = (u32)sum;
                        if (s) { set_nz(c, res); c->cf = (u32)(sum >> 32); c->vf = ((~(a ^ op2) & (a ^ res)) >> 31) & 1; }
                        break;
                    }
                    case 0x6: case 0x7: {                                                                            // SBC, RSC
                        const u32 x = opcode == 0x6 ? a : op2, y = opcode == 0x6 ? op2 : a, borrow = cin ? 0 : 1;
                        res = x - y - borrow;
                        if (s) { set_nz(c, res); c->cf = (u64)x >= (u64)y + borrow; c->vf = (((x ^ y) & (x ^ res)) >> 31) & 1; }
                        break;
                    }
                    case 0x8: res = a & op2; logical = true; break;                                                   // TST
                    case 0x9: res = a ^ op2; logical = true; break;                                                   // TEQ
                    case 0xA: set_sub_flags(c, a, op2, a - op2); break;                                               // CMP
                    case 0xB: set_add_flags(c, a, op2, a + op2); break;                                               // CMN
                    case 0xC: res = a | op2; logical = true; break;                                                   // ORR
                    case 0xD: res = op2; logical = true; break;                                                       // MOV
                    case 0xE: res = a & ~op2; logical = true; break;                                                  // BIC
                    default: res = ~op2; logical = true; break;                                                       // MVN
                }
                if (logical && (s || opcode == 0x8 || opcode == 0x9)) {
                    set_nz(c, res);
                    c->cf = carry;
                }
                if (opcode >= 0x8 && opcode <= 0xB) {  // the comparisons write no register
                    c->pc = next_pc;
                    continue;
                }
                if (rd == 15) {
                    if (s) FATAL("fallback interpreter: an exception return (S with Rd = pc) 0x%08x at 0x%08x", op, at);
                    c->pc = res;
                    if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                    continue;
                }
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // Single Data Transfer (LDR, STR, LDRB, STRB)
            if ((op & 0x0C000000u) == 0x04000000u) {
                const bool imm = (op & 0x02000000u) == 0;
                const bool p = (op & 0x01000000u) != 0;
                const bool u = (op & 0x00800000u) != 0;
                const bool b = (op & 0x00400000u) != 0;
                const bool w = (op & 0x00200000u) != 0;
                const bool l = (op & 0x00100000u) != 0;
                const u32 rn = (op >> 16) & 0xFu;
                const u32 rd = (op >> 12) & 0xFu;
                u32 offset = 0;

                if (imm) {
                    offset = op & 0xFFFu;
                } else {
                    const u32 rm = op & 0xFu;
                    const u32 shift_type = (op >> 5) & 3u;
                    const u32 shift_imm = (op >> 7) & 0x1Fu;
                    offset = c->r[rm];
                    if (shift_imm) {
                        if (shift_type == 0) offset = offset << shift_imm;
                        else if (shift_type == 1) offset = offset >> shift_imm;
                        else if (shift_type == 2) offset = (s32)offset >> shift_imm;
                        else if (shift_type == 3) offset = ror32_val(offset, shift_imm);
                    }
                }

                const u32 base = (rn == 15) ? (at + 8) : c->r[rn];
                const u32 addr = p ? (u ? (base + offset) : (base - offset)) : base;
                const u32 new_base = u ? (base + offset) : (base - offset);

                if (l) {
                    c->r[rd] = b ? RD8(addr) : RD32(addr);
                } else {
                    if (b) WR8(addr, c->r[rd]);
                    else WR32(addr, c->r[rd]);
                }

                if (!p || w) {
                    if (!l || rn != rd) c->r[rn] = new_base;
                }

                if (l && rd == 15) {
                    c->pc = c->r[15];
                    if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                } else {
                    c->pc = next_pc;
                }
                continue;
            }

            FATAL("fallback interpreter unsupported ARM opcode 0x%08x at 0x%08x", op, at);
        } else {
            // Thumb mode (16-bit)
            const u16 op = RD16(at);

            // PUSH {regs, lr}
            if ((op & 0xFE00u) == 0xB400u) {
                const u16 list = op & 0x00FFu;
                const bool push_lr = (op & 0x0100u) != 0;
                int count = 0;
                for (int i = 0; i < 8; i++) if (list & (1 << i)) count++;
                if (push_lr) count++;
                c->r[13] -= count * 4;
                u32 sp = c->r[13];
                for (int i = 0; i < 8; i++) {
                    if (list & (1 << i)) {
                        WR32(sp, c->r[i]);
                        sp += 4;
                    }
                }
                if (push_lr) WR32(sp, c->r[14]);
                c->pc = next_pc;
                continue;
            }

            // POP {regs, pc}
            if ((op & 0xFE00u) == 0xBC00u) {
                const u16 list = op & 0x00FFu;
                const bool pop_pc = (op & 0x0100u) != 0;
                u32 sp = c->r[13];
                for (int i = 0; i < 8; i++) {
                    if (list & (1 << i)) {
                        c->r[i] = RD32(sp);
                        sp += 4;
                    }
                }
                if (pop_pc) {
                    c->pc = RD32(sp);
                    sp += 4;
                } else {
                    c->pc = next_pc;
                }
                c->r[13] = sp;
                if (pop_pc && (c->pc == stop_pc || c->pc == kMagicCallReturn)) return;
                continue;
            }

            // BX / BLX Rm
            if ((op & 0xFF87u) == 0x4700u) {
                const u32 rm = (op >> 3) & 0xFu;
                c->pc = c->r[rm];
                if (c->pc == stop_pc || c->pc == kMagicCallReturn) return;
                continue;
            }
            if ((op & 0xFF87u) == 0x4780u) {
                call_compiled(c, c->r[(op >> 3) & 0xFu], next_pc);
                continue;
            }

            // BL / BLX <imm22>: two halfwords, the first (0xF000) holds the high part of the offset from this pair's PC+4
            if ((op & 0xF800u) == 0xF000u) {
                const u16 second = RD16(at + 2);
                if ((second & 0xE800u) != 0xE800u) FATAL("fallback interpreter: a BL prefix without its suffix at 0x%08x", at);
                s32 high = op & 0x07FFu;
                if (high & 0x0400) high |= (s32)0xFFFFF800;
                const u32 return_pc = (at + 4) | 1u;
                u32 target = at + 4 + (u32)(high << 12) + ((second & 0x07FFu) << 1);
                target = (second & 0x1000u) ? target | 1u : target & ~3u;  // BL stays Thumb, BLX goes to ARM
                call_any(c, target, return_pc);
                continue;
            }

            // MOVS Rd, #imm8
            if ((op & 0xF800u) == 0x2000u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 imm8 = op & 0xFFu;
                c->r[rd] = imm8;
                set_nz(c, imm8);
                c->pc = next_pc;
                continue;
            }

            // MOV Rd, Rm (high/low registers); Rd = pc branches and stays Thumb (ARMv6: no interworking here, that's BX)
            if ((op & 0xFF00u) == 0x4600u) {
                const u32 rd = (op & 7u) | ((op >> 4) & 8u);
                const u32 rm = (op >> 3) & 0xFu;
                const u32 val = rm == 15 ? at + 4 : c->r[rm];
                if (rd == 15) {
                    c->pc = (val & ~1u) | 1u;
                    if (c->pc == stop_pc || c->pc == kMagicCallReturn || (c->pc & ~1u) == (stop_pc & ~1u)) return;
                    continue;
                }
                c->r[rd] = val;
                c->pc = next_pc;
                continue;
            }

            // ADDS Rd, Rn, Rm
            if ((op & 0xFE00u) == 0x1800u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 rm = (op >> 6) & 7u;
                const u32 res = c->r[rn] + c->r[rm];
                set_add_flags(c, c->r[rn], c->r[rm], res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // SUBS Rd, Rn, Rm
            if ((op & 0xFE00u) == 0x1A00u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 rm = (op >> 6) & 7u;
                const u32 res = c->r[rn] - c->r[rm];
                set_sub_flags(c, c->r[rn], c->r[rm], res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // ADDS Rd, Rn, #imm3
            if ((op & 0xFE00u) == 0x1C00u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 imm3 = (op >> 6) & 7u;
                const u32 res = c->r[rn] + imm3;
                set_add_flags(c, c->r[rn], imm3, res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // SUBS Rd, Rn, #imm3
            if ((op & 0xFE00u) == 0x1E00u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 imm3 = (op >> 6) & 7u;
                const u32 res = c->r[rn] - imm3;
                set_sub_flags(c, c->r[rn], imm3, res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // ADDS Rd, #imm8
            if ((op & 0xF800u) == 0x3000u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 imm8 = op & 0xFFu;
                const u32 res = c->r[rd] + imm8;
                set_add_flags(c, c->r[rd], imm8, res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // SUBS Rd, #imm8
            if ((op & 0xF800u) == 0x3800u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 imm8 = op & 0xFFu;
                const u32 res = c->r[rd] - imm8;
                set_sub_flags(c, c->r[rd], imm8, res);
                c->r[rd] = res;
                c->pc = next_pc;
                continue;
            }

            // CMP Rd, #imm8
            if ((op & 0xF800u) == 0x2800u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 imm8 = op & 0xFFu;
                set_sub_flags(c, c->r[rd], imm8, c->r[rd] - imm8);
                c->pc = next_pc;
                continue;
            }

            // B<cond> <imm8>
            if ((op & 0xF000u) == 0xD000u) {
                const u32 cond = (op >> 8) & 0xFu;
                if (cond < 14) {
                    if (eval_cond(c, cond)) {
                        s32 offset = (s8)(op & 0xFFu);
                        offset = (offset << 1) + 4;
                        c->pc = (at + offset) | 1u;
                    } else {
                        c->pc = next_pc;
                    }
                    continue;
                }
            }

            // B <imm11>
            if ((op & 0xF800u) == 0xE000u) {
                s32 offset = op & 0x07FFu;
                if (offset & 0x0400) offset |= 0xFFFFF800;
                offset = (offset << 1) + 4;
                c->pc = (at + offset) | 1u;
                continue;
            }

            // LDR Rd, [PC, #imm8*4] (Literal Pool)
            if ((op & 0xF800u) == 0x4800u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 addr = ((at + 4) & ~2u) + (op & 0xFFu) * 4;
                c->r[rd] = RD32(addr);
                c->pc = next_pc;
                continue;
            }

            // STR / LDR Rd, [Rn, #imm5*4]
            if ((op & 0xF800u) == 0x6000u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu) * 4;
                WR32(addr, c->r[rd]);
                c->pc = next_pc;
                continue;
            }
            if ((op & 0xF800u) == 0x6800u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu) * 4;
                c->r[rd] = RD32(addr);
                c->pc = next_pc;
                continue;
            }

            // STRB / LDRB Rd, [Rn, #imm5]
            if ((op & 0xF800u) == 0x7000u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu);
                WR8(addr, c->r[rd]);
                c->pc = next_pc;
                continue;
            }
            if ((op & 0xF800u) == 0x7800u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu);
                c->r[rd] = RD8(addr);
                c->pc = next_pc;
                continue;
            }

            // STRH / LDRH Rd, [Rn, #imm5*2]
            if ((op & 0xF800u) == 0x8000u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu) * 2;
                WR16(addr, c->r[rd]);
                c->pc = next_pc;
                continue;
            }
            if ((op & 0xF800u) == 0x8800u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 addr = c->r[rn] + ((op >> 6) & 0x1Fu) * 2;
                c->r[rd] = RD16(addr);
                c->pc = next_pc;
                continue;
            }

            // STR / LDR Rd, [SP, #imm8*4]
            if ((op & 0xF800u) == 0x9000u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 addr = c->r[13] + (op & 0xFFu) * 4;
                WR32(addr, c->r[rd]);
                c->pc = next_pc;
                continue;
            }
            if ((op & 0xF800u) == 0x9800u) {
                const u32 rd = (op >> 8) & 7u;
                const u32 addr = c->r[13] + (op & 0xFFu) * 4;
                c->r[rd] = RD32(addr);
                c->pc = next_pc;
                continue;
            }

            // Register-offset Load/Store (LDR, STR, LDRH, STRH, LDRB, STRB, LDRSB, LDRSH)
            if ((op & 0xF200u) == 0x5000u) {
                const u32 rd = op & 7u;
                const u32 rn = (op >> 3) & 7u;
                const u32 rm = (op >> 6) & 7u;
                const u32 addr = c->r[rn] + c->r[rm];
                const u32 subop = (op >> 9) & 7u;

                switch (subop) {
                    case 0: WR32(addr, c->r[rd]); break;             // STR
                    case 1: WR16(addr, c->r[rd]); break;             // STRH
                    case 2: WR8(addr, c->r[rd]); break;              // STRB
                    case 3: c->r[rd] = (s32)(s8)RD8(addr); break;    // LDRSB
                    case 4: c->r[rd] = RD32(addr); break;            // LDR
                    case 5: c->r[rd] = RD16(addr); break;            // LDRH
                    case 6: c->r[rd] = RD8(addr); break;             // LDRB
                    case 7: c->r[rd] = (s32)(s16)RD16(addr); break;  // LDRSH
                }
                c->pc = next_pc;
                continue;
            }

            FATAL("fallback interpreter unsupported Thumb opcode 0x%04x at 0x%08x", op, at);
        }
    }

    FATAL("fallback interpreter step budget exceeded (%u steps) at 0x%08x", kMaxSteps, c->pc);
}

} // namespace fallback
