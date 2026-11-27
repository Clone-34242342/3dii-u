"""Compile the actual TEV evaluator/normalizer and check pixel and Python key parity.

Run: python tools/test_tev_prefix.py [--source-root FULL_ROOT_OR_PRIVATE_OVERLAY]
Missing overlay files use this checkout. No game inputs or optional decompression dependency.
"""
import argparse
import ast
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    selected = parser.parse_args().source_root.resolve()

    def source(name):
        candidate = selected / name
        return candidate if candidate.is_file() else ROOT / name

    # As in test_fs_normal.py: exercise production conversion functions without importing zstd.
    tools = []
    for name in ('tev_to_log.py', 'azahar_to_log.py'):
        ns = types.SimpleNamespace(struct=struct)
        tree = ast.parse(source('tools/' + name).read_text())
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef)], type_ignores=[]), name, 'exec'), vars(ns))
        tools.append(ns)
    tev, azahar = tools
    with tempfile.TemporaryDirectory(prefix='3dii-tev-prefix-') as temp:
        work = Path(temp)
        config_keys = []
        for kind in range(6):
            for op in (0, 1):
                config = bytearray(272)
                struct.pack_into('<I', config, 0, 1 | 1 << 5)
                for stage in range(6):
                    struct.pack_into('<4I', config, 48 + 16 * stage, 0x00ff0fff, 0, 0, 0)
                struct.pack_into('<4I', config, 48, 0x00030003, 0, op | op << 16, 0)
                struct.pack_into('<4I', config, 96, 0x00030003 if kind < 3 else 0x000f000f, 2 if kind in (2, 5) else 0, 0, 0)
                if kind in (1, 4):
                    struct.pack_into('<I', config, 32, 1 << 4)
                key = azahar.key_from_config(config)
                assert key is not None
                config_keys.append(key)
        (work / 'configs.bin').write_bytes(b''.join(struct.pack('<I', len(k)) + k for k in config_keys))
        flags = build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-static']
        for directory in ('port/wiiu', 'port/pc/win', 'port/pc', 'third_party/3dsrecomp/runtime', 'third_party/3dsrecomp/runtime/gpu'):
            flags += ['-I' + str(selected / directory), '-I' + str(ROOT / directory)]
        exe = work / 'test_tev_prefix.exe'
        subprocess.run(flags + ['-include', str(source('port/pc/shim.h')), str(ROOT / 'tools/test_tev_prefix.cpp'),
                               str(source('port/wiiu/shaderkeys.cpp')), str(source('port/wiiu/shadergen.cpp')),
                               str(source('third_party/3dsrecomp/runtime/gpu/fragment.cpp')),
                               str(source('third_party/3dsrecomp/runtime/gpu/shader.cpp')),
                               str(source('tools/win/shadergen_stubs.cpp')), '-lpthread', '-o', str(exe)], check=True)
        pairs = work / 'pairs.bin'
        subprocess.run([str(exe), str(pairs), str(work / 'configs.bin')], check=True)
        data, pos, cases = pairs.read_bytes(), 0, 0
        normals = []
        while pos < len(data):
            pair = []
            for _ in range(2):
                size, = struct.unpack_from('<I', data, pos)
                pos += 4
                pair.append(data[pos:pos + size])
                pos += size
            raw, normal = pair
            assert tev.encode(tev.normal(tev.decode(raw))) == normal, f'TEV mirror mismatch: {cases}'
            assert azahar.normalize_logged(raw) == normal, f'Azahar mirror mismatch: {cases}'
            normals.append(normal)
            cases += 1
        assert pos == len(data) and cases == 1054
        assert normals[-len(config_keys):] == config_keys, 'generated Azahar configs match actual C++ keys'
        for kind in range(6):
            assert (config_keys[2 * kind] == config_keys[2 * kind + 1]) == (kind in (0, 2)), 'only overwritten arithmetic aliases generated configs'
        print(f'TEV prefix Python parity: {cases} production keys, {len(config_keys)} generated configs')


if __name__ == '__main__':
    main()
