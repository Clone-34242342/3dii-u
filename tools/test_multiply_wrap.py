"""Run actual ARM multiply lowering against integer bit-vector results, trapping host signed overflow.

ARM's multiply accumulators wrap (ARM ARM, SMLALD/SMMLA/SMMLS); C signed overflow must not stand for that.
Reference: https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
"""
from pathlib import Path
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp import arm
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def main():
    out = Path(build3ds.WORK) / 'selftest/multiply_wrap'
    out.mkdir(parents=True, exist_ok=True)
    source = ['#include "cpu.h"', '#include <assert.h>', '#include <stdio.h>']
    checks = []
    rng = random.Random(315)
    edges = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x80008000, 0x7fff7fff]
    count = 0
    for op1, op2 in [(0x74, k) for k in range(4)] + [(0x75, k) for k in (0, 1, 6, 7)]:
        for ra in ([0, 2] if op1 == 0x74 else [0, 15] if op2 < 6 else [0]):
            # Include destination/source aliases; the long result uses r1:r<ra>.
            word = 0xe0000010 | (op1 << 20) | (1 << 16) | (ra << 12) | (3 << 8) | (op2 << 5) | 2
            ins = arm.decode(0x100000, word, arm.Ctx())
            assert ins.note in ('smlald', 'smmul'), ins.note
            name = f'm_{op1:x}_{op2}_{ra}'
            source.append(f'__attribute__((noinline)) static void {name}(u32 *r) {{ ' +
                          ' '.join(f'u32 r{k}=r[{k}];' for k in range(4)) +
                          ' '.join(ins.code) + ' '.join(f'r[{k}]=r{k};' for k in range(4)) + ' }')
            vectors = [[a, b, c, d] for a, b, c, d in
                       [(0xffffffff, 0x7fffffff, 1, 1), (0, 0x80000000, 0xffff, 1),
                        (0xffffffff, 0xffffffff, 0x80008000, 0x80008000)]]
            vectors += [[rng.choice(edges) for _ in range(4)] for _ in range(24)]
            vectors += [[rng.getrandbits(32) for _ in range(4)] for _ in range(24)]
            for initial in vectors:
                result = initial.copy()
                if op1 == 0x74:
                    m = initial[3]
                    if op2 & 1:
                        m = ((m << 16) | (m >> 16)) & 0xffffffff
                    p = signed(initial[2], 16) * signed(m, 16)
                    q = signed(initial[2] >> 16, 16) * signed(m >> 16, 16)
                    acc = (initial[1] << 32) | initial[ra]
                    value = (acc + p + (-q if op2 & 2 else q)) & 0xffffffffffffffff
                    result[ra], result[1] = value & 0xffffffff, value >> 32
                else:
                    product = signed(initial[2], 32) * signed(initial[3], 32)
                    value = product if ra == 15 else (initial[ra] << 32) + (-product if op2 >= 6 else product)
                    result[1] = ((value + (0x80000000 if op2 & 1 else 0)) >> 32) & 0xffffffff
                checks.append('{ u32 r[4]={' + ','.join(f'0x{x:x}u' for x in initial) + f'}}; {name}(r); ' +
                              ' '.join(f'assert(r[{k}]==0x{x:x}u);' for k, x in enumerate(result)) + ' }')
                count += 1
    source.append('int main(void) { ' + '\n'.join(checks) + f' puts("multiply wrap: {count} cases passed"); }}')
    generated = out / 'multiply_wrap.c'
    generated.write_text('\n'.join(source))
    exe = out / 'multiply_wrap.exe'
    subprocess.run(build3ds.HOST_CXX + ['-x', 'c', '-std=gnu11', '-O2', '-ftrapv', '-static', f'-I{build3ds.RT}',
                                      str(generated), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
