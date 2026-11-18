// A program from the console's own shader compiler, made fit for a real GPU: shader_compile.cpp runs every one through fit_program.
// CafeGLSL 1.0, which the console runs, writes two things that 0.2 never does. 0.2 is the PC's: it compiles every shader a build ships, and
// consoles have drawn with its programs since the first port. An emulator takes both, so Cemu showed nothing, while testers' consoles drew
// scrambled what the vertex shaders compiled there shaded, and right what the build had shipped (3D Land, New Leaf, Omega Ruby, 2026-11-12).
//   - A uniform read by a computed index (a bone's matrix) is a fetch from the uniform block's buffer, and 1.0 sets the fetch's
//     USE_CONST_FIELDS: the format of the data then comes from the buffer and not from the instruction, which says four floats. GX2 gives
//     a uniform block's buffer no format. With the bit cleared the instruction is field for field what 0.2 writes.
//   - An instruction with three operands always stores its result, and where only the next group reads it (through PV) 1.0 stores it in
//     register 123. The default partition gives vertex programs 48 registers; larger programs reserve more before use. The store goes to the first register
//     the program doesn't use, and the program says it uses one more.
//   - Two instructions of a group may store to the same component of register 123: one in a vector unit and one in the scalar unit, each
//     read by the next group through its own latch (PV, PS). A GPU keeps the two apart whatever becomes of the register. Cemu reads both
//     back from the register and gets the later result twice: Pokemon Ultra Sun's road drew in one flat colour, its light table's entry
//     taken for the alpha test's value (2026-11-13). The second of such a pair goes to a register of its own, the one after the first's.
//     This one is for Cemu: a console needs it no more than it minds it.
// Which of the first two a console minds isn't known. Every vertex program 1.0 makes for a game has the second and all that index a uniform the
// first; pixel programs, which consoles have drawn right from 1.0, almost never have the first, and their 123 is inside their 200. They are
// the only two differences that comparing both compilers' programs for all 632 of 3D Land's shaders found among the rules a GPU holds a
// program to and an emulator doesn't (tools/gsh_rules.py checks those). No console has run this yet.
// Nothing of the console's here, so a PC can run it: tools/test_latte_program.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include "shader_budget.h"

