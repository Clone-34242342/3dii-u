#!/usr/bin/env python3
"""Drive the existing builder with native Linux host/PPC tools and optional Wine GLSLC."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]


def load_builder(work, compiler=None):
    original_path = os.environ.get("PATH", "")
    sys.path.insert(0, str(ROOT / "tools/win"))
    import build3ds
    os.environ["PATH"] = original_path
    build3ds.WORK = str(Path(work).resolve())
    build3ds.HOST_CXX = shlex.split(os.environ.get("CXX", "g++"))
    build3ds.HOST_CXX[0] = shutil.which(build3ds.HOST_CXX[0]) or build3ds.HOST_CXX[0]
    def free_mb():
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"): return int(line.split()[1]) // 1024
        raise RuntimeError("Cannot determine available RAM")
    build3ds.free_mb = free_mb
    def native_run(argv, **kwargs):
        executable = Path(argv[0])
        pe = executable.is_file() and executable.open("rb").read(2) == b"MZ"
        if pe:
            argv = ["wine", str(executable)] + ["Z:" + a.replace("/", "\\") if isinstance(a, str) and a.startswith("/") else a for a in argv[1:]]
            kwargs["env"] = dict(kwargs.get("env", os.environ), WINEDEBUG="-all")
        return subprocess.run(argv, **kwargs)
    build3ds.run = native_run
    if compiler: build3ds.GLSLC = str(Path(compiler).resolve())
    return build3ds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=["code", "shaders", "selftest"])
    ap.add_argument("--game-dir", type=Path)
    ap.add_argument("--name")
    ap.add_argument("--output", type=Path)
    ap.add_argument("--work", type=Path, default=ROOT / "build_native")
    ap.add_argument("--devkitpro", type=Path, default=Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")))
    ap.add_argument("--wut", type=Path)
    ap.add_argument("--cafeglsl-ppc", type=Path)
    ap.add_argument("--compiler", type=Path)
    ap.add_argument("--sdk-alias", type=Path)
    ap.add_argument("--tools-dir", type=Path)
    ap.add_argument("--jobs", type=int, default=min(3, os.cpu_count() or 1))
    ap.add_argument("--resumable", action="store_true", help="alias for --execution-mode resumable")
    ap.add_argument("--execution-mode", choices=("auto", "classic", "resumable"), default="auto",
                    help="classic disables checkpoint execution for controlled comparisons; keep separate work/output folders")
    ap.add_argument("--opt", choices=("-O0", "-O1", "-O2", "-Os"), help="explicit game-code optimization for matched benchmarks")
    a = ap.parse_args()
    if a.resumable and a.execution_mode == "classic": ap.error("--resumable conflicts with classic execution")
    if a.resumable: a.execution_mode = "resumable"
    a.resumable = a.execution_mode == "resumable"
    if a.jobs < 1: ap.error("--jobs must be positive")
    if a.resumable and a.command != 'code': ap.error('--resumable applies only to code')
    if a.resumable and a.work == ROOT / 'build_native':
        a.work = ROOT / 'build_native_resumable'
    b = load_builder(a.work, a.compiler)
    if a.command == "selftest":
        sys.path.insert(0, str(ROOT / "tools"))
        import selftest
        selftest.main()
        return
    if not a.game_dir or not a.output: ap.error("--game-dir and --output are required")
    if a.command == "code":
        sdk = a.devkitpro.resolve(); wut = (a.wut or sdk / "wut").resolve()
        targets = {"devkitPPC": sdk / "devkitPPC", "tools": sdk / "tools", "wut": wut}
        digest = hashlib.sha256(str(targets).encode()).hexdigest()[:12]
        alias = a.sdk_alias or Path(tempfile.gettempdir()) / ("3dii-native-sdk-" + digest)
        alias.mkdir(parents=True, exist_ok=True)
        for name, target in targets.items():
            if not target.is_dir(): ap.error("Missing SDK directory: " + str(target))
            link = alias / name
            if link.exists():
                if link.resolve() != target: ap.error("SDK alias already points elsewhere: " + str(link))
            else: link.symlink_to(target, target_is_directory=True)
        tools = a.tools_dir or a.work / "native-tools"; tools.mkdir(parents=True, exist_ok=True)
        sources = list((sdk / "devkitPPC/bin").glob("powerpc-eabi-*")) + [sdk / "tools/bin/elf2rpl", sdk / "tools/bin/wuhbtool"]
        for source in sources:
            if not source.is_file(): continue
            link = tools / (source.name + ".exe")
            if not link.exists(): link.symlink_to(source)
        b.DKP = str(alias); b.WUT = str(alias / "wut"); b.BIN = str(tools.resolve() / "powerpc-eabi-")
        if a.cafeglsl_ppc: b.CAFEGLSL_PPC = str(a.cafeglsl_ppc.resolve())
        os.environ.update(DEVKITPRO=str(alias), DEVKITPPC=str(alias / "devkitPPC"), PATH=str(tools.resolve()) + os.pathsep + str(alias / "devkitPPC/bin") + os.pathsep + str(alias / "tools/bin") + os.pathsep + os.environ["PATH"])
    previous = os.environ.get("THREEDII_SESSION_STATES")
    if a.execution_mode != "auto":
        os.environ["THREEDII_SESSION_STATES"] = "0" if a.execution_mode == "classic" else "1"
    try:
        game = b.Game(a.name or a.game_dir.name, str(a.game_dir.resolve()))
    finally:
        if previous is None: os.environ.pop("THREEDII_SESSION_STATES", None)
        else: os.environ["THREEDII_SESSION_STATES"] = previous
    if a.resumable:
        game.gen, game.mode, game.opt, game.safe_ranges = 'gen_resumable', 'resumable', ['-O1'], []
        game.gen_dir = game.dir + '/' + game.gen
    game.out = str(a.output.resolve()); game.rpx = game.out + "/" + game.name + ".rpx"; game.nm = game.out + "/game.nm"; game.shaders = game.out + "/shaders"
    args = SimpleNamespace(jobs=a.jobs, force=False, extra_flags='-DRECOMP_RESUMABLE' if a.resumable else None, only=None, opt=a.opt, link_flags=None)
    if a.command == "code":
        b.build_code(game, args)
        manifest = {"game": game.name, "execution_mode": game.mode,
                    "checkpoint_runtime": game.mode == "resumable",
                    "optimization": shlex.split(a.opt) if a.opt else game.opt or b.GAME_OPT,
                    "code_sha256": hashlib.sha256((a.game_dir / "code.bin").read_bytes()).hexdigest(),
                    "rpx_sha256": hashlib.sha256(Path(game.rpx).read_bytes()).hexdigest()}
        (a.output / "execution-build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    else: b.build_shaders(game, args)


if __name__ == "__main__": main()
