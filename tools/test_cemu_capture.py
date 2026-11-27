"""Run the real PowerShell capture loop with a simulated clock and screenshot source."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from test_vertex_cache import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path)
    cases = ('available', 'dense', 'missing', 'exited', 'pad_exception', 'fatal', 'guest_exit', 'guest_exit_pad',
             'guest_exit_no_images', 'stale_locked', 'stale_recreated', 'partial', 'empty', 'no_signal', 'foreign_log',
             'checkpoint', 'checkpoint_missing', 'checkpoint_stale')
    parser.add_argument('--case', choices=cases)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (args.runner or root / 'tools/win/cemu_run.ps1').read_text()
    shot = function(source, 'function Shot(')
    guest_log = function(source, 'function Guest-Log {') if 'function Guest-Log {' in source else ''
    guest = function(source, 'function Guest-Exited {') if 'function Guest-Exited {' in source else ''
    report_exit = function(source, 'function Report-CemuExit {')
    cleanup_start = source.index('if (Test-Path -LiteralPath "$sd\\log.txt")')
    cleanup = source[cleanup_start:source.index('# Cemu killed', cleanup_start)]
    start = source.index('$per = ')
    loop = source[start:source.index('\nStart-Sleep -Seconds 3', start)]
    loop = loop.replace('[Diagnostics.Stopwatch]::StartNew()', '$script:fakeClock')
    fixture = r'''
$ErrorActionPreference = 'Stop'
$mode = $args[0]
$Seconds = 90; $Shots = 24; $shotDir = 'unused'; $Out = 'unused'; $sd = $args[1]
$CaptureAfterCommand = if ($mode -like 'checkpoint*') { 123 } else { 0 }
if ($mode -eq 'dense') { $Seconds = 345; $Shots = 138 }
$p = [pscustomobject]@{HasExited=$false;ExitCode=0;StartTime=[datetime]::new(2026,1,1,0,0,0,[DateTimeKind]::Utc)}
$script:fakeClock = [pscustomobject]@{Elapsed=[pscustomobject]@{TotalSeconds=0.0}}
$script:pending = $false; $script:tabHeld = $false; $script:captured = 0; $script:posted = @()
$script:wroteLog = $false
function Write-GuestLog([string]$text, [bool]$stale=$false) {
  [IO.File]::WriteAllText("$sd\log.txt", $text)
  $when = if ($stale) { $p.StartTime.AddSeconds(-60) } else { $p.StartTime.AddSeconds($script:fakeClock.Elapsed.TotalSeconds) }
  [IO.File]::SetLastWriteTimeUtc("$sd\log.txt", $when)
}
function Start-Sleep {
  param([int]$Milliseconds=0, [int]$Seconds=0)
  $script:fakeClock.Elapsed.TotalSeconds += $Milliseconds/1000.0 + $Seconds
  if ($mode -eq 'exited' -and $script:fakeClock.Elapsed.TotalSeconds -ge 18) { $p.HasExited=$true }
  $at = if ($mode -eq 'guest_exit_pad') { 4.15 } else { 18 }
  if (-not $script:wroteLog -and $script:fakeClock.Elapsed.TotalSeconds -ge $at) {
    $script:wroteLog=$true
    switch ($mode) {
      {$_ -in @('guest_exit','guest_exit_pad','guest_exit_no_images')} { Write-GuestLog "   100 [quit] sound system shut down`r`n" }
      'fatal' { Write-GuestLog "   100 FATAL: no recompiled code`r`n" }
      'stale_recreated' { Write-GuestLog "   100 [quit] sound system shut down`n" $true }
      'partial' { Write-GuestLog '   100 [quit] sound system shut down' }
      'empty' { Write-GuestLog '' }
      'no_signal' { Write-GuestLog "   100 [hang] frame stopped`n   100 [quit] audio thread stopped after 0 ms`n" }
      'foreign_log' { [IO.File]::WriteAllText("$sd\other-title.log", "   100 [quit] sound system shut down`n") }
      'checkpoint' { Write-GuestLog "   100 [live] command 123 starts (720 frames)`r`n" }
      'checkpoint_missing' { Write-GuestLog "   100 [live] command 1234 starts (720 frames)`n" }
      'checkpoint_stale' { Write-GuestLog "   100 [live] command 123 starts (720 frames)`n" $true }
    }
  }
  if ($script:fakeClock.Elapsed.TotalSeconds -gt 1000) { throw 'Unbounded capture loop' }
}
function Get-Date { [datetime]::new(2026,1,1).AddSeconds($script:fakeClock.Elapsed.TotalSeconds) }
function Post {
  param([uint32]$msg, [int]$key, [int64]$l)
  if ($key -eq 9) { $script:tabHeld = $msg -eq 0x100 }
  if ($key -eq 0x7B -and $msg -eq 0x101) { $script:pending=$true; $script:posted += $script:fakeClock.Elapsed.TotalSeconds }
}
function Get-ChildItem {
  if ($mode -eq 'pad_exception' -and $script:tabHeld) { throw 'Synthetic screenshot failure' }
  if ($mode -notin @('missing','exited','guest_exit_no_images') -and $script:pending) { [pscustomobject]@{FullName='unused'} }
}
function Remove-Item {
  param([string]$LiteralPath)
  if ($LiteralPath -eq "$sd\log.txt" -and $mode -ne 'stale_locked') { [IO.File]::Delete($LiteralPath) }
}
function Move-Item { $script:pending=$false; [void]($script:captured++) }
$script:cacheDrops = 0
function Drop-Caches { $script:cacheDrops++ }
if ($mode -eq 'stale_locked') { Write-GuestLog "   100 [quit] sound system shut down`n" $true }
'''
    fixture += '\n' + cleanup + '\n' + guest_log + '\n' + guest + '\n' + report_exit + '\n' + shot + '\n$caught=$false\ntry {\n' + loop + '\n} catch { $caught=$true }\n'
    fixture += r'''
if ($mode -ne 'pad_exception' -and $caught) { throw 'Unexpected capture failure' }
if ($mode -eq 'pad_exception' -and -not $caught) { throw 'Exception fixture did not execute' }
if ($script:tabHeld) { throw 'GamePad Tab key was left held' }
$normal = $mode -in @('available','dense','stale_locked','stale_recreated','partial','empty','no_signal','foreign_log')
if ($normal -and $script:captured -ne 2*$Shots) { throw 'Normal capture count changed or invalid log ended the run' }
if ($normal) {
  if ([Math]::Abs($script:fakeClock.Elapsed.TotalSeconds - ($Seconds+0.6)) -gt 0.000001) { throw 'Capture failed to cover the requested duration' }
  for ($j=0; $j -lt $Shots; $j++) {
    $when = $Seconds*($j+1)/[double]$Shots
    if ([Math]::Abs($script:posted[2*$j] - $when) -gt 0.000001 -or
        [Math]::Abs($script:posted[2*$j+1] - ($when+0.5)) -gt 0.000001) { throw 'Normal TV/Pad capture cadence changed' }
  }
}
if ($mode -eq 'missing' -and $script:fakeClock.Elapsed.TotalSeconds -gt ($Seconds+10.1)) { throw 'Missing screenshots exceeded the session budget' }
if ($mode -eq 'exited' -and $script:fakeClock.Elapsed.TotalSeconds -gt 18.6) { throw 'Continued capturing an exited process' }
if ($mode -eq 'fatal' -and ($script:captured -ne 10 -or $script:fakeClock.Elapsed.TotalSeconds -gt 19.6)) { throw 'Did not capture one final pair after the fatal log' }
if ($mode -in @('guest_exit','guest_exit_no_images') -and $script:fakeClock.Elapsed.TotalSeconds -gt 19.1) { throw "Continued capturing after current-session runtime shutdown: $($script:fakeClock.Elapsed.TotalSeconds) seconds, $($script:captured) screenshots" }
if ($mode -eq 'guest_exit_pad' -and ($script:captured -ne 1 -or $script:fakeClock.Elapsed.TotalSeconds -gt 4.6)) { throw 'Did not stop safely during the GamePad pair' }
if ($mode -eq 'checkpoint' -and ($script:captured -ne 2*$Shots -or $script:posted[0] -ne 21)) { throw 'Did not wait for the guest checkpoint' }
if ($mode -in @('checkpoint_missing','checkpoint_stale') -and ($script:captured -ne 0 -or $script:fakeClock.Elapsed.TotalSeconds -ne $Seconds)) {
  throw 'Captured an unverified route or exceeded the deadline waiting for a checkpoint'
}
if ($mode -like 'guest_exit*' -and $p.HasExited) { throw 'Fixture must leave the Cemu GUI alive' }
$normalExit = Report-CemuExit
if ($mode -like 'guest_exit*') {
  if ($normalExit -or $script:cacheDrops -ne 0) { throw 'Clean guest exit was reported as an emulator crash' }
} elseif ($normalExit -notlike 'Cemu exited by itself,*' -or $script:cacheDrops -ne 1) {
  throw 'Unexpected emulator exit was accepted without evidence of guest shutdown'
}
$script:cacheDrops = 0
if ((Report-CemuExit -CloseRequested) -or $script:cacheDrops -ne 0) { throw 'Requested clean close was reported as a crash' }
$p.ExitCode = 2
if ((Report-CemuExit -CloseRequested) -notlike 'Cemu exited by itself, code 2' -or $script:cacheDrops -ne 1) {
  throw 'Nonzero emulator exit code was accepted after WM_CLOSE'
}
@{mode=$mode; seconds=$script:fakeClock.Elapsed.TotalSeconds; captures=$script:captured; tab_released=(-not $script:tabHeld); cemu_alive=(-not $p.HasExited)} | ConvertTo-Json -Compress
'''
    out = root / 'build_win/selftest/cemu_capture' / hashlib.sha256(source.encode()).hexdigest()[:12]
    out.mkdir(parents=True, exist_ok=True)
    path = out / 'capture_fixture.ps1'
    path.write_text(fixture, encoding='utf-8')
    for case in (args.case,) if args.case else cases:
        case_dir = out / case
        case_dir.mkdir(exist_ok=True)
        result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(path), case, str(case_dir)], capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise AssertionError(case + ': ' + result.stdout + result.stderr)
        report = json.loads(result.stdout.splitlines()[-1])
        print(f'Cemu capture: {case}, {report["seconds"]:.1f} simulated seconds, {report["captures"]} screenshots')


if __name__ == '__main__':
    main()
