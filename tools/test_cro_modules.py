"""Exercise the production CRO loader with synthetic static and NDCF-wrapped modules."""
import argparse
import importlib.util
from pathlib import Path
import struct
import tempfile
from unittest.mock import patch
import zlib

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'tools/win/build3ds.py').is_file())


def module(name):
    data = bytearray(0x180)
    data[0x80:0x84] = b'CRO0'
    struct.pack_into('<I', data, 0xC0, 0x140)
    text = name.encode('ascii') + b'\0'
    data[0x140:0x140 + len(text)] = text
    return bytes(data)


def ndcf(data, size=None, extra=b''):
    packed = zlib.compress(data) + extra
    return b'NDCF' + bytes(4) + struct.pack('<II', len(data) if size is None else size, len(packed)) + packed


def check(cro):
    controls = 0
    with tempfile.TemporaryDirectory(prefix='3dii-cro-') as directory:
        game = Path(directory)
        (game / 'romfs.bin').touch()

        def load(base, update=None):
            path = game / 'update_romfs.bin'
            if update is None:
                path.unlink(missing_ok=True)
            else:
                path.touch()

            def read(path, want):
                files = update if Path(path).name == 'update_romfs.bin' else base
                return {name: data for name, data in files.items() if want(name)}

            with patch.object(cro, 'romfs_files', side_effect=read):
                return cro.load(str(game))

        static = {'static.crs': module('static')}
        result = load(static)
        assert result is not None and result.crs.name == 'static' and not result.mods, 'static-only CRS must survive loading'
        controls += 1

        result = load(static | {'module.cro.p': ndcf(module('base'))})
        assert len(result.mods) == 1 and result.mods[0].path == 'module.cro' and bytes(result.mods[0].d) == module('base')
        controls += 1

        for base, update in (({'module.cro.p': ndcf(module('base'))}, {'module.cro': module('update')}),
                             ({'module.cro': module('base')}, {'module.cro.p': ndcf(module('update'))})):
            result = load(static | base, update)
            assert len(result.mods) == 1 and result.mods[0].path == 'module.cro' and bytes(result.mods[0].d) == module('update'), 'update must replace the normalized base module'
            controls += 1

        assert load({'module.cro.p': ndcf(module('base'))}) is None, 'a dynamic module without a CRS is not a loadable set'
        controls += 1

        for label, data in (
            ('short wrapper', b'NDCF' + bytes(8)),
            ('wrong wrapper magic', b'BAD!' + bytes(12)),
            ('truncated payload', b'NDCF' + bytes(4) + struct.pack('<II', 0x180, 99) + b'x'),
            ('size mismatch', ndcf(module('bad'), size=0x181)),
            ('wrong CRO magic', ndcf(bytes(0x180))),
            ('trailing compressed data', ndcf(module('bad'), extra=b'extra')),
        ):
            try:
                load(static | {'module.cro.p': data})
            except ValueError as error:
                assert 'module.cro.p' in str(error), label
            else:
                raise AssertionError(f'{label}: malformed module was accepted')
            controls += 1
    assert controls == 11
    print(f'CRO modules: {controls} production loader controls passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    path = parser.parse_args().source_root.resolve() / 'third_party/3dsrecomp/tools/recomp/cro.py'
    spec = importlib.util.spec_from_file_location('tested_cro', path)
    cro = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cro)
    check(cro)


if __name__ == '__main__':
    main()
