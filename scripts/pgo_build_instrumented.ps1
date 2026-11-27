<#
PGO step 1: build the instrumented programs build_pgo_gen\ResistanceFoM.exe + ResistanceFoMGame.exe (build_cpu.bat pgo-gen).
Instruments the game runtime (incl. the D3D12 renderer), the hand-written code and the lifted SPU code.
The normal build\ and build_game\ are not touched. Then: tools\pgo_train.bat, then scripts\pgo_merge_and_build.ps1.
Never starts or stops the game. Takes the repo's scratch\BUILD.lock like build_locked.ps1.
#>
param([int]$Jobs = 4, [int]$TimeoutMin = 120)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$cpu = Get-Content (Join-Path $repo 'build_cpu.bat') -Raw
$game = if ($cpu -match 'build_cpu\\(\w+)\.exe') { $Matches[1] } else { throw "build_cpu.bat: exe name not found" }
$lock = Join-Path $repo 'scratch\BUILD.lock'
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
$t0 = Get-Date; $fs = $null
while (-not $fs) {
    try { $fs = [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { if (((Get-Date) - $t0).TotalMinutes -gt $TimeoutMin) { Write-Output "LOCK TIMEOUT"; exit 2 }; Start-Sleep 10 }
}
try {
    $target = Join-Path $repo "build_pgo_gen\$game.exe"
    while (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -ieq $target -or $_.Path -ieq ($target -replace '\.exe$','Game.exe')) }) {
        if (((Get-Date) - $t0).TotalMinutes -gt $TimeoutMin) { Write-Output "build_pgo_gen\$game.exe is still running - build skipped"; exit 3 }
        Start-Sleep 15
    }
    $env:PORT_JOBS = "$Jobs"
    $log = Join-Path $repo "scratch\pgo_gen_$(Get-Date -f MMdd_HHmmss).log"
    Write-Output "Building $target (build_cpu.bat pgo-gen; log $log) ..."
    $p = Start-Process cmd.exe -ArgumentList ('/s /c ""' + "$repo\build_cpu.bat" + '" pgo-gen nopause > "' + $log + '" 2>&1"') -WorkingDirectory $repo -PassThru -WindowStyle Hidden
    $p.WaitForExit()
    Get-Content $log -Tail 8
    if ($p.ExitCode -ne 0) { Write-Output "Instrumented build FAILED (exit $($p.ExitCode))"; exit $p.ExitCode }
    Write-Output "Done: $target"
    Write-Output "Next: tools\pgo_train.bat (play the usual heavy scenes for 10+ minutes), then scripts\pgo_merge_and_build.ps1"
} finally { $fs.Close() }
