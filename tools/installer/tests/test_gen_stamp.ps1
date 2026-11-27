# Tests for the code generator's stamp (tools\build_doctor.ps1: Get-GenPlan, Save-GenStamp, Test-GenStale): which half
# of the game code (PPU / SPU) Setup generates again after an update, with fake generator files in build_test_setup\
# (git-ignored).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\tests\test_gen_stamp.ps1
$ErrorActionPreference = 'Stop'
$Port = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$Root = Join-Path $Port 'build_test_setup\genstamp'
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Root | Out-Null
function Info($t) { }
. (Join-Path $Port 'tools\build_doctor.ps1')

$script:Failed = 0
function Check($Ok, [string]$What) {
    if ([bool]$Ok) { Write-Host "  ok   $What" } else { Write-Host "  FAIL $What"; $script:Failed++ }
}
function Put([string]$Rel, [string]$Text) {
    $p = Join-Path $fp $Rel; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
    [IO.File]::WriteAllText($p, $Text.Replace("`r`n", "`n"))
}
function Plan { Get-GenPlan $fp 'x_regen.py' 'x_spu.py' }
function Show($p) { "Ppu=$($p.Ppu) Spu=$($p.Spu) Why=$($p.Why) Legacy=$($p.Legacy)" }

# a fake port: the driver, the shared helpers (which name an SPU-only tool), the SPU half, the lifter, hooks
$fp = Join-Path $Root 'port'
Put 'tools\x_regen.py' "import os`nfrom regen_common import ppu`nfrom x_spu import spu`nHOOKS = 'x.json'`n"
Put 'tools\regen_common.py' "import os`nLIFTER = 'ppu_lifter.py'`nPOST = ('spu_hooks.py',)`n"
Put 'tools\x_spu.py' "from regen_common import run`nIGJOB_EXTRA = {}`n"
Put 'tools\spu_hooks.py' "# SPU-only tool`n"
Put 'tools\hooks\x.json' '{"ppu": 1}'
Put 'tools\hooks\spu\gate.json' '{"spu": 1}'
Put 'ps3recomp\tools\ppu_lifter.py' "# the lifter`n"
New-Item -ItemType Directory -Force -Path (Join-Path $fp 'recompiled') | Out-Null
$stamp = Join-Path $fp 'recompiled\.generator_inputs'

Write-Host 'input sets'
$parts = Get-GenParts $fp 'x_regen.py' 'x_spu.py'
$pn = @($parts.PpuFiles | ForEach-Object { Split-Path -Leaf $_ }); $sn = @($parts.SpuFiles | ForEach-Object { Split-Path -Leaf $_ })
Check (($pn -contains 'x_regen.py') -and ($pn -contains 'regen_common.py') -and ($pn -contains 'ppu_lifter.py') -and ($pn -contains 'x.json')) ('PPU inputs: ' + ($pn -join ', '))
Check (-not ($pn -contains 'x_spu.py') -and -not ($pn -contains 'spu_hooks.py') -and -not ($pn -contains 'gate.json')) 'PPU inputs leave out the SPU script, SPU-only tools and hooks\spu'
Check (($sn -contains 'x_spu.py') -and ($sn -contains 'regen_common.py') -and ($sn -contains 'spu_hooks.py') -and ($sn -contains 'gate.json') -and ($sn -contains 'x_regen.py')) ('SPU inputs: ' + ($sn -join ', '))
Check (-not ($sn -contains 'x.json')) 'SPU inputs leave out the PPU hooks'

