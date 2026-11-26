"""Exercise generated callers and core.cpp's real dispatcher with an omitted ARM/Thumb callee.

The test-only lazy provider interprets the small synthetic callee in test_transition_contract.cpp.
It is a boundary probe, not an interpreter shipped in game builds. Run with Windows Python.
"""
import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp import gen, image


def main():
    with tempfile.TemporaryDirectory(prefix='3dii_transition_') as tmp:
        work = Path(tmp)
        code = bytearray(0x220)
        def arm(offset, words):
            struct.pack_into('<%dI' % len(words), code, offset, *words)
        # Caller keeps r4=49 live across the indirect call, then adds it to the result.
        arm(0, [0xE92D4010, 0xE3A04031, 0xE12FFF31, 0xE0800004, 0xE8BD8010])
        # Both omitted callees keep r4=7 live across an indirect call to the compiled helper.
        arm(0x100, [0xE92D4010, 0xE3A04007, 0xE12FFF32, 0xE0800004, 0xE8BD8010])
        struct.pack_into('<5H', code, 0x140, 0xB510, 0x2407, 0x4790, 0x1900, 0xBD10)
        # SVC spills the helper's working r4=99 and lowered sp into CPU; ABI return leaves them stale there.
        arm(0x200, [0xE92D4010, 0xE3A04063, 0xEF00007F, 0xE2800005, 0xE8BD8010])
        (work / 'code.bin').write_bytes(code)
        (work / 'info.json').write_text(json.dumps({
            'text': {'addr': 0x100000, 'size': len(code)},
            'rodata': {'addr': 0x101000, 'size': 0},
            'data': {'addr': 0x102000, 'size': 0}}))
        for mode in ('safe', 'abi', 'mixed'):
            for omitted in (False, True):
                out = work / (mode + ('_omitted' if omitted else '_compiled'))
                keys = [0x100000, 0x100200] if omitted else [0x100000, 0x100100, 0x100141, 0x100200]
                an = gen.Analyzer(image.Image(str(work)), log=lambda *args: None)
                an.funcs = dict.fromkeys(keys, True)
                em = gen.Emitter(an, str(out))
                em.abi = mode != 'safe'
                em.safe_ranges = [(0x100000 if mode == 'mixed' else 0x100100, 0x100180)] if em.abi else []
                with contextlib.redirect_stdout(io.StringIO()):
                    sources = em.run(keys)
                assert not em.undefined, em.undefined
                objects = []
                for source in sources:
                    obj = Path(source).with_suffix('.o')
                    subprocess.run(build3ds.host_game_flags(str(out)) + ['-c', source, '-o', str(obj)], check=True)
                    objects.append(str(obj))
                exe = out / 'transition.exe'
                flags = [*build3ds.HOST_CXX, '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                         '-Wl,--gc-sections', '-static', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc',
                         f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.RT}', '-include', f'{build3ds.PORT}/pc/shim.h',
                         f'-DOMIT_CALLEES={int(omitted)}', str(ROOT / 'tools/test_transition_contract.cpp'),
                         f'{build3ds.PORT}/pc/win/win_compat.cpp', *objects, '-lpthread', '-lwinmm', '-o', str(exe)]
                subprocess.run(flags, check=True)
                subprocess.run([str(exe)], cwd=out, check=True)
                print(f'transition contract: {out.name} ok', flush=True)


if __name__ == '__main__':
    main()
