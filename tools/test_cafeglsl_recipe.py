"""Run the actual build recipe in Windows PowerShell 5 with tiny native-command fixtures.

git apply is real; only clone/version lookup and the expensive build are replaced.
Every source, venv and output is inside the temporary test directory.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RECIPE = ROOT / 'tools/win/build_cafeglsl.ps1'
PATCHES = [ROOT / 'tools/win' / name for name in
           ('cafeglsl_windows.patch', 'cafeglsl_deterministic.patch')]
REVISION = 'e3e385c700e6e70dd2d9d6cd015162991084df6b'
POWERSHELL = shutil.which('powershell.exe')
GIT = shutil.which('git.exe')
NAMES = ('glslcompiler.exe', 'libwinpthread-1.dll', 'zlib1.dll')


def ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def preimages(folder):
    """Reconstruct only the old/context lines needed by the real shipped patches."""
    files = {}
    for patch in PATCHES:
        path = None
        old_line = None
        for line in patch.read_text().splitlines():
            if line.startswith('--- a/'):
                path = line[6:]
                files.setdefault(path, [])
                old_line = None
            elif line.startswith('@@ '):
                old_line = int(re.match(r'@@ -(\d+)', line)[1]) - 1
            elif path is not None and old_line is not None and line[:1] in (' ', '-'):
                lines = files[path]
                while len(lines) <= old_line:
                    lines.append('')
                if lines[old_line] and lines[old_line] != line[1:]:
                    raise AssertionError(f'conflicting patch fixture: {path}:{old_line}')
                lines[old_line] = line[1:]
                old_line += 1
    for name, lines in files.items():
        target = folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('\n'.join(lines) + '\n', newline='\n')
    (folder / 'meson.build').write_text('# synthetic fixture, not compiled\n')


@unittest.skipUnless(os.name == 'nt' and POWERSHELL and GIT, 'requires Windows PowerShell 5 and git')
class RecipeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cafeglsl_recipe_')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.fixture = self.base / 'fixture'
        self.fixture.mkdir()
        preimages(self.fixture)
        self.git('init', '--quiet', self.fixture)
        self.git('-C', self.fixture, 'add', '.')
        self.git('-C', self.fixture, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '--quiet', '-m', 'Synthetic patch preimages')
        self.source = self.base / 'source'
        self.venv = self.base / 'venv'
        (self.venv / 'Scripts').mkdir(parents=True)
        (self.venv / 'Scripts/meson.exe').touch()
        (self.venv / 'Scripts/python.exe').touch()
        self.msys = self.base / 'msys'
        (self.msys / 'mingw64/bin').mkdir(parents=True)
        for name in NAMES[1:]:
            (self.msys / 'mingw64/bin' / name).write_bytes(('new ' + name).encode())
        self.output = self.base / 'output'
        self.output.mkdir()
        for name in NAMES:
            target = self.output / name
            target.write_bytes(('old ' + name).encode())
            os.utime(target, ns=(1600000000000000000, 1600000000000000000))
        self.before = self.bundle()
        self.log = self.base / 'calls.jsonl'

    def git(self, *args):
        return subprocess.run([GIT, '-c', 'core.autocrlf=false', *map(str, args)], check=True, capture_output=True)

    def existing(self, patches=0):
        self.git('clone', '--quiet', self.fixture, self.source)
        for patch in PATCHES[:patches]:
            self.git('-C', self.source, 'apply', patch)

    def bundle(self):
        return {n: self.snapshot(self.output / n) for n in NAMES}

    def snapshot(self, path):
        info = path.stat()
        return path.read_bytes(), info.st_mtime_ns, info.st_nlink

    def ticket(self):
        target = self.base / 'NaNd/rights/ticket/fake.tik'
        target.parent.mkdir(parents=True)
        target.write_bytes(b'synthetic ticket: preserve bytes, time and link count')
        os.utime(target, ns=(1600000000000000000, 1600000000000000000))
        return target

    def junction(self, alias, target):
        result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(target)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.addCleanup(lambda: alias.rmdir() if os.path.lexists(alias) else None)

    def run_recipe(self, *, clone_error=False, revision=REVISION, meson_error=False,
                   ninja_error=False, no_compiler=False, apply_error=False, relative_paths=False, roots=None,
                   late_alias=False):
        self.log.unlink(missing_ok=True)
        harness = self.base / 'run.ps1'
        paths = list(roots or [self.source, self.venv, self.output]) + [self.msys]
        if relative_paths:
            paths = [p.relative_to(self.base) for p in paths]
        # Native wrappers preserve LASTEXITCODE, including the real git apply failures under PS5.
        harness.write_text(f"""
