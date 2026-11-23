# Tests for tools\installer\progress.ps1 (Setup's progress, time estimates and step timings) without a real build:
# fake generator output, fake Ninja lines and fake build folders in build_test_setup\ (git-ignored).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\tests\test_progress.ps1
$ErrorActionPreference = 'Stop'
$Port = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$Root = Join-Path $Port 'build_test_setup\progress'
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Root | Out-Null
. (Join-Path $Port 'tools\installer\progress.ps1')
$script:SpEmit = $false; $script:SpInteractive = $false

$script:Failed = 0
function Check($Ok, [string]$What) {
    if ([bool]$Ok) { Write-Host "  ok   $What" } else { Write-Host "  FAIL $What"; $script:Failed++ }
}
function Near([double]$a, [double]$b, [double]$tol = 0.01) { [math]::Abs($a - $b) -le $tol }

Write-Host 'generator progress lines'
$p = ConvertFrom-SpRegenLine '@@PROGRESS stage=ppu_lift done=120 total=35741'
Check ($p -and $p.Stage -eq 'ppu_lift' -and $p.Done -eq 120 -and $p.Total -eq 35741) 'parses stage, done and total'
Check ($null -eq (ConvertFrom-SpRegenLine '[r3] lifted 120/35741 functions')) 'ignores ordinary lines'
Check ($null -eq (ConvertFrom-SpRegenLine 'x @@PROGRESS stage=a done=1 total=2')) 'only at the start of a line'

$w = @{ a = 10; b = 30; c = 60 }
$stages = 'a', 'b', 'c'
Check (Near (Get-SpRegenFraction $stages @{} $w) 0) 'nothing reported: 0'
Check (Near (Get-SpRegenFraction $stages @{ a = @{ Done = 5; Total = 10 } } $w) 0.05) 'half of the first stage (weight 10 of 100)'
Check (Near (Get-SpRegenFraction $stages @{ b = @{ Done = 0; Total = 10 } } $w) 0.10) 'a later stage started: the earlier one counts as done'
Check (Near (Get-SpRegenFraction $stages @{ a = @{ Done = 3; Total = 10 }; c = @{ Done = 30; Total = 60 } } $w) 0.70) 'a stage that printed less than its total but was followed by another counts fully'
Check (Near (Get-SpRegenFraction $stages @{ c = @{ Done = 60; Total = 60 } } $w) 1) 'last stage done: 1'
Check (Near (Get-SpRegenFraction @('a') @{ a = @{ Done = 1; Total = 1 }; z = @{ Done = 0; Total = 4 } } $w) (10 / 11)) 'an unexpected stage is added at the end with weight 1'

Write-Host 'Ninja lines'
$n = ConvertFrom-SpNinjaLine '[12/436] Building CXX object CMakeFiles\Resistance3.dir\D_\Games\Resistance3Port\recompiled\ppu_recomp_012.cpp.obj'
Check ($n.N -eq 12 -and $n.M -eq 436 -and $n.Key -eq 'recompiled\ppu_recomp_012.cpp') 'a PPU file: step numbers and the file'
$n = ConvertFrom-SpNinjaLine '[13/436] Building CXX object CMakeFiles/Resistance3.dir/D_/x/recompiled_game/ppu_recomp_game_003.cpp.obj'
Check ($n.Key -eq 'recompiled_game\ppu_recomp_game_003.cpp') 'R1''s second PPU folder, forward slashes'
$n = ConvertFrom-SpNinjaLine '[14/436] Building C object CMakeFiles\Resistance3.dir\D_\x\project\spu\igjob_1\spu_recomp.c.obj'
Check ($n.N -eq 14 -and $null -eq $n.Key) 'another file: steps only'
Check ($null -eq (ConvertFrom-SpNinjaLine 'Building the game...')) 'not a Ninja line'
Check ($null -eq (ConvertFrom-SpNinjaLine '[1/2] Building front end (EBOOT)...')) 'nor is build.bat''s own [1/2] (R1)'

