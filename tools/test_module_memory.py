"""Run production loader/boot-loan/core/RO memory-policy regression without games or Cemu."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=next(p for p in Path(__file__).resolve().parents if(p/'tools/win/build3ds.py').is_file())
sys.path.insert(0,str(ROOT/'tools/win'))
import build3ds as b

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source-root',type=Path,default=ROOT)
    selected=parser.parse_args().source_root.resolve()
    def source(name):
        p=selected/name;return p if p.is_file()else ROOT/name
    with tempfile.TemporaryDirectory(prefix='3dii-module-memory-')as directory:
        work=Path(directory);core=source('third_party/3dsrecomp/runtime/core.cpp').read_text()
        start=core.index('static MEMHeapHandle lent_heap;');end=core.index('void *host_alloc(',start)
        (work/'loan_impl.inc').write_text('namespace mem {\n'+core[start:end]+'\n}\n')
        kernel=source('third_party/3dsrecomp/runtime/kernel.cpp').read_text()
        start=kernel.index('const u32 clear =',kernel.index('void start('));end=kernel.index(';',kernel.index('mem::lend(',start))+1
        (work/'boot_loan.inc').write_text('namespace kernel { static void production_boot_loan() {\n'+kernel[start:end]+'\n}}\n')
        flags=b.HOST_CXX+['-std=c++20','-O1','-w','-D__WIIU__','-DRECOMP_MODULES','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-static','-I'+str(work)]
        for folder in('third_party/3dsrecomp/runtime','port/wiiu','port/pc/win','port/pc'):
            flags+=['-I'+str(selected/folder),'-I'+str(ROOT/folder)]
        ro=source('third_party/3dsrecomp/runtime/services/ro.cpp')
        exe=work/'test_module_memory.exe'
        subprocess.run(flags+['-include',str(ROOT/'port/pc/shim.h'),'-DMODULE_MEMORY_RO_SOURCE="'+ro.as_posix()+'"',str(Path(__file__).with_suffix('.cpp')),'-lpthread','-o',str(exe)],check=True)
        subprocess.run([str(exe),str(work/'game')],check=True)

if __name__=='__main__':main()