namespace gpu {
constexpr uint32_t kVertexRegisters = 48, kPixelRegisters = 200;  // default partition limits retained for the fitter regression fixtures

namespace latte {
// a program is 32-bit words, least significant byte first, whatever the CPU reading it
inline uint32_t word(const uint8_t *p, uint32_t i) { return p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24; }
inline void set_word(uint8_t *p, uint32_t i, uint32_t v) {
    for (uint32_t b = 0; b < 4; b++) p[4 * i + b] = (uint8_t)(v >> (8 * b));
}

// CafeGLSL can report success with only a masked color EXPORT_DONE: no
// calculation, fetch, kill or color output. A real eight-byte export is valid.
inline bool hollow_pixel_program(const void *program, uint32_t bytes) {
    if (!program || bytes != 8) return false;
    const auto *p = static_cast<const uint8_t *>(program);
    const uint32_t w0 = word(p, 0), w1 = word(p, 1);
    return ((w1 >> 23) & 127) == 40 && (w1 & (1u << 21)) &&
           ((w0 >> 13) & 3) == 0 && (w1 & 0xfff) == 0xfff;
}

constexpr uint32_t kThrowaway = 123, kLiteral = 253;
// The same unary opcodes as gsh_rules.py: their unused second selector is neither a GPR nor a literal read.
inline bool one_operand(uint32_t op) {
    return (op >= 16 && op <= 22) || op == 24 || op == 25 || op == 26 || op == 51 ||
           (op >= 97 && op <= 111) || (op >= 119 && op <= 121);
}

// One pass over a program: its control flow list from the start, and each clause the list names. Returns whether it is laid out as a program is
// (every clause inside it, every instruction group ended, the list ended by its END_OF_PROGRAM before the first clause begins). `top` gets
// the ordinary register count it reads or writes, the throwaway one aside, and `spares` how many registers the stores to that one need: one, or
// two if two of a group store to the same component of it. With `to`, those stores go to register `to`, the second of such a pair to the
// one after it, and uniform fetches lose their USE_CONST_FIELDS.
inline bool walk(uint8_t *p, uint32_t words, uint32_t &top, uint32_t &spares, const uint32_t *to, bool repair = true, bool *reads123 = nullptr) {
    top = spares = 0;
    if (reads123) *reads123 = false;
    uint32_t first_clause = words / 2;  // in instructions (two words each), as the control flow list counts
    uint32_t furthest_target = 0;
    auto reg = [&](uint32_t r) {
        if (r < shader_budget::kOrdinary) {
            if (r + 1 > top) top = r + 1;
            if (r == kThrowaway && reads123) *reads123 = true;
        }
    };
    for (uint32_t pc = 0; pc < first_clause; pc++) {
        const uint32_t w0 = word(p, 2 * pc), w1 = word(p, 2 * pc + 1);
        if (w1 & (1u << 29)) {  // arithmetic: address and count, then instructions two words each, a group's literals after its last one
            uint32_t i = w0 & 0x3FFFFF;
            const uint32_t end = i + ((w1 >> 18) & 0x7F) + 1;
            if (i <= pc || end > words / 2) return false;
            if (i < first_clause) first_clause = i;
            uint32_t literals = 0;  // of the group so far: the highest literal component named, plus one
            uint32_t thrown = 0;    // and the components of the throwaway register stored to
            bool ended = true;
            while (i < end) {
                const uint32_t a0 = word(p, 2 * i), a1 = word(p, 2 * i + 1);
                const bool three = (a1 >> 15) & 7;
                const uint32_t dst = (a1 >> 21) & 0x7F, sources = three ? 3 : one_operand((a1 >> 7) & 0x7FF) ? 1 : 2;
                for (uint32_t s = 0; s < sources; s++) {
                    const uint32_t sel = s == 0 ? a0 & 0x1FF : s == 1 ? (a0 >> 13) & 0x1FF : a1 & 0x1FF;
                    const uint32_t chan = s == 0 ? (a0 >> 10) & 3 : s == 1 ? (a0 >> 23) & 3 : (a1 >> 10) & 3;
                    const bool relative = s == 0 ? a0 & (1u << 9) : s == 1 ? a0 & (1u << 22) : a1 & (1u << 9);
                    if (sel < 128 && relative) return false;  // the computed register cannot be bounded by its base alone
                    if (sel == kLiteral && chan + 1 > literals) literals = chan + 1;
                    reg(sel);  // 124 to 127 are a clause's own, and 128 up constants: neither counts
                }
                if (three || (a1 & 0x10)) {  // it stores: two-operand instructions only with their write bit
                    if (a1 & (1u << 28)) return false;
                    if (dst == kThrowaway && repair) {
                        const uint32_t component = 1u << ((a1 >> 29) & 3), again = (thrown & component) != 0;
                        thrown |= component;
                        if (spares < 1 + again) spares = 1 + again;
                        if (to) set_word(p, 2 * i + 1, (a1 & ~(0x7Fu << 21)) | ((*to + again) << 21));
                    }
                    else reg(dst);
                }
                i++;
                ended = a0 >> 31;
                if (ended) {
                    i += (literals + 1) / 2;
                    literals = thrown = 0;
                }
            }
            if (i != end || !ended) return false;
            continue;
        }
        const uint32_t kind = (w1 >> 23) & 0x7F;
        // Mesa r600_isa.c marks 4..17 as LOOP/BRANCH; their full WORD0 address is in CF instructions (r600_asm.c).
        if (kind >= 4 && kind <= 17 && w0 > furthest_target) furthest_target = w0;
        if (kind == 18 || kind == 20 || (kind == 19 && (w0 || (w1 & (63u << 13))))) return false;  // CALL/RET need a subroutine walk; canonical CALL_FS invokes the separate fetch shader
        if (kind == 39 || kind == 40) {  // exports: a register, and with a burst the ones after it
            const uint32_t last = ((w0 >> 15) & 0x7F) + ((w1 >> 17) & 15);
            if (last > 127 || (w0 & (1u << 22))) return false;
            if ((w1 & 7) < 4 || ((w1 >> 3) & 7) < 4 || ((w1 >> 6) & 7) < 4 || ((w1 >> 9) & 7) < 4) {
                if (last >= shader_budget::kOrdinary) return false;  // clause temporaries never survive to an export
                reg(last);
            }
        } else if ((kind >= 1 && kind <= 3) || (kind >= 27 && kind <= 29)) {  // fetches, including ACK: four words each
            const uint32_t count = (((w1 >> 10) & 7) | ((w1 >> 19) & 1) << 3) + 1;
            if (w0 <= pc || w0 > words / 2 || 2 * count > words / 2 - w0) return false;
            if (w0 < first_clause) first_clause = w0;
            for (uint32_t k = 0; k < count; k++) {
                const uint32_t at = 2 * (w0 + 2 * k), f0 = word(p, at), f1 = word(p, at + 1);
                // TEX and ordinary VTX fetches share these GPR fields (Mesa r600_sq.h). Semantic/relative destinations need a different proof.
                if ((f0 & 0x1F) == 1 || (f0 & (1u << 23)) || (f1 & (1u << 7))) return false;
                if (((f0 >> 16) & 0x7F) >= shader_budget::kOrdinary || (f1 & 0x7F) >= shader_budget::kOrdinary) return false;
                reg((f0 >> 16) & 0x7F);
                reg(f1 & 0x7F);
                if (to && !(f0 & 0x1F) && ((f0 >> 8) & 0xFF) >= 0x80) set_word(p, at + 1, f1 & ~(1u << 21));  // a uniform block's buffer: 128 and up
            }
        } else if (kind >= 30) return false;  // uninspected memory/export encodings cannot certify a register budget
        if (w1 & (1u << 21)) return furthest_target <= pc;  // no branch may escape the prefix whose references were inspected
    }
    return false;
}
}  // namespace latte

// Inspect completely before changing words or metadata. Underdeclared ordinary counts are normalized; an ambiguous R123 read is refused.
inline bool program_registers(const void *program, uint32_t bytes, uint32_t declared, uint32_t &required, uint32_t *spare_base = nullptr) {
    if (!program || bytes < 8 || bytes % 8 || declared > shader_budget::kOrdinary) return false;
    uint32_t top, spares;
    bool reads123;
    const bool repair = declared < shader_budget::kOrdinary;
    if (!latte::walk((uint8_t *)program, bytes / 4, top, spares, nullptr, repair, &reads123) || (repair && reads123)) return false;
    if (!declared && (top || spares)) return false;  // zero is valid for a constant-only pixel program, never missing metadata for real GPRs
    const uint32_t base = declared > top ? declared : top;
    required = base + spares;
    if (required > shader_budget::kOrdinary) return false;
    if (spare_base) *spare_base = base;
    return true;
}

// False leaves both words and count unchanged, including programs needing no throwaway repair.
inline bool fit_program(void *program, uint32_t bytes, uint32_t &registers, uint32_t limit) {
    uint32_t required, spare, top, spares;
    if (!program_registers(program, bytes, registers, required, &spare) || required > limit) return false;
    latte::walk((uint8_t *)program, bytes / 4, top, spares, &spare, registers < shader_budget::kOrdinary);
    registers = required;
    return true;
}

// A vertex program's bursts of exports written out as single exports: for runs in Cemu alone. Cemu up to 2.6 hands the pixel shader only the
// first register of a burst (its own fix, "Latte: Support burst for vertex exports" of 2026-10-02, is in no release yet), and the console's
// compiler writes bursts where the PC's never does (none in the 11,800 vertex programs of fourteen packs). So in Cemu what a shader
// compiled at play time drew lost its texture coordinates or its colour: Pokemon Ultra Sun's tall grass was gone, its road and its shadows
// a layer short (2026-11-13). A console takes bursts as they are.
// Returns the program's length in words without bursts, or 0 if it has none, and writes it to `out` if given. Its list is longer by an
// instruction for each register past a burst's first, and the clauses lie that much further on, made even: a fetch clause has to begin at
// an even place. For a program fit_program has passed.
inline uint32_t unburst(const uint8_t *p, uint32_t words, uint8_t *out) {
    const auto burst_of = [&](uint32_t i) {  // how many registers past its first an instruction of the list exports
        const uint32_t w1 = latte::word(p, 2 * i + 1), kind = (w1 >> 23) & 0x7F;
        return !(w1 & (1u << 29)) && (kind == 39 || kind == 40) ? (w1 >> 17) & 15 : 0u;
    };
    uint32_t list = 0, extra = 0;
    for (bool last = false; !last; list++) {
        const uint32_t w1 = latte::word(p, 2 * list + 1);
        extra += burst_of(list);
        last = !(w1 & (1u << 29)) && (w1 & (1u << 21));  // END_OF_PROGRAM
    }
    if (!extra) return 0;
    const uint32_t shift = extra + (extra & 1);  // how much further on the clauses lie: an odd count leaves an empty place after the list
    if (!out) return words + 2 * shift;
    const auto lies_at = [&](uint32_t i) {  // where an instruction of the list comes to lie
        uint32_t at = i;
        for (uint32_t k = 0; k < i && k < list; k++) at += burst_of(k);
        return at;
    };
    uint32_t o = 0;
    const auto put = [&](uint32_t w0, uint32_t w1) {
        latte::set_word(out, 2 * o, w0);
        latte::set_word(out, 2 * o + 1, w1);
        o++;
    };
    for (uint32_t i = 0; i < list; i++) {
        const uint32_t w0 = latte::word(p, 2 * i), w1 = latte::word(p, 2 * i + 1), kind = (w1 >> 23) & 0x7F;
        if (w1 & (1u << 29)) put((w0 & ~0x3FFFFFu) | ((w0 & 0x3FFFFF) + shift), w1);  // an arithmetic clause
        else if (kind == 39 || kind == 40) {
            const uint32_t burst = burst_of(i);
            for (uint32_t k = 0; k <= burst; k++)  // the next place from the next register; all but the last a plain export that ends nothing
                put(w0 + k * (1u | 1u << 15), k < burst ? (w1 & ~(0xFu << 17 | 1u << 21 | 0x7Fu << 23)) | 39u << 23 : w1 & ~(0xFu << 17));
        } else if (kind >= 1 && kind <= 3) put(w0 + shift, w1);  // a fetch clause
        else if (kind >= 4 && kind <= 18) put(lies_at(w0), w1);  // loops, jumps and calls name an instruction of the list
        else put(w0, w1);
    }
    if (shift > extra) put(0, 0);
    memcpy(out + 8 * (list + shift), p + 8 * list, 4 * words - 8 * list);  // the clauses, as they are
    return words + 2 * shift;
}
}  // namespace gpu
