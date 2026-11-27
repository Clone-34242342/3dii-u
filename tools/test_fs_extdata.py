"""Synthetic IPC requests against the real extra-data filesystem service, with no game files."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'tools/win'))
import build3ds as b

exe = Path(b.WORK)/'selftest/test_fs_extdata.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([*b.HOST_CXX, '-std=c++20', '-O1', '-w', '-ffunction-sections', '-fdata-sections',
    '-Wl,--gc-sections', '-static', f'-I{b.PORT}/pc/win', f'-I{b.PORT}/pc', f'-I{b.RT}',
    '-include', b.PORT+'/pc/shim.h', str(ROOT/'tools/test_fs_extdata.cpp'),
    b.PORT+'/pc/win/win_compat.cpp', '-lpthread', '-lwinmm', '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='3dii_extdata_') as directory:
    assert Path(directory).resolve().is_relative_to(Path(tempfile.gettempdir()).resolve())
    subprocess.run([str(exe), directory], check=True)
