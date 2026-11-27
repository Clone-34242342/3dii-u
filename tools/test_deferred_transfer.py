"""Compile the actual deferred-transfer state and CPU-read function from the GX2 backend."""
import argparse
from pathlib import Path
import subprocess
import sys
from test_vertex_cache import function

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds
    out = args.out or Path(build3ds.WORK) / 'selftest/deferred_transfer'
    out.mkdir(parents=True, exist_ok=True)
    source = (args.backend or root / 'port/wiiu/gx2_backend.cpp').read_text()
    records = source[source.index('struct LastTransfer {'):source.index('namespace {', source.index('struct LastTransfer {'))]
    (out / 'deferred_transfer_production.inc').write_text(records + '\n' + function(source, 'void gx2_cpu_will_read('))
    executable = out / 'test_deferred_transfer.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-Wall', '-Wextra', '-static',
        '-I' + str(root / 'port/wiiu'), '-I' + str(out), str(Path(__file__).with_suffix('.cpp')),
        '-lpthread', '-o', str(executable)], check=True)
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
