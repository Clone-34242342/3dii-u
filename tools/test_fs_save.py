"""Run the real FS/kernel/save-cache regression without game files or console storage."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools/win'))
import build3ds as b
out=Path(os.environ.get('THREEDII_SAVE_TEST_OUT',Path(b.WORK)/'selftest/fs_save'))
out.mkdir(parents=True,exist_ok=True)
fake=out/'fake_wut/coreinit';fake.mkdir(parents=True,exist_ok=True)
for name in ('dynload.h','filesystem.h','time.h'):(fake/name).touch()
exe=out/'test_fs_save.exe'
subprocess.run([*b.HOST_CXX,'-std=c++20','-O1','-w','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-static',
    f'-I{fake.parent}',f'-I{b.PORT}/wiiu',f'-I{b.PORT}/pc/win',f'-I{b.PORT}/pc',f'-I{b.RT}',
    '-include',b.PORT+'/pc/shim.h',str(ROOT/'tools/test_fs_save.cpp'),b.PORT+'/pc/win/win_compat.cpp','-lpthread','-lwinmm','-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='3dii_fs_save_') as directory:
    assert Path(directory).resolve().is_relative_to(Path(tempfile.gettempdir()).resolve())
    subprocess.run([str(exe),directory],check=True,timeout=30)