$ErrorActionPreference = 'Stop'
$global:RecipeLog = {ps(self.log)}
function Record($kind, $value) {{
  @{{kind=$kind; value=$value}} | ConvertTo-Json -Compress -Depth 5 | Add-Content -LiteralPath $global:RecipeLog
}}
function global:git {{
  Record 'git' @($args)
  if ($args -contains 'clone') {{
    if ({ps(clone_error)} -eq 'True') {{ $global:LASTEXITCODE=21; return }}
    & {ps(GIT)} -c core.autocrlf=false clone --quiet {ps(self.fixture)} {ps(self.source)}
  }} elseif ($args -contains 'rev-parse') {{
    {ps(revision)}
    $global:LASTEXITCODE=0
    return
  }} elseif (($args -contains 'apply') -and -not ($args -contains '--check') -and ({ps(apply_error)} -eq 'True')) {{
    $global:LASTEXITCODE=22; return
  }} else {{ & {ps(GIT)} @args }}
  $global:LASTEXITCODE=$LASTEXITCODE
}}
function global:python {{
  if ($args -contains '-c') {{
    Record 'validation' @($args)
    & {ps(sys.executable)} @args
    $global:LASTEXITCODE=$LASTEXITCODE
  }} else {{ Record 'python' @($args); $global:LASTEXITCODE=23 }}
}}
function global:meson {{
  Record 'meson' @($args)
  New-Item -ItemType Directory -Force {ps(self.source / 'build-win')} | Out-Null
  New-Item -ItemType File -Force {ps(self.source / 'build-win/build.ninja')} | Out-Null
  $global:LASTEXITCODE=0
  if ({ps(meson_error)} -eq 'True') {{ $global:LASTEXITCODE=24 }}
}}
function global:Start-Process {{
  param($FilePath, $WorkingDirectory, $ArgumentList, $WindowStyle, [switch]$PassThru,
        $RedirectStandardOutput, $RedirectStandardError)
  Record 'ninja' @($FilePath, $WorkingDirectory, $ArgumentList, $WindowStyle)
  if (({ps(ninja_error)} -ne 'True') -and ({ps(no_compiler)} -ne 'True')) {{
    New-Item -ItemType Directory -Force {ps(self.source / 'build-win/cafecompiler')} | Out-Null
    [IO.File]::WriteAllText({ps(self.source / 'build-win/cafecompiler/glslcompiler.elf.exe')}, 'new compiler')
  }}
  if ({ps(late_alias)} -eq 'True') {{
    Remove-Item -LiteralPath {ps(self.output / NAMES[0])}
    New-Item -ItemType HardLink -Path {ps(self.output / NAMES[0])} -Target {ps(self.base / 'NaNd/rights/ticket/fake.tik')} | Out-Null
  }}
  $process = [pscustomobject]@{{Handle=1; HasExited=$false; PriorityClass='Normal'; ExitCode=0}}
  if ({ps(ninja_error)} -eq 'True') {{ $process.ExitCode=25 }}
  $process | Add-Member ScriptMethod WaitForExit {{ Record 'priority' $this.PriorityClass; $this.HasExited=$true }}
  $process | Add-Member ScriptMethod Dispose {{}}
  return $process
}}
try {{
  Set-Location -LiteralPath {ps(self.base)}
  & {ps(RECIPE)} -Jobs 2 -Source {ps(paths[0])} -Venv {ps(paths[1])} -Output {ps(paths[2])} -Msys {ps(paths[3])}
  exit 0
}} catch {{ Write-Output $_; exit 1 }}
""", encoding='utf-8-sig')
        return subprocess.run([POWERSHELL, '-NoLogo', '-NoProfile', '-NonInteractive', '-File', str(harness)],
                              cwd=ROOT, capture_output=True, text=True, errors='replace')

    def calls(self, kind):
        return [r['value'] for r in map(json.loads, self.log.read_text(encoding='utf-8-sig').splitlines())
                if r['kind'] == kind]

    def success(self, **args):
        result = self.run_recipe(**args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.output / NAMES[0]).read_bytes(), b'new compiler')
        for patch in PATCHES:
            self.git('-C', self.source, 'apply', '--reverse', '--check', patch)
        self.assertEqual(self.calls('priority'), ['BelowNormal'])
        ninja = self.calls('ninja')[0]
        self.assertIn('ninja.exe', ninja[0])
        self.assertEqual(ninja[2][2:4], ['-j', '2'])
        self.assertEqual(ninja[3], 'Hidden')
        self.assertFalse(list(self.output.glob('.cafeglsl-*')))

    def failure(self, **args):
        result = self.run_recipe(**args)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.bundle(), self.before)
        self.assertFalse(list(self.output.glob('.cafeglsl-*')))
        return result

    def rejected_before_build(self, **args):
        self.failure(**args)
        for kind in ('git', 'python', 'meson', 'ninja'):
            self.assertFalse(self.calls(kind), kind)

    def test_fresh_clone_applies_both(self):
        self.success()
        self.assertEqual(len(self.calls('meson')), 1)
        self.assertEqual(sum('clone' in c for c in self.calls('git')), 1)

    def test_existing_meson_tree_applies_both(self):
        self.existing()
        self.success()
        self.assertFalse(any('clone' in c for c in self.calls('git')))

    def test_relative_paths_resolve_before_changing_directory(self):
        self.existing()
        self.success(relative_paths=True)

    def test_existing_windows_patch_gets_deterministic_patch(self):
        self.existing(1)
        self.success()

    def test_already_applied_is_idempotent(self):
        self.existing(2)
        before = {str(p): p.read_bytes() for p in self.source.rglob('*') if p.is_file() and '.git' not in p.parts}
        self.success()
        self.success()
        for path, data in before.items():
            self.assertEqual(Path(path).read_bytes(), data)
        self.assertFalse(self.calls('meson'))

    def test_rejected_patch_stops_before_build(self):
        self.existing(1)
        target = self.source / 'src/gallium/drivers/r600/sb/sb_ir.h'
        target.write_text('conflicting source\n')
        self.failure()
        self.assertFalse(self.calls('meson'))
        self.assertFalse(self.calls('ninja'))

    def test_native_apply_failure_stops_before_build(self):
        self.existing()
        self.failure(apply_error=True)
        self.assertFalse(self.calls('ninja'))

    def test_wrong_source_version_stops_before_patch(self):
        self.existing()
        self.failure(revision='0' * 40)
        self.assertFalse(any('apply' in c for c in self.calls('git')))

    def test_clone_failure_stops(self):
        self.failure(clone_error=True)
        self.assertFalse(self.calls('ninja'))

    def test_meson_failure_stops(self):
        self.existing(2)
        self.failure(meson_error=True)
        self.assertFalse(self.calls('ninja'))

    def test_venv_failure_stops(self):
        self.existing(2)
        (self.venv / 'Scripts/meson.exe').unlink()
        self.failure()
        self.assertEqual(len(self.calls('python')), 1)
        self.assertFalse(self.calls('ninja'))

    def test_failed_ninja_does_not_publish_stale_exe(self):
        self.existing(2)
        stale = self.source / 'build-win/cafecompiler/glslcompiler.elf.exe'
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b'stale compiler from an earlier build')
        self.failure(ninja_error=True)
        self.assertEqual(stale.read_bytes(), b'stale compiler from an earlier build')

    def test_success_without_exe_does_not_publish(self):
        self.existing(2)
        self.failure(no_compiler=True)

    def test_missing_dll_does_not_publish(self):
        self.existing(2)
        (self.msys / 'mingw64/bin/zlib1.dll').unlink()
        self.failure()

    def test_failed_exe_publication_restores_dlls(self):
        self.existing(2)
        target = self.output / NAMES[0]
        target.chmod(0o444)
        try:
            self.failure()
        finally:
            target.chmod(0o666)

    def test_nand_write_roots_are_rejected_before_clone(self):
        ticket = self.ticket()
        before = self.snapshot(ticket)
        for i in range(3):
            roots = [self.source, self.venv, self.output]
            roots[i] = ticket.parents[2] / ('new-root-' + str(i))
            with self.subTest(root=i):
                self.rejected_before_build(roots=roots)
                self.assertFalse(roots[i].exists())
                self.assertEqual(self.snapshot(ticket), before)

    def test_junction_write_roots_and_ancestors_are_rejected(self):
        outside = self.base / 'ordinary-external'
        outside.mkdir()
        ticket = outside / 'fake.tik'
        ticket.write_bytes(b'synthetic external ticket')
        os.utime(ticket, ns=(1600000000000000000, 1600000000000000000))
        before = self.snapshot(ticket)
        for i in range(3):
            alias = self.base / ('alias-' + str(i))
            self.junction(alias, outside)
            for path in (alias, alias / 'new-root'):
                roots = [self.source, self.venv, self.output]
                roots[i] = path
                with self.subTest(root=i, path=path):
                    self.rejected_before_build(roots=roots)
                    self.assertEqual(self.snapshot(ticket), before)
                    self.assertEqual(list(outside.iterdir()), [ticket])

    def test_hardlinked_output_files_preserve_ticket_aliases(self):
        ticket = self.ticket()
        for name in NAMES:
            target = self.output / name
            original = self.snapshot(target)
            target.unlink()
            os.link(ticket, target)
            self.before = self.bundle()
            before = self.snapshot(ticket)
            with self.subTest(name=name):
                self.rejected_before_build()
                self.assertEqual(self.snapshot(ticket), before)
                self.assertEqual(self.snapshot(target), before)
            target.unlink()
            target.write_bytes(original[0])
            os.utime(target, ns=(original[1], original[1]))
            self.before = self.bundle()

    def test_nested_source_and_venv_hardlinks_are_rejected_before_tools(self):
        self.existing(2)
        ticket = self.ticket()
        targets = (self.venv / 'Scripts/python3.exe', self.source / 'build-win/ninja.stdout.log',
                   self.source / 'cafecompiler/api.cpp')
        for target in targets:
            original = self.snapshot(target) if target.exists() else None
            target.parent.mkdir(parents=True, exist_ok=True)
            target.unlink(missing_ok=True)
            os.link(ticket, target)
            before = self.snapshot(ticket)
            with self.subTest(target=target):
                self.rejected_before_build()
                self.assertEqual(self.snapshot(ticket), before)
                self.assertEqual(self.snapshot(target), before)
            target.unlink()
            if original:
                target.write_bytes(original[0])
                os.utime(target, ns=(original[1], original[1]))

    def test_nested_source_and_venv_junctions_are_rejected_before_tools(self):
        outside = self.base / 'ordinary-external'
        outside.mkdir()
        ticket = outside / 'fake.tik'
        ticket.write_bytes(b'synthetic external ticket')
        before = self.snapshot(ticket)
        for i, folder in ((0, 'build-win'), (1, 'Scripts')):
            roots = [self.source, self.base / 'another-venv', self.output]
            roots[i].mkdir(exist_ok=True)
            alias = roots[i] / folder
            self.junction(alias, outside)
            with self.subTest(root=i):
                self.rejected_before_build(roots=roots)
                self.assertEqual(self.snapshot(ticket), before)
                self.assertEqual(list(outside.iterdir()), [ticket])
            alias.rmdir()

    def test_alias_created_during_build_is_rejected_before_staging(self):
        ticket = self.ticket()
        before = self.snapshot(ticket)
        result = self.run_recipe(late_alias=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        # The fixture's Ninja creates the second link; validation must leave both aliases alone.
        expected = before[:2] + (2,)
        self.assertEqual(self.snapshot(ticket), expected)
        self.assertEqual(self.snapshot(self.output / NAMES[0]), expected)
        for name in NAMES[1:]:
            self.assertEqual(self.snapshot(self.output / name), self.before[name])
        self.assertFalse(list(self.output.glob('.cafeglsl-*')))
        self.assertEqual(len(self.calls('validation')), 2)
        self.assertEqual(len(self.calls('ninja')), 1)


if __name__ == '__main__':
    unittest.main()
