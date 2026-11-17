# Build CafeGLSL v0.2.0 for Windows, with the Windows fixes and allocator-independent r600 scheduling patch.
# Needs MSYS2's MinGW GCC, flex, bison and ninja, Windows Python and git. Source/venv paths must have no spaces.
# The source and venv are retained for later builds; use explicit paths to build a separate candidate.
param(
  [ValidateRange(1, 64)][int]$Jobs = 4,
  [string]$Source = 'C:\devkitPro\src\CafeGLSL',
  [string]$Venv = 'C:\devkitPro\src\venv',
  [string]$Output = (Join-Path $PSScriptRoot '..\..\third_party\cafeglsl'),
  [string]$Msys = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$revision = 'e3e385c700e6e70dd2d9d6cd015162991084df6b'  # upstream v0.2.0
$Source = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Source)
$Venv = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Venv)
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$Msys = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Msys)

function Invoke-CheckedNative([string]$Program, [string[]]$NativeArgs) {
  $command = Get-Command -Name $Program -ErrorAction Stop
  $saved = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'  # PS5 can turn native stderr into an error before its exit code is checked.
    & $command @NativeArgs
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $saved }
  if ($code -ne 0) { throw "$Program failed with exit code $code" }
}

function Assert-WritePaths([switch]$Trees) {
  # Reuse the build's NAND/reparse/shared-file checks before any writable roots are used.
  $check = @'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from objcache import _checked
for root in sys.argv[2:5]:
    _checked(root, directory=True)
for name in ('libwinpthread-1.dll', 'zlib1.dll', 'glslcompiler.exe'):
    _checked(Path(sys.argv[4]) / name)
if sys.argv[5] == 'True':
    for root in sys.argv[2:4]:
        pending = [Path(root)]
        while pending:
            parent = pending.pop()
            if not parent.exists():
                continue
            for entry in parent.iterdir():
                directory = entry.is_dir()
                _checked(entry, directory=directory)
                # Existing Git metadata is read by these commands, not patched or built.
                if directory and entry != Path(sys.argv[2]) / '.git':
                    pending.append(entry)
'@
  Invoke-CheckedNative python @('-B', '-c', $check, $PSScriptRoot, $Source, $Venv, $Output, "$($Trees.IsPresent)")
}

function Ensure-Patch([string]$Patch) {
  $saved = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    & git -C $Source -c core.autocrlf=false apply --reverse --check $Patch 2>$null
    $applied = $LASTEXITCODE -eq 0
  } finally { $ErrorActionPreference = $saved }
  if ($applied) { return }
  Invoke-CheckedNative git @('-C', $Source, '-c', 'core.autocrlf=false', 'apply', '--check', $Patch)
  Invoke-CheckedNative git @('-C', $Source, '-c', 'core.autocrlf=false', 'apply', $Patch)
  Invoke-CheckedNative git @('-C', $Source, '-c', 'core.autocrlf=false', 'apply', '--reverse', '--check', $Patch)
}

Assert-WritePaths -Trees
if (-not (Test-Path -LiteralPath $Source)) {
  Invoke-CheckedNative git @('-c', 'core.longpaths=true', '-c', 'core.autocrlf=false', 'clone', '--quiet', '--depth', '1', '--branch', 'v0.2.0', 'https://github.com/Exzap/CafeGLSL.git', $Source)
}
$actual = (Invoke-CheckedNative git @('-C', $Source, 'rev-parse', 'HEAD')).Trim()
if ($actual -ne $revision) { throw "CafeGLSL must be v0.2.0 ($revision), not $actual" }
# Existing trees must be checked too; meson.build exists both before and after either patch.
foreach ($patch in 'cafeglsl_windows.patch', 'cafeglsl_deterministic.patch') {
  Ensure-Patch (Join-Path $PSScriptRoot $patch)
}

