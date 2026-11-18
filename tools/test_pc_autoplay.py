"""Exercise the actual native frontend autoplay expression with a fresh process per setting."""
from pathlib import Path
import os
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds


def main():
    source = (ROOT / 'port/pc/frontend_pc.cpp').read_text()
    expression = re.search(r'static const bool autoplay = (.*);', source).group(1)
    out = Path(build3ds.WORK) / 'selftest/pc_autoplay'
    out.mkdir(parents=True, exist_ok=True)
    cpp, exe = out / 'autoplay.cpp', out / 'autoplay.exe'
    cpp.write_text('#include <cstdlib>\nint main() { return (' + expression + ') ? 1 : 0; }\n')
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20','-O1','-static',str(cpp),'-o',str(exe)], check=True)
    for value, expected in [(None,0),('0',0),('1',1),('2',1),('',0)]:
        env = os.environ.copy()
        env.pop('RECOMP_AUTOPLAY',None)
        if value is not None:
            env['RECOMP_AUTOPLAY'] = value
        assert subprocess.run([str(exe)],env=env,timeout=10).returncode == expected, value
    print('Native autoplay: unset, zero, nonzero and empty settings passed')


if __name__ == '__main__':
    main()