Write-Host 'build estimate (by compile time, not by step count)'
# R3's shape: 4 heavy files (~10 min) and 25 light ones (~1.5 min), 2 at a time
$plan = @{}
for ($i = 0; $i -lt 29; $i++) { $plan["recompiled\ppu_recomp_{0:000}.cpp" -f $i] = @{ Bytes = 150e6; Cost = $(if ($i -lt 4) { 700.0 } else { 90.0 }) } }
$now = Get-Date
$done = @{}; for ($i = 4; $i -lt 29; $i++) { $done["recompiled\ppu_recomp_{0:000}.cpp" -f $i] = $true }
$run = @{ 'recompiled\ppu_recomp_000.cpp' = $now.AddSeconds(-100); 'recompiled\ppu_recomp_001.cpp' = $now.AddSeconds(-100) }
$e = Get-SpBuildEstimate $plan $done $run $now 2 1.0
# 25 of 29 files done but only 2250 of 5050 predicted seconds: the bar must not say "nearly done"
Check ($e.Fraction -gt 0.45 -and $e.Fraction -lt 0.55) ("25 light files done, 4 heavy left: about half ({0:0.00})" -f $e.Fraction)
Check ($e.Eta -gt 900) ("time left counts the heavy files still to come ({0:0} s)" -f $e.Eta)
Check ($e.DoneCount -eq 25 -and $e.Total -eq 29 -and $e.Running -eq 2) 'counts'
Check ($e.Current -eq 'recompiled\ppu_recomp_000.cpp' -or $e.Current -eq 'recompiled\ppu_recomp_001.cpp') 'the file compiling longest is shown'
$e2 = Get-SpBuildEstimate $plan $done $run $now 2 2.0
Check ($e2.Eta -gt $e.Eta * 1.8) 'a slower PC (speed 2) roughly doubles the time left'
$runLate = @{ 'recompiled\ppu_recomp_000.cpp' = $now.AddSeconds(-5000) }
$done3 = $done.Clone(); $done3['recompiled\ppu_recomp_001.cpp'] = $true; $done3['recompiled\ppu_recomp_002.cpp'] = $true; $done3['recompiled\ppu_recomp_003.cpp'] = $true
$e3 = Get-SpBuildEstimate $plan $done3 $runLate $now 2 1.0
Check ($e3.Eta -gt 60 -and $e3.Fraction -lt 1) ("a file running far longer than predicted: never 0 s left, never 100% ({0:0} s, {1:0.000})" -f $e3.Eta, $e3.Fraction)
$e4 = Get-SpBuildEstimate @{} @{} @{} $now 2 1.0
Check ($e4.Fraction -eq 1 -and $e4.Eta -eq 0) 'nothing to compile: done'
Check (Near (Get-SpSpeed 1.0 0 0) 1.0) 'speed: nothing measured keeps the prior'
Check ((Get-SpSpeed 1.0 1200 2400) -gt 1.5) 'speed: files took twice as long -> slower'

Write-Host 'build plan from the build folder'
$fp = Join-Path $Root 'port'
$rc = Join-Path $fp 'recompiled'; $ob = Join-Path $fp 'build\CMakeFiles\Resistance3.dir\D_\x\recompiled'
New-Item -ItemType Directory -Force -Path $rc, $ob | Out-Null
$t0 = (Get-Date).AddHours(-2)
foreach ($i in 0..2) {
    $f = Join-Path $rc ("ppu_recomp_{0:000}.cpp" -f $i); [IO.File]::WriteAllText($f, ('x' * (1000 * ($i + 1)))); (Get-Item $f).LastWriteTime = $t0
    $o = Join-Path $ob ("ppu_recomp_{0:000}.cpp.obj" -f $i); [IO.File]::WriteAllText($o, 'o'); (Get-Item $o).LastWriteTime = $t0.AddMinutes(10)
}
$h = Join-Path $rc 'ppu_recomp.h'; [IO.File]::WriteAllText($h, 'h'); (Get-Item $h).LastWriteTime = $t0
(Get-Item (Join-Path $rc 'ppu_recomp_001.cpp')).LastWriteTime = $t0.AddMinutes(20)   # regenerated after its object
Remove-Item (Join-Path $ob 'ppu_recomp_002.cpp.obj')                                     # never compiled
$pl = Get-SpPpuBuildPlan $fp @(, @('recompiled', 'build'))
Check ($pl.Count -eq 2 -and $pl.ContainsKey('recompiled\ppu_recomp_001.cpp') -and $pl.ContainsKey('recompiled\ppu_recomp_002.cpp')) 'only changed and never-compiled files'
Check ($pl['recompiled\ppu_recomp_002.cpp'].Bytes -eq 3000) 'their size'
(Get-Item $h).LastWriteTime = $t0.AddMinutes(30)
Check ((Get-SpPpuBuildPlan $fp @(, @('recompiled', 'build'))).Count -eq 3) 'a newer generated header: everything'
# compile times: .ninja_log, the shipped table and the size fallback
$nl = Join-Path $fp 'build\.ninja_log'
[IO.File]::WriteAllLines($nl, [string[]]@('# ninja log v6', "0`t300000`t0`tCMakeFiles/Resistance3.dir/D_/x/recompiled/ppu_recomp_001.cpp.obj`tabc",
                                           "0`t5000`t0`tCMakeFiles/Resistance3.dir/D_/x/other.cpp.obj`tdef", "1000`t121000`t0`tCMakeFiles/Resistance3.dir/D_/x/recompiled/ppu_recomp_001.cpp.obj`tabc"))
