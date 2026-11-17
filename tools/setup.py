#!/usr/bin/env python3
"""Gets a Windows PC ready to build 3Dii U: python tools/setup.py [--check]

Checks every tool the build needs, installs what it can, and says how to get the rest. Run it again after installing
anything: it skips what's already there. --check only reports, and changes nothing.

Maintainers: --pack writes the dependencies zip from this PC's copies, for a new release (DEPS_TAG below)."""
import argparse
import hashlib
import importlib.util
import ntpath
import os
from pathlib import Path, PureWindowsPath
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'win'))
from objcache import _checked

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MSYS2 = 'C:/msys64'
LLVM = 'C:/Program Files/LLVM'
DEVKITPRO = 'C:/devkitPro/opt/devkitpro'
TOOLCHAIN = ['devkitppc-gcc', 'devkitppc-binutils', 'devkitppc-newlib', 'devkitppc-crtls', 'wut', 'wut-tools', 'general-tools']
PIP = {'PySide6': 'PySide6-Essentials', 'pyctr': 'pyctr', 'Crypto': 'pycryptodome', 'cryptography': 'cryptography'}

# Binaries the build needs that no package manager has. They come as one zip attached to a release of this repository,
# checked against this hash before anything is unpacked.
DEPS_TAG = 'deps-1'
DEPS_ZIP = '3dii-u-deps-1.zip'
DEPS_SHA256 = '9cad1a44cfd2322cbc1c269e6774adb136cf57c5b1eb2d0273a4fa1c0a9caa0e'
DEPS = ['third_party/cafeglsl/glslcompiler.exe', 'third_party/cafeglsl/libwinpthread-1.dll', 'third_party/cafeglsl/zlib1.dll',
        'third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc', 'toolchain/aac_ppc', 'builder/bin/makerom.exe']
NOTICE = 'third_party/DEPS-NOTICES.txt'
# These directories are not installed merely because an empty/partial folder exists.
REQUIRED = {
    DEPS[3]: ('lib/libcafeglsl.a', 'include/cafeglsl/CafeGLSLCompiler.h', 'include/cafeglsl/cafe_gx2.h', 'include/cafeglsl/gfd.h'),
    DEPS[4]: ('lib/libavcodec.a', 'lib/libavutil.a', 'include/libavcodec/avcodec.h', 'include/libavutil/avutil.h'),
}
NOTICES = """Build dependencies for 3Dii U, unpacked by tools/setup.py. Each is under its own licence.

third_party/cafeglsl/glslcompiler.exe: CafeGLSL v0.2.0 by Exzap (MIT), built for Windows by tools/win/build_cafeglsl.ps1.
  https://github.com/Exzap/CafeGLSL
third_party/cafeglsl/libwinpthread-1.dll and zlib1.dll: MinGW-w64 winpthreads (MIT and BSD-style) and zlib (zlib licence),
  from MSYS2. https://www.mingw-w64.org/ and https://zlib.net/
third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc: CafeGLSL v1.0.0-rc1's Wii U library (MIT, licence text inside), with its
  five trap instructions made no-ops by tools/win/patch_cafeglsl_traps.py.
toolchain/aac_ppc: FFmpeg 4.4's libavcodec and libavutil with only the AAC decoder (LGPL-2.1 or later), built by
  tools/aac_build.sh. Source: https://ffmpeg.org/download.html
builder/bin/makerom.exe: makerom v0.19.0 from Project_CTR by 3DSGuy (MIT). https://github.com/3DSGuy/Project_CTR
"""


def run(argv, **kw):
    print('  >', ' '.join(argv), flush=True)
    return subprocess.run(argv, **kw).returncode == 0


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def pip_missing():
    return [pkg for mod, pkg in PIP.items() if importlib.util.find_spec(mod) is None]


def deps_missing():
    missing = []
    for group in DEPS:
        try:
            for name in required_files(group):
                _, info = _checked(Path(ROOT) / name)
                if info is None or not info.st_size:
                    missing.append(group)
                    break
        except (OSError, ValueError):
            missing.append(group)
    return missing


def required_files(group):
    return [group + '/' + leaf for leaf in REQUIRED[group]] if group in REQUIRED else [group]