if (-not (Test-Path -LiteralPath "$Venv\Scripts\meson.exe")) {
  Invoke-CheckedNative python @('-m', 'venv', $Venv)
  Invoke-CheckedNative "$Venv\Scripts\python.exe" @('-m', 'pip', 'install', '--quiet', 'meson', 'mako', 'setuptools', 'packaging')
  if (-not (Test-Path -LiteralPath "$Venv\Scripts\meson.exe")) { throw 'pip did not install meson' }
}
# meson looks for python3 first, and MSYS2's has no mako. This also covers a pre-created venv.
Copy-Item -LiteralPath "$Venv\Scripts\python.exe" -Destination "$Venv\Scripts\python3.exe" -Force
$savedPath = $env:PATH
Push-Location -LiteralPath $Source
try {
  $env:PATH = "$Venv\Scripts;$Msys\mingw64\bin;$env:PATH;$Msys\usr\bin"
  if (-not (Test-Path -LiteralPath "$Source\build-win\build.ninja")) {
    Invoke-CheckedNative meson @('setup', 'build-win', '-Db_sanitize=none', '-Dtools=glsl', '-Dvulkan-drivers=', '-Dgallium-drivers=r600', '-Db_lundef=false', '-Dglx=disabled', '-Degl=disabled', '-Dplatforms=', '-Dllvm=disabled', '-Dzstd=disabled', '--buildtype=release')
  }
  $process = Start-Process -FilePath "$Msys\mingw64\bin\ninja.exe" -WorkingDirectory $Source -ArgumentList @('-C', 'build-win', '-j', "$Jobs", 'cafecompiler/glslcompiler.elf.exe') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Source\build-win\ninja.stdout.log" -RedirectStandardError "$Source\build-win\ninja.stderr.log"
  try {
    $null = $process.Handle
    try { if (-not $process.HasExited) { $process.PriorityClass = 'BelowNormal' } }
    catch { if (-not $process.HasExited) { throw } }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "ninja failed with exit code $($process.ExitCode); see $Source\build-win\ninja.stderr.log" }
  } finally { $process.Dispose() }
  $compiler = "$Source\build-win\cafecompiler\glslcompiler.elf.exe"
  if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'ninja did not produce the compiler' }
} finally {
  Pop-Location
  $env:PATH = $savedPath
}

# Stage all inputs before publishing; replace the executable last, atomically, so a failed build/copy keeps the old compiler.
Assert-WritePaths
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$stage = Join-Path $Output ('.cafeglsl-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$published = @()
$keepStage = $false
try {
  Copy-Item -LiteralPath $compiler -Destination "$stage\glslcompiler.exe"
  foreach ($dll in 'libwinpthread-1.dll', 'zlib1.dll') {
    Copy-Item -LiteralPath "$Msys\mingw64\bin\$dll" -Destination "$stage\$dll"
  }
  foreach ($name in 'libwinpthread-1.dll', 'zlib1.dll', 'glslcompiler.exe') {
    Assert-WritePaths
    $destination = Join-Path $Output $name
    if (Test-Path -LiteralPath $destination) { [IO.File]::Replace((Join-Path $stage $name), $destination, (Join-Path $stage ($name + '.previous'))) }
    else { [IO.File]::Move((Join-Path $stage $name), $destination) }
    $published += $name
  }
} catch {
  $publicationError = $_
  for ($i = $published.Count - 1; $i -ge 0; $i--) {
    $name = $published[$i]
    $backup = Join-Path $stage ($name + '.previous')
    try {
      Assert-WritePaths
      if (Test-Path -LiteralPath $backup) { [IO.File]::Replace($backup, (Join-Path $Output $name), (Join-Path $stage ($name + '.discard'))) }
      else { Remove-Item -LiteralPath (Join-Path $Output $name) }
    } catch { $keepStage = $true }
  }
  if ($keepStage) { throw "Compiler publication failed; previous files are retained in $stage. $publicationError" }
  throw $publicationError
} finally {
  $resolvedStage = [IO.Path]::GetFullPath($stage)
  if (-not $resolvedStage.StartsWith($Output.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid compiler staging path' }
  if (-not $keepStage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
"built $Output\glslcompiler.exe"