$tm = Read-SpNinjaLog $nl 'recompiled'
Check ($tm.Count -eq 1 -and (Near $tm['recompiled\ppu_recomp_001.cpp'].Sec 120)) '.ninja_log: the latest compile of each PPU file'
$tt = Join-Path $fp 'tools\installer'; New-Item -ItemType Directory -Force -Path $tt | Out-Null
[IO.File]::WriteAllLines((Join-Path $tt 'ppu_compile_times.txt'), [string[]]@('# test', 'recompiled\ppu_recomp_000.cpp 600 1000', 'recompiled\ppu_recomp_001.cpp 50 2000'))
$times = Get-SpCompileTimes $fp @(, @('recompiled', 'build'))
Check ((Near $times['recompiled\ppu_recomp_000.cpp'].Sec 600) -and (Near $times['recompiled\ppu_recomp_001.cpp'].Sec 50)) 'the shipped table (with sizes) wins over .ninja_log (no sizes)'
Check (Near (Get-SpFileCost 'recompiled\ppu_recomp_000.cpp' 1500 $times) 900) 'a known file that grew by half: its time scales with it'
Check (Near (Get-SpFileCost 'recompiled\ppu_recomp_000.cpp' 100000 $times) 1200) '... at most twice'
Check (Near (Get-SpFileCost 'recompiled\ppu_recomp_009.cpp' 7e6 $times) 10) 'an unknown file: by its size (0.7 MB/s per job)'

Write-Host 'overall bar and timings file'
$est = [ordered]@{ check = 10; tools = 10; gen = 30; build = 50 }
Check (Near (Get-SpOverall $est @('check', 'tools') 'gen' 0.5) 0.35) 'done steps count fully, the current one by its fraction'
Check (Near (Get-SpOverall $est @() 'check' 2) 0.1) 'a fraction above 1 is capped'
Check ((Format-SpEta 30) -eq 'less than a minute left' -and (Format-SpEta 125) -eq 'about 3 min left' -and (Format-SpEta 7200) -eq 'about 2:00 h left' -and (Format-SpEta -1) -eq '') 'time left texts'
Check ((Format-SpDuration 75) -eq '1:15' -and (Format-SpDuration 3725) -eq '1:02:05') 'durations'