def install_archive(z):
    """Validate the whole archive, then publish only absent files in incomplete groups.

    Present tools are never replaced, including in a partial group. An empty required file
    needs manual repair; silently mixing a replacement library with existing headers is unsafe.
    """
    root, _ = _checked(ROOT, directory=True)
    missing = set(deps_missing())
    members, selected = {}, []
    for item in z.infolist():
        name = item.filename[:-1] if item.is_dir() else item.filename
        parts = name.split('/')
        if (item.orig_filename != item.filename or not name or '\\' in name or
                any(p in ('', '.', '..') or p.rstrip(' .') != p or
                    any(c in ':<>"|?*' or ord(c) < 32 for c in p) or
                    (ntpath.isreserved(p) if hasattr(ntpath, 'isreserved') else PureWindowsPath(p).is_reserved())
                    for p in parts)):
            raise ValueError(f'Unsafe dependency archive path: {item.filename!r}')
        group = next((g for g in DEPS if name == g or (g in REQUIRED and name.startswith(g + '/'))), None)
        if group is None and name != NOTICE:
            raise ValueError(f'Undeclared dependency archive path: {name}')
        mode = stat.S_IFMT(item.external_attr >> 16)
        if (mode not in (0, stat.S_IFDIR if item.is_dir() else stat.S_IFREG) or
                item.external_attr & 0x400 or (item.is_dir() and group not in REQUIRED)):
            raise ValueError(f'Dependency archive member is not an ordinary file/folder: {name}')
        key = name.casefold()
        if key in members:
            raise ValueError(f'Duplicate dependency archive path: {name}')
        destination, info = _checked(root / name, directory=item.is_dir())
        if not destination.is_relative_to(root):
            raise ValueError(f'Dependency archive path escapes the workspace: {name}')
        members[key] = item
        if not item.is_dir() and info is None and (group in missing or name == NOTICE):
            selected.append((item, destination))
    for key, item in members.items():
        for parent in Path(key).parents:
            ancestor = members.get(parent.as_posix())
            if ancestor is not None and not ancestor.is_dir():
                raise ValueError(f'Dependency archive file is also a parent: {item.filename}')
    for group in missing:
        for name in required_files(group):
            _, info = _checked(root / name)
            item = members.get(name.casefold())
            if info is not None:
                if not info.st_size:
                    raise ValueError(f'Existing dependency is empty; repair it before setup: {name}')
            elif item is None or item.is_dir() or not item.file_size:
                raise ValueError(f'Dependency archive is incomplete: {name}')
    # Reading all selected files before publication checks CRC/truncation too. The staging
    # directory is on the destination volume so publication cannot become a cross-volume copy.
    with tempfile.TemporaryDirectory(prefix='.3dii-deps-', dir=root) as temporary:
        staged = []
        for index, (item, destination) in enumerate(selected):
            source = Path(temporary) / str(index)
            with z.open(item) as reader, source.open('xb') as writer:
                shutil.copyfileobj(reader, writer)
            staged.append((source, destination))
        for source, destination in staged:
            _checked(destination.parent, directory=True)
            destination.parent.mkdir(parents=True, exist_ok=True)
            _, info = _checked(destination)
            if info is not None:
                raise ValueError(f'Dependency appeared during installation: {destination}')
            if os.name == 'nt':
                os.rename(source, destination)  # Windows rename refuses an existing target.
            else:
                os.link(source, destination)  # Same no-overwrite guarantee in host tests.
                source.unlink()


def install_mingw():
    return run([MSYS2 + '/usr/bin/bash.exe', '-lc', 'pacman -S --needed --noconfirm mingw-w64-x86_64-gcc'])


def install_toolchain(packages=TOOLCHAIN):
    return run([MSYS2 + '/usr/bin/bash.exe', '--noprofile', '--norc', 'tools/win/install_toolchain.sh'] + list(packages), cwd=ROOT)


def fetch_deps():
    if not shutil.which('gh') or subprocess.run(['gh', 'auth', 'status'], capture_output=True).returncode:
        print('  needs the GitHub CLI, signed in: winget install GitHub.cli, then gh auth login')
        return False
    with tempfile.TemporaryDirectory() as tmp:
        if not run(['gh', 'release', 'download', DEPS_TAG, '--pattern', DEPS_ZIP, '--dir', tmp], cwd=ROOT):
            return False
        path = os.path.join(tmp, DEPS_ZIP)
        if sha256(path) != DEPS_SHA256:
            print(f'  {DEPS_ZIP} is not the file this script expects (SHA-256 {sha256(path)}), so nothing was unpacked')
            return False
        try:
            with zipfile.ZipFile(path) as z:
                install_archive(z)
        except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
            print(f'  Dependencies were not fully installed: {error}')
            return False
    return True