Write-Host 'plans'
$p = Plan
Check ($p.Ppu -and $p.Spu -and $p.Why -eq 'missing') ('no stamp: both (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
Check ((Get-Content $stamp -TotalCount 1) -eq 'v2') 'the stamp is written in the split format'
$p = Plan
Check (-not $p.Ppu -and -not $p.Spu -and $null -eq $p.Why) ('just generated: nothing (' + (Show $p) + ')')
Check ($null -eq (Test-GenStale $fp 'x_regen.py' 'recompiled' 'x_spu.py')) 'Test-GenStale: up to date'

Put 'tools\x_spu.py' "from regen_common import run`nIGJOB_EXTRA = {0x1: 2}`n"
$p = Plan
Check (-not $p.Ppu -and $p.Spu -and $p.Why -eq 'changed') ('an SPU table changed: SPU only (' + (Show $p) + ')')
Check ((Test-GenStale $fp 'x_regen.py' 'recompiled' 'x_spu.py') -eq 'changed') 'Test-GenStale: changed'
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
Put 'tools\spu_hooks.py' "# SPU-only tool, changed`n"
$p = Plan; Check (-not $p.Ppu -and $p.Spu) ('an SPU-only tool changed: SPU only (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
Put 'tools\hooks\spu\gate.json' '{"spu": 2}'
$p = Plan; Check (-not $p.Ppu -and $p.Spu) ('an SPU hook changed: SPU only (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
Put 'tools\hooks\x.json' '{"ppu": 2}'
$p = Plan; Check ($p.Ppu -and -not $p.Spu) ('a PPU hook changed: PPU only (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
Put 'ps3recomp\tools\ppu_lifter.py' "# the lifter, changed`n"
$p = Plan; Check ($p.Ppu -and $p.Spu) ('a shared tool changed: both (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'
[IO.File]::WriteAllText((Join-Path $fp 'tools\x_spu.py'), [IO.File]::ReadAllText((Join-Path $fp 'tools\x_spu.py')).Replace("`n", "`r`n"))
$p = Plan; Check (-not $p.Ppu -and -not $p.Spu) 'line endings alone change nothing (git checkout vs release zip)'

Write-Host 'unfinished runs'
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py' -SpuPending
$p = Plan; Check (-not $p.Ppu -and $p.Spu) ('SPU generation stopped half way: SPU again (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py' -PpuPending -SpuPending
$p = Plan; Check ($p.Ppu -and $p.Spu) ('both stopped half way: both again (' + (Show $p) + ')')
Save-GenStamp $fp 'x_regen.py' -SpuScript 'x_spu.py'

Write-Host 'stamps from earlier versions (one hash)'
$cur = Get-GenParts $fp 'x_regen.py' 'x_spu.py'
$oldHash = Get-GenInputsHash $fp 'x_regen.py'   # what an earlier Setup wrote for the same files
Check ($oldHash -match '^[0-9a-f]{64}$') 'the one-hash value is still computed the same way'
[IO.File]::WriteAllLines($stamp, [string[]]@($oldHash, '# old format'))
$p = Plan
Check ($p.Ppu -and $p.Spu -and $p.Why -eq 'legacy') ('an old stamp Setup cannot translate: both, files that come out the same keep their date (' + (Show $p) + ')')
$legacy = Join-Path $fp 'tools\gen_stamps_legacy.txt'
[IO.File]::WriteAllLines($legacy, [string[]]@('# old one-hash stamp -> the split stamp it equals', "$oldHash ppu=$($cur.Ppu) spu=-"))
$p = Plan
Check (-not $p.Ppu -and $p.Spu -and $p.Legacy) ('a known old stamp whose PPU code is unchanged: SPU only (' + (Show $p) + ')')
[IO.File]::WriteAllLines($legacy, [string[]]@("$oldHash ppu=$($cur.Ppu) spu=$($cur.Spu)"))
$p = Plan; Check (-not $p.Ppu -and -not $p.Spu -and $p.Legacy) 'a known old stamp equal to the current generator: nothing'
Put 'tools\hooks\x.json' '{"ppu": 3}'
$p = Plan; Check ($p.Ppu -and -not $p.Spu) ('a known old stamp, then the PPU half changed: PPU (' + (Show $p) + ')')

Write-Host 'ports without a separate SPU script (one hash, as before)'
Save-GenStamp $fp 'x_regen.py'
Check ((Get-Content $stamp -TotalCount 1) -match '^[0-9a-f]{64}$') 'the stamp keeps the one-hash format'
Check ($null -eq (Test-GenStale $fp 'x_regen.py')) 'up to date'
Put 'tools\x_spu.py' "from regen_common import run`nIGJOB_EXTRA = {0x1: 3}`n"
Check ((Test-GenStale $fp 'x_regen.py') -eq 'changed') 'any change: changed'
$p = Get-GenPlan $fp 'x_regen.py'
Check ($p.Ppu -and $p.Spu) 'and both halves are generated'
$oldOne = Get-Content $stamp -TotalCount 1
Save-GenStamp $fp 'x_regen.py'; $newOne = Get-Content $stamp -TotalCount 1
[IO.File]::WriteAllLines($stamp, [string[]]@($oldOne))
[IO.File]::WriteAllLines($legacy, [string[]]@("$oldOne same=$newOne"))
$p = Get-GenPlan $fp 'x_regen.py'
Check (-not $p.Ppu -and -not $p.Spu -and $p.Legacy -and $null -eq (Test-GenStale $fp 'x_regen.py')) 'an old stamp listed as writing the same code: nothing generated'
[IO.File]::WriteAllLines($legacy, [string[]]@("$oldOne same=$('0' * 64)"))
Check ((Test-GenStale $fp 'x_regen.py') -eq 'changed') '... listed as the same as another version: changed'
[IO.File]::WriteAllLines($legacy, [string[]]@("$oldOne same=$newOne"))
$p = Get-GenPlan $fp 'x_regen.py' 'x_spu.py'
Check ($p.Ppu -and $p.Spu -and $p.Why -eq 'legacy') 'a same= line does not translate a stamp for the split format'
Remove-GenStamp $fp
Check ((Test-GenStale $fp 'x_regen.py') -eq 'missing') 'no stamp: missing'

Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue
if ($script:Failed) { Write-Host "$($script:Failed) test(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'All generator stamp tests passed.' -ForegroundColor Green
exit 0