$logs = Join-Path $fp 'logs'; New-Item -ItemType Directory -Force -Path $logs | Out-Null
Initialize-SpProgress $fp -Unattended
Check ($null -eq $script:SpOld) 'no timings file yet'
Check ((Get-SpTime 'steps' 'gen') -eq 1000) 'defaults without a timings file'
Check ((Get-SpTime 'steps' 'game_build') -eq 565) 'first install: the game build time from the port''s compile-time table (650 s on 2 jobs + 240 s)'
Check ((Get-SpTime 'steps' 'rpcs3') -eq 0 -and -not (Test-SpHasRpcs3 $fp)) 'a port without RPCS3''s renderer in build.bat (R1): no time for it'
[IO.File]::WriteAllText((Join-Path $fp 'build.bat'), 'cmake --build build_rpcs3')
$script:SpDefaults['steps']['rpcs3'] = 600
Initialize-SpProgress $fp -Unattended
Check ((Get-SpTime 'steps' 'rpcs3') -eq 600 -and (Test-SpHasRpcs3 $fp)) 'with it (R2, R3): its default'
$script:SpRun.steps['gen'] = 432.1; $script:SpRun.regen['ppu_lift'] = 250; $script:SpRun.build['speed'] = 1.5
$script:SpRun['files'] = @{ 'recompiled\ppu_recomp_000.cpp' = '321.5 1000' }
Save-SpTimings
Check (Test-Path (Join-Path $logs 'setup_timings.json')) 'logs\setup_timings.json written'
$script:SpRun = @{ steps = @{}; regen = @{}; build = @{} }
Initialize-SpProgress $fp -Unattended
Check ((Near (Get-SpTime 'steps' 'gen') 432.1) -and (Near (Get-SpTime 'regen' 'ppu_lift') 250) -and (Near (Get-SpTime 'build' 'speed') 1.5)) 'the next run uses the measured times'
Check ((Get-SpTime 'steps' 'tools') -eq 60) 'steps not measured keep the default'
$times = Get-SpCompileTimes $fp @(, @('recompiled', 'build'))
Check (Near $times['recompiled\ppu_recomp_000.cpp'].Sec 321.5) 'per-file compile times measured on this PC win over the shipped table'
$script:SpRun.steps['tools'] = 12
Save-SpTimings
$o = Read-SpTimings (Join-Path $logs 'setup_timings.json')
Check ((Near $o.steps['gen'] 432.1) -and (Near $o.steps['tools'] 12) -and $o.files['recompiled\ppu_recomp_000.cpp']) 'a later run keeps what it did not measure again'
[IO.File]::WriteAllText((Join-Path $logs 'setup_timings.json'), '{ broken')
Check ($null -eq (Read-SpTimings (Join-Path $logs 'setup_timings.json'))) 'a broken timings file is ignored'
# the log can be read while Setup writes it (FileShare.ReadWrite on both sides)
$lf = Join-Path $logs 'build_rpcs3.log'
$ws = New-Object IO.FileStream($lf, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
$wr = New-Object IO.StreamWriter($ws); $wr.AutoFlush = $true
$wr.WriteLine('[1/900] Building CXX object a.obj'); $wr.WriteLine('[37/900] Building CXX object b.obj')
$off = 0L
$r = Get-SpLogNinja $lf ([ref]$off)
Check ($r -and $r.N -eq 37 -and $r.M -eq 900 -and $off -gt 0) 'reads the last [N/M] of a log that is still open for writing'
$wr.Dispose()

Write-Host 'running the generator with progress'
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if ($py) {
    $script:SpRun = @{ steps = @{}; regen = @{}; build = @{} }
    $s = Join-Path $Root 'fake_regen.py'
    [IO.File]::WriteAllText($s, @"
import time, sys
for st, n in (('ppu_lift', 3), ('ppu_post', 2)):
    for i in range(n + 1):
        print('@@PROGRESS stage=%s done=%d total=%d' % (st, i, n), flush=True)
        time.sleep(0.4)
print('[r3] ordinary line')
print('warning on stderr', file=sys.stderr)
sys.exit(0)
"@)
    $out = & { Invoke-SpRegen $py ('"' + $s + '"') @('ppu_lift', 'ppu_post') } 6>&1 | ForEach-Object { "$_" }
    $code = $out[-1]
    Check ($code -eq '0') 'exit code passed on'
    Check (@($out | Where-Object { $_ -eq '[r3] ordinary line' }).Count -eq 1) 'ordinary lines are shown'
    Check (@($out | Where-Object { $_ -like '@@PROGRESS*' }).Count -eq 0) 'progress lines are not'
    Check (@($out | Where-Object { $_ -eq 'warning on stderr' }).Count -eq 1) 'stderr is shown'
    Check ($script:SpRun.regen['ppu_lift'] -gt 0.5 -and $script:SpRun.regen['ppu_post'] -gt 0.3) ('stage times measured (ppu_lift {0:0.0} s)' -f $script:SpRun.regen['ppu_lift'])
    [IO.File]::WriteAllText($s, "import sys`nprint('@@PROGRESS stage=ppu_lift done=1 total=3')`nsys.exit(3)`n")
    $code = Invoke-SpRegen $py ('"' + $s + '"') @('ppu_lift') 6>$null
    Check ($code -eq 3 -and $script:SpRun.regen.Count -eq 0) 'a failed run: its exit code, and its times are not kept'
    $script:SpEmit = $true; $script:SpLastEmit = [datetime]::MinValue; $script:SpShown = 0
    $script:SpSteps = New-Object System.Collections.Specialized.OrderedDictionary; $script:SpSteps['gen'] = 10; $script:SpCur = 'gen'
    $sw = New-Object IO.StringWriter; $old = [Console]::Out; [Console]::SetOut($sw)
    try { Write-SpStatus 'Generating the game code' 0.5 125 14 6>$null } finally { [Console]::SetOut($old) }
    $script:SpEmit = $false
    Check ($sw.ToString() -match '^@@SETUP pct=500 eta=125 idle=14 Generating the game code, about 3 min left - still working, last activity 14 s ago') ('Setup.exe line: ' + $sw.ToString().Trim())
} else { Write-Host '  SKIP python not found' }

Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue
if ($script:Failed) { Write-Host "$($script:Failed) test(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'All progress tests passed.' -ForegroundColor Green
exit 0