def wut_current():
    """wut's headers as the port uses them: a thread's entry point takes (int, void *), as in wut 1.9.2. An older wut, left by a devkitPro
    install this skips, declares (int, const char **), and the port's thread code doesn't compile (a tester's Tomodachi build, Nov 9)."""
    try:
        with open(DEVKITPRO + '/wut/include/coreinit/thread.h', encoding='utf-8', errors='replace') as f:
            return 'OSThreadEntryPointFn)(int argc, void *argv)' in f.read()
    except OSError:
        return False


def update_wut():
    """only an install this script made (its own pacman under C:\\devkitPro\\pacman); devkitPro's own installer has its own pacman"""
    return os.path.exists('C:/devkitPro/pacman/pacman.conf') and install_toolchain(['wut', 'wut-tools'])


CHECKS = [  # name, test, what installs it (or None), what to do by hand
    ('Python packages', lambda: not pip_missing(), lambda: run([sys.executable, '-m', 'pip', 'install', *pip_missing()]),
     'python -m pip install ' + ' '.join(PIP.values())),
    ('MSYS2', lambda: os.path.exists(MSYS2 + '/usr/bin/bash.exe'), None, 'install it from https://www.msys2.org/ into C:\\msys64'),
    ('MinGW-w64 GCC', lambda: os.path.exists(MSYS2 + '/mingw64/bin/gcc.exe'), install_mingw,
     'in an MSYS2 shell: pacman -S mingw-w64-x86_64-gcc'),
    ('LLVM', lambda: os.path.exists(LLVM + '/bin/clang.exe'), None, 'winget install LLVM.LLVM'),
    ('devkitPPC and wut', lambda: os.path.exists(DEVKITPRO + '/devkitPPC/bin/powerpc-eabi-gcc.exe'), install_toolchain,
     'needs MSYS2 first; see CONTRIBUTING.md'),
    ('wut 1.9.2 or newer', wut_current, update_wut,
     'this wut is older than the port needs: in devkitPro\'s MSYS2 shell, run  pacman -Syu wut wut-tools'),
    ('CafeGLSL, the AAC decoder and makerom', lambda: not deps_missing(), fetch_deps,
     'sign in to gh, then rerun python tools/setup.py (existing tools are preserved)'),
]


def pack():
    if deps_missing():
        sys.exit('not on this PC: ' + ', '.join(deps_missing()))
    out = os.path.join(ROOT, DEPS_ZIP)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(NOTICE, NOTICES)
        for p in DEPS:
            full = os.path.join(ROOT, p)
            files = [full] if os.path.isfile(full) else [os.path.join(r, f) for r, _, fs in os.walk(full) for f in fs]
            for f in sorted(files):
                z.write(f, os.path.relpath(f, ROOT).replace('\\', '/'))
    print(f'{out}\nSHA-256 {sha256(out)}\nPut that hash in DEPS_SHA256, then upload the zip:\n'
          f'  gh release create {DEPS_TAG} "{out}" --title "Build dependencies" --notes "Unpacked by tools/setup.py"')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true', help='only report, change nothing')
    ap.add_argument('--pack', action='store_true', help='maintainers: write the dependencies zip from this PC')
    a = ap.parse_args()
    if os.name != 'nt':
        sys.exit('The build runs on Windows only for now.')
    if sys.version_info < (3, 10):
        sys.exit('Python 3.10 or newer is needed: https://www.python.org/')
    if a.pack:
        return pack()
    todo = []
    for name, ok, install, by_hand in CHECKS:
        if not ok() and install and not a.check:
            print(f'{name}: installing', flush=True)
            install()
        good = ok()
        print(f'{"ok     " if good else "MISSING"}  {name}', flush=True)
        if not good:
            todo.append(f'  {name}: {by_hand}')
    if todo:
        print('\nStill to do, then run this again:\n' + '\n'.join(todo))
        return 1
    print('\nReady. Try python builder/selftest.py, then python builder/pipeline.py --help')
    return 0


if __name__ == '__main__':
    sys.exit(main())
