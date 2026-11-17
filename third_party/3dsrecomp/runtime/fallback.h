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

// Bounded ARM/Thumb fallback execution engine for missed code
inline void execute(CPU *c, u32 entry_pc) {
    u8 *M = g_mem;
    const u32 stop_pc = c->r[14];
    u32 steps = 0;

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
                const u32 rm = op & 0xFu;
                const u32 target = c->r[rm];
                u32 saved[9];
                for (int i = 0; i < 8; i++) saved[i] = c->r[4 + i];
                saved[8] = c->r[13];
                c->r[14] = next_pc;
                c->pc = target;
                rt_dispatch(c, 0);
                assert(c->pc == next_pc);
                for (int i = 0; i < 8; i++) c->r[4 + i] = saved[i];
                c->r[13] = saved[8];
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

            // B / BL <imm24>
            if ((op & 0x0E000000u) == 0x0A000000u) {
                const bool link = (op & 0x01000000u) != 0;
                s32 offset = op & 0x00FFFFFFu;
                if (offset & 0x00800000) offset |= 0xFF000000;
                offset = (offset << 2) + 8;
                if (link) c->r[14] = next_pc;
                c->pc = (at + offset);
                continue;
            }

            // Data processing (MOV, ADD, SUB, RSB, CMP, CMN, TST, AND, ORR, EOR, BIC)
            if ((op & 0x0C000000u) == 0x00000000u) {
                const bool imm = (op & 0x02000000u) != 0;
                const u32 opcode = (op >> 21) & 0xFu;
                const bool s = (op & 0x00100000u) != 0;
                const u32 rn = (op >> 16) & 0xFu;
                const u32 rd = (op >> 12) & 0xFu;
                u32 op2 = 0;

                if (imm) {
                    const u32 val = op & 0xFFu;
                    const u32 rot = (op >> 8) & 0xFu;
                    op2 = ror32_val(val, rot * 2);
                } else {
                    const u32 rm = op & 0xFu;
                    const u32 shift_type = (op >> 5) & 3u;
                    const u32 shift_imm = (op >> 7) & 0x1Fu;
                    op2 = c->r[rm];
                    if (shift_imm) {
                        if (shift_type == 0) op2 = op2 << shift_imm;
                        else if (shift_type == 1) op2 = op2 >> shift_imm;
                        else if (shift_type == 2) op2 = (s32)op2 >> shift_imm;
                        else if (shift_type == 3) op2 = ror32_val(op2, shift_imm);
                    }
                }

                u32 res = 0;
                switch (opcode) {
                    case 0x0: res = c->r[rn] & op2; break;         // AND
                    case 0x1: res = c->r[rn] ^ op2; break;         // EOR
                    case 0x2: res = c->r[rn] - op2; if (s) set_sub_flags(c, c->r[rn], op2, res); break; // SUB
                    case 0x3: res = op2 - c->r[rn]; if (s) set_sub_flags(c, op2, c->r[rn], res); break; // RSB
                    case 0x4: res = c->r[rn] + op2; if (s) set_add_flags(c, c->r[rn], op2, res); break; // ADD
                    case 0x8: set_nz(c, c->r[rn] & op2); break;    // TST
                    case 0xA: set_sub_flags(c, c->r[rn], op2, c->r[rn] - op2); break; // CMP
                    case 0xB: set_add_flags(c, c->r[rn], op2, c->r[rn] + op2); break; // CMN
                    case 0xC: res = c->r[rn] | op2; break;         // ORR
                    case 0xD: res = op2; if (s) set_nz(c, res); break; // MOV
                    case 0xE: res = c->r[rn] & ~op2; break;        // BIC
                    default:
                        FATAL("fallback interpreter unsupported ARM opcode 0x%08x at 0x%08x", op, at);
                }

                if (opcode != 0x8 && opcode != 0xA && opcode != 0xB) {
                    c->r[rd] = res;
                    if (s && opcode != 0x2 && opcode != 0x3 && opcode != 0x4 && opcode != 0xD) {
                        set_nz(c, res);
                    }
                }
                c->pc = next_pc;
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
                const u32 rm = (op >> 3) & 0xFu;
                const u32 target = c->r[rm];
                u32 saved[9];
                for (int i = 0; i < 8; i++) saved[i] = c->r[4 + i];
                saved[8] = c->r[13];
                c->r[14] = next_pc;
                c->pc = target;
                rt_dispatch(c, 0);
                assert(c->pc == next_pc);
                for (int i = 0; i < 8; i++) c->r[4 + i] = saved[i];
                c->r[13] = saved[8];
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

            // MOV Rd, Rm (high/low registers)
            if ((op & 0xFF00u) == 0x4600u) {
                const u32 rd = (op & 7u) | ((op >> 4) & 8u);
                const u32 rm = (op >> 3) & 0xFu;
                c->r[rd] = c->r[rm];
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

            FATAL("fallback interpreter unsupported Thumb opcode 0x%04x at 0x%08x", op, at);
        }
    }

    FATAL("fallback interpreter step budget exceeded (%u steps) at 0x%08x", kMaxSteps, c->pc);
}

} // namespace fallback
