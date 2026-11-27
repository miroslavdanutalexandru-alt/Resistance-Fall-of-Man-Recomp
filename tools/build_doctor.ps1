# Build repair and disk-space helpers for setup.ps1 (the same file in every port). setup.ps1 dot-sources it;
# it also runs on its own:
#   powershell -ExecutionPolicy Bypass -File tools\build_doctor.ps1 -DiskUsage     how much space each part takes
#   powershell -ExecutionPolicy Bypass -File tools\build_doctor.ps1 -ResetConfig   fresh CMake configuration (keeps
#                                                                                  the compiled files), then run build.bat
#   powershell -ExecutionPolicy Bypass -File tools\build_doctor.ps1 -FirstError    the first error in the build logs
param([switch]$DiskUsage, [switch]$ResetConfig, [switch]$FirstError, [string]$PortRoot = (Split-Path -Parent $PSScriptRoot))

# Build folders build.bat configures (R1: build, build_game; R2/R3: build_rpcs3, build) and its logs, in build order.
$script:BdDirs = @('build_rpcs3', 'build', 'build_game')
$script:BdLogs = @('configure_rpcs3', 'build_rpcs3', 'configure', 'build', 'configure_game', 'build_game', 'build_launcher')

function Get-ConfiguredBuildDirs([string]$Port) {
    foreach ($d in $script:BdDirs) {
        $p = Join-Path $Port $d
        if ((Test-Path -LiteralPath (Join-Path $p 'CMakeCache.txt')) -or (Test-Path -LiteralPath (Join-Path $p 'build.ninja'))) { $p }
    }
}

# Throws away CMake's configuration of the build folders but keeps the compiled files (CMakeFiles\*.dir) and Ninja's
# .ninja_log / .ninja_deps, so the next build.bat configures again and only recompiles what really changed. Fixes
# "manifest 'build.ninja' still dirty after 100 tries", "rebuilding 'build.ninja': subcommand failed", a folder that
# was moved or renamed ("CMakeCache.txt directory ... is different") and a changed generator or compiler.
function Reset-BuildConfig([string]$Port) {
    foreach ($b in @(Get-ConfiguredBuildDirs $Port)) {
        foreach ($f in 'build.ninja', 'CMakeCache.txt', 'CMakeFiles\cmake.check_cache', 'CMakeFiles\VerifyGlobs.cmake', 'CMakeFiles\cmake.verify_globs', 'CMakeFiles\rules.ninja') {
            Remove-Item -LiteralPath (Join-Path $b $f) -Force -ErrorAction SilentlyContinue
        }
        # compiler checks (CMakeFiles\<cmake version>\), redone by the next configure
        Get-ChildItem -LiteralPath (Join-Path $b 'CMakeFiles') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d+\.\d+' } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        # Ninja's record of build.ninja itself (a time from the future there keeps the manifest "dirty")
        $log = Join-Path $b '.ninja_log'
        if (Test-Path -LiteralPath $log) {
            try {
                $lines = [IO.File]::ReadAllLines($log)
                $keep = @($lines | Where-Object { $_.StartsWith('#') -or ($_ -split "`t")[3] -ne 'build.ninja' })
                if ($keep.Count -ne $lines.Count) { [IO.File]::WriteAllLines($log, [string[]]$keep) }
            } catch { }
        }
        Write-Host "   Reset the build configuration in $(Split-Path -Leaf $b)\ (the compiled files are kept)."
    }
}

# A build folder configured for another path (the port folder was moved, renamed or copied) cannot be reused as is.
function Repair-MovedBuildDirs([string]$Port) {
    foreach ($b in @(Get-ConfiguredBuildDirs $Port)) {
        $cache = Join-Path $b 'CMakeCache.txt'
        if (-not (Test-Path -LiteralPath $cache)) { continue }
        $was = Select-String -LiteralPath $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $was) { continue }
        $was = $was.Matches[0].Groups[1].Value.Trim().Replace('/', '\').TrimEnd('\')
        if ($was -and $was -ne [IO.Path]::GetFullPath($b).TrimEnd('\')) {
            Write-Host "   $(Split-Path -Leaf $b)\ was set up in $was (the folder moved): setting it up again for this folder."
            Reset-BuildConfig $Port
            return
        }
    }
}

function Get-BuildLogs([string]$Port, [datetime]$Since) {
    foreach ($n in $script:BdLogs) {
        $f = Join-Path $Port "logs\$n.log"
        if ((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).LastWriteTime -ge $Since) { $f }
    }
}

# The first real error of the last build: "<log>:<line>: <text>" plus up to 2 lines that follow it, or $null.
function Get-FirstBuildError([string]$Port, [datetime]$Since = [datetime]::MinValue) {
    $strong = '^ninja: error:|CMake Error|: (fatal )?error( [A-Z]{1,4}\d+)?:|^\s*error( [A-Z]{1,4}\d+)?:|LINK : fatal|lld-link: error|clang-cl: error|ninja: build stopped: subcommand failed|is not recognized as an internal or external command'
    foreach ($f in @(Get-BuildLogs $Port $Since)) {
        $lines = @(Get-Content -LiteralPath $f -ErrorAction SilentlyContinue)
        $hit = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match $strong -and $lines[$i] -notmatch 'ninja: build stopped') { $hit = $i; break }
        }
        if ($hit -lt 0) { for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^FAILED: |ninja: build stopped|Configure FAILED') { $hit = $i; break } } }
        if ($hit -ge 0) {
            $out = @("logs\$(Split-Path -Leaf $f) line $($hit + 1):")
            for ($i = $hit; $i -lt [math]::Min($lines.Count, $hit + 3); $i++) {
                $t = $lines[$i].Trim(); if ($t.Length -gt 300) { $t = $t.Substring(0, 300) + '...' }
                if ($t) { $out += "  $t" }
            }
            return ($out -join "`n")
        }
    }
    return $null
}

# Problems a fresh configuration fixes (as opposed to an error in the code, which fails again the same way).
function Test-BuildConfigProblem([string]$Port, [datetime]$Since) {
    $pat = 'still dirty after|manifest .build\.ninja.|rebuilding .build\.ninja.|loading .build\.ninja.|^FAILED: build\.ninja|CMakeCache\.txt directory .* is different|does not match the (generator|platform|toolset) used previously|re-run cmake|CMAKE_(C|CXX)_COMPILER.*(changed|not found)|Configure FAILED|CMake Error|Error: could not load cache'
    foreach ($f in @(Get-BuildLogs $Port $Since)) {
        if (Select-String -LiteralPath $f -Pattern $pat -Quiet) { return $true }
    }
    return $false
}
function Test-BuildCodeError([string]$Port, [datetime]$Since) {
    foreach ($f in @(Get-BuildLogs $Port $Since)) {
        if (Select-String -LiteralPath $f -Pattern '^FAILED: .*\.(obj|o|exe|dll|lib)\b' -Quiet) { return $true }
    }
    return $false
}

function Get-TreeBytes([string]$Path) {
    $sum = [int64]0
    $stack = New-Object System.Collections.Stack; $stack.Push($Path)
    while ($stack.Count) {
        $d = $stack.Pop()
        try { foreach ($f in [IO.Directory]::EnumerateFiles($d)) { try { $sum += ([IO.FileInfo]$f).Length } catch { } } } catch { }
        try {
            foreach ($s in [IO.Directory]::EnumerateDirectories($d)) {
                if (([IO.File]::GetAttributes($s) -band [IO.FileAttributes]::ReparsePoint) -eq 0) { $stack.Push($s) }
            }
        } catch { }
    }
    return $sum
}

# Leftovers a finished setup no longer needs:
#  - build_regen\: the code generator's work folder; the result is copied to recompiled*\ and project\spu\, and
#    the generator starts it from scratch every time (R1 ~0.3 GB, R2 ~1.4 GB, R3 ~3.9 GB).
#  - gamefiles\PS3_CONTENT (R3: 2.5 GB of bonus videos for the PS3 menu) and gamefiles\PS3_UPDATE (the disc's PS3
#    system update): the game never reads them. New copies skip them (tools\installer\gamefiles.ps1).
#  - Setup's downloads in %TEMP% left by an interrupted run.
# Not removed: the build folders (their compiled files make updates fast), recompiled*\ (the build's sources) and
# your game files.
function Remove-SetupLeftovers([string]$Port, [string]$TitleId = '') {
    $freed = [int64]0
    $items = @((Join-Path $Port 'build_regen'), (Join-Path $Port 'gamefiles\PS3_CONTENT'), (Join-Path $Port 'gamefiles\PS3_UPDATE'))
    if ($TitleId -and $env:TEMP) { $items += @(Get-ChildItem -LiteralPath $env:TEMP -Filter "$($TitleId)_*" -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }) }
    foreach ($p in $items) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $n = if ((Get-Item -LiteralPath $p -Force).PSIsContainer) { Get-TreeBytes $p } else { (Get-Item -LiteralPath $p -Force).Length }
        Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $p)) { $freed += $n }
    }
    if ($freed -ge 50MB) { Write-Host ("   Removed setup leftovers that are no longer needed: {0:N1} GB freed." -f ($freed / 1GB)) }
}

# Where the space in the port folder goes. Large folders setup does not make are listed separately: an extra copy of
# the game or a second, nested copy of the port (an update unpacked into a subfolder) shows up there.
function Show-DiskUsage([string]$Port) {
    $known = [ordered]@{
        'Your game files (gamefiles)'                 = @('gamefiles')
        'Compiled game (build folders, kept so updates rebuild only what changed)' = @('build', 'build_game', 'build_rpcs3')
        'Generated game code (recompiled)'            = @('recompiled', 'recompiled_game', 'project')
        'Code generator work folder (build_regen)'    = @('build_regen')
        'Port and RPCS3 source (ps3recomp)'           = @('ps3recomp')
        'Tools and SDKs (tools)'                      = @('tools')
        'Saves, decrypted program, shader cache'      = @('gamedata', 'game', 'shadercache')
        'Logs'                                        = @('logs')
    }
    $seen = @{}
    $rows = @()
    foreach ($k in $known.Keys) {
        $b = [int64]0
        foreach ($d in $known[$k]) { $p = Join-Path $Port $d; $seen[$d] = $true; if (Test-Path -LiteralPath $p) { $b += Get-TreeBytes $p } }
        $rows += [pscustomobject]@{ What = $k; Bytes = $b }
    }
    $other = [int64]0; $odd = @()
    foreach ($d in (Get-ChildItem -LiteralPath $Port -Force -ErrorAction SilentlyContinue)) {
        if ($seen.ContainsKey($d.Name)) { continue }
        if (-not $d.PSIsContainer) { $other += $d.Length; continue }
        if (($d.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
        $b = Get-TreeBytes $d.FullName
        $nested = (Test-Path -LiteralPath (Join-Path $d.FullName 'setup.ps1')) -or (Test-Path -LiteralPath (Join-Path $d.FullName 'gamefiles'))
        if ($b -ge 1GB -or $nested) { $odd += [pscustomobject]@{ Name = $d.Name; Bytes = $b; Nested = $nested } } else { $other += $b }
    }
    $rows += [pscustomobject]@{ What = 'Everything else'; Bytes = $other }
    $total = ($rows | Measure-Object Bytes -Sum).Sum + ($odd | Measure-Object Bytes -Sum).Sum
    Write-Host ("   Disk space used by {0}: {1:N1} GB" -f $Port, ($total / 1GB))
    foreach ($r in $rows) { if ($r.Bytes -ge 50MB) { Write-Host ("     {0,7:N1} GB  {1}" -f ($r.Bytes / 1GB), $r.What) } }
    foreach ($o in $odd) {
        $why = if ($o.Nested) { 'looks like a second copy of the port (an update unpacked into a subfolder?) - safe to delete if you do not use it' } else { 'not made by Setup' }
        Write-Host ("     {0,7:N1} GB  {1}\  <- {2}" -f ($o.Bytes / 1GB), $o.Name, $why) -ForegroundColor Yellow
    }
    try { $free = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Port)).AvailableFreeSpace; Write-Host ("   Free on {0}: {1:N1} GB" -f [IO.Path]::GetPathRoot($Port), ($free / 1GB)) } catch { }
}

# ---------------------------------------------------------------- code generator version
# Setup generates the game code once and reuses it; an update that changes the generator (its scripts, the port's
# hooks, the lifter in ps3recomp\tools) has to generate it again. The generator's input files are found by
# following the regen script's imports and the script/json names it mentions, plus everything in tools\hooks.
# A port whose SPU half is its own script (R3: tools\r3_spu.py) keeps two hashes in the stamp ("v2"): one for the
# PPU code (recompiled\, 10-25 minutes to generate and most of the build) and one for the SPU code (project\spu\,
# about a minute), so a change to the SPU half only regenerates (and rebuilds) the SPU code.
$script:GenStampName = '.generator_inputs'
# Python tools of the SPU half only: regen_common.py names them (spu_post, for R2), but the PPU code never uses them.
$script:GenSpuToolPattern = '^spu_'

function Get-GenInputFiles([string]$Port, [string]$Script, [string[]]$Skip = @(), [string]$SkipPattern = '',
                           [ValidateSet('all', 'nospu', 'spu', 'none')][string]$Hooks = 'all') {
    $dirs = @((Join-Path $Port 'tools'), (Join-Path $Port 'ps3recomp\tools'), (Join-Path $Port 'tools\hooks'))
    $skipSet = @{}; foreach ($k in $Skip) { if ($k) { $skipSet[$k.ToLowerInvariant()] = 1 } }
    $seen = @{}
    $queue = New-Object System.Collections.Queue
    $first = Join-Path $Port "tools\$Script"
    if (-not (Test-Path -LiteralPath $first)) { return @() }
    $seen[$first.ToLowerInvariant()] = $first; $queue.Enqueue($first)
    while ($queue.Count) {
        $f = $queue.Dequeue()
        $text = [IO.File]::ReadAllText($f)
        $names = New-Object System.Collections.Generic.List[string]
        foreach ($m in [regex]::Matches($text, '(?m)^\s*from\s+([A-Za-z_][\w\.]*)\s+import')) { $names.Add(($m.Groups[1].Value -replace '\.', '\') + '.py') }
        foreach ($m in [regex]::Matches($text, '(?m)^\s*import\s+([A-Za-z_][\w\., ]*)')) {
            foreach ($n in $m.Groups[1].Value -split ',') { $n = ($n.Trim() -split '\s+')[0]; if ($n) { $names.Add(($n -replace '\.', '\') + '.py') } }
        }
        foreach ($m in [regex]::Matches($text, '[''"]([\w\-\.]+\.(?:py|json))[''"]')) { $names.Add($m.Groups[1].Value) }
        foreach ($n in $names) {
            $leaf = (Split-Path -Leaf $n).ToLowerInvariant()
            if ($skipSet.ContainsKey($leaf) -or ($SkipPattern -and $leaf -match $SkipPattern)) { continue }
            foreach ($d in $dirs) {
                $p = Join-Path $d $n
                if (-not $seen.ContainsKey($p.ToLowerInvariant()) -and [IO.File]::Exists($p)) {
                    $seen[$p.ToLowerInvariant()] = $p
                    if ($p -like '*.py') { $queue.Enqueue($p) }
                    break
                }
            }
        }
    }
    $hk = Join-Path $Port 'tools\hooks'
    if ($Hooks -ne 'none' -and (Test-Path -LiteralPath $hk)) {
        $spuHk = (Join-Path $hk 'spu').ToLowerInvariant() + '\'
        Get-ChildItem -LiteralPath $hk -Recurse -File -Force | Where-Object { $_.Extension -in '.json', '.py', '.c', '.cpp', '.h', '.hpp', '.inc', '.txt' } |
            Where-Object { $inSpu = $_.FullName.ToLowerInvariant().StartsWith($spuHk); $Hooks -eq 'all' -or ($Hooks -eq 'spu' -and $inSpu) -or ($Hooks -eq 'nospu' -and -not $inSpu) } |
            ForEach-Object { $seen[$_.FullName.ToLowerInvariant()] = $_.FullName }
    }
    $seen.Values | Sort-Object { $_.ToLowerInvariant() }
}

# One hash over a list of files (relative names + contents, line endings ignored so a git checkout and a release
# zip give the same value).
function Get-GenFilesHash([string]$Port, [string[]]$Files) {
    if (-not $Files -or -not $Files.Count) { return $null }
    $sha = [Security.Cryptography.SHA256]::Create()
    $ms = New-Object IO.MemoryStream
    $root = [IO.Path]::GetFullPath($Port).TrimEnd('\') + '\'
    foreach ($f in $Files) {
        $b = [Text.Encoding]::UTF8.GetBytes($f.Substring($root.Length).ToLowerInvariant().Replace('\', '/') + "`n")
        $ms.Write($b, 0, $b.Length)
        $b = [Text.Encoding]::UTF8.GetBytes([IO.File]::ReadAllText($f).Replace("`r`n", "`n") + "`n")
        $ms.Write($b, 0, $b.Length)
    }
    $h = $sha.ComputeHash($ms.ToArray())
    -join ($h | ForEach-Object { $_.ToString('x2') })
}

# The one-hash stamp (ports without a separate SPU script, and the format before the split).
function Get-GenInputsHash([string]$Port, [string]$Script) { Get-GenFilesHash $Port @(Get-GenInputFiles $Port $Script) }

# The two halves of a split generator: PPU = the regen script and what it uses, without the SPU script, the SPU-only
# tools and tools\hooks\spu; SPU = the SPU script and what it uses (shared helpers count for both), tools\hooks\spu
# and the regen script itself (it drives both halves).
function Get-GenParts([string]$Port, [string]$Script, [string]$SpuScript) {
    $pf = @(Get-GenInputFiles $Port $Script -Skip @($SpuScript) -SkipPattern $script:GenSpuToolPattern -Hooks nospu)
    $sf = @(Get-GenInputFiles $Port $SpuScript -Hooks spu)
    $drv = Join-Path $Port "tools\$Script"
    if ((Test-Path -LiteralPath $drv) -and -not @($sf | Where-Object { $_ -eq $drv }).Count) { $sf = @($sf + $drv | Sort-Object { $_.ToLowerInvariant() }) }
    [pscustomobject]@{ PpuFiles = $pf; SpuFiles = $sf; Ppu = (Get-GenFilesHash $Port $pf); Spu = (Get-GenFilesHash $Port $sf) }
}

# What the stamp says: Format 'none' / 'v1' (Hash) / 'v2' (Ppu, Spu; '-' = that half was being generated and did not finish).
function Read-GenStamp([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return [pscustomobject]@{ Format = 'none'; Hash = $null; Ppu = $null; Spu = $null } }
    $lines = @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)
    if ($lines.Count -and $lines[0].Trim() -eq 'v2') {
        $o = [pscustomobject]@{ Format = 'v2'; Hash = $null; Ppu = $null; Spu = $null }
        foreach ($l in $lines) { if ($l -match '^(ppu|spu)\s+(\S+)') { $o.($Matches[1]) = $Matches[2] } }
        return $o
    }
    [pscustomobject]@{ Format = 'v1'; Hash = $(if ($lines.Count) { $lines[0].Trim() } else { '' }); Ppu = $null; Spu = $null }
}

# One-hash stamps written by earlier versions, and the split stamp each one is known to equal (tools\gen_stamps_legacy.txt,
# "<old hash> ppu=<hash>|- spu=<hash>|-"; '-' = that half has to be generated again). Made when the split was introduced,
# from the generator inputs of each released version: only lines for versions whose PPU code is known to be identical.
# Ports with one hash: "<old hash> same=<hash>" = that generator writes exactly the same code as the one with <hash>
# (a change that only touched how it reports progress or writes files), so nothing is generated again.
function Get-GenLegacyMap([string]$Port) {
    $map = @{}
    $f = Join-Path $Port 'tools\gen_stamps_legacy.txt'
    if (-not (Test-Path -LiteralPath $f)) { return $map }
    foreach ($l in Get-Content -LiteralPath $f) {
        if ($l -match '^\s*([0-9a-f]{64})\s+ppu=(\S+)\s+spu=(\S+)') { $map[$Matches[1]] = [pscustomobject]@{ Ppu = $Matches[2]; Spu = $Matches[3]; Same = $null } }
        elseif ($l -match '^\s*([0-9a-f]{64})\s+same=([0-9a-f]{64})') { $map[$Matches[1]] = [pscustomobject]@{ Ppu = $null; Spu = $null; Same = $Matches[2] } }
    }
    $map
}

# What has to be generated again: Ppu / Spu ($true = generate that half), Why ($null = nothing; 'missing' = no stamp;
# 'changed' = the generator changed; 'legacy' = a one-hash stamp from an earlier version that is not in the legacy map,
# so both halves are generated again - files that come out the same keep their date, so the build only compiles what
# really changed), Legacy ($true when an old one-hash stamp was translated).
function Get-GenPlan([string]$Port, [string]$Script, [string]$SpuScript = '', [string]$StampDir = 'recompiled') {
    $st = Read-GenStamp (Join-Path $Port "$StampDir\$($script:GenStampName)")
    $plan = [pscustomobject]@{ Ppu = $false; Spu = $false; Why = $null; Legacy = $false; Parts = $null }
    if ($st.Format -eq 'none') { $plan.Ppu = $plan.Spu = $true; $plan.Why = 'missing'; return $plan }
    if (-not $SpuScript) {
        $new = Get-GenInputsHash $Port $Script
        if ($new -and $st.Hash -ne $new) {
            $m = (Get-GenLegacyMap $Port)[$st.Hash]
            if ($m -and $m.Same -eq $new) { $plan.Legacy = $true } else { $plan.Ppu = $plan.Spu = $true; $plan.Why = 'changed' }
        }
        return $plan
    }
    $cur = Get-GenParts $Port $Script $SpuScript
    $plan.Parts = $cur
    $old = $st
    if ($st.Format -eq 'v1') {
        $m = (Get-GenLegacyMap $Port)[$st.Hash]
        if (-not $m -or -not $m.Ppu) { $plan.Ppu = $plan.Spu = $true; $plan.Why = 'legacy'; return $plan }
        $old = $m; $plan.Legacy = $true
    }
    $plan.Ppu = [bool]($cur.Ppu -and $old.Ppu -ne $cur.Ppu)
    $plan.Spu = [bool]($cur.Spu -and $old.Spu -ne $cur.Spu)
    if ($plan.Ppu -or $plan.Spu) { $plan.Why = 'changed' }
    $plan
}

# $null when the generated code matches the generator, 'missing' when it was made by a version without the
# stamp (or the stamp was lost), 'changed' when the generator changed since.
function Test-GenStale([string]$Port, [string]$Script, [string]$StampDir = 'recompiled', [string]$SpuScript = '') {
    $p = Get-GenPlan $Port $Script $SpuScript $StampDir
    if (-not ($p.Ppu -or $p.Spu)) { return $null }
    if ($p.Why -eq 'missing') { return 'missing' }
    'changed'
}

# -PpuPending / -SpuPending: that half is about to be generated (written as '-', so a run that stops half way
# generates it again next time); without them the stamp says both halves match the generator.
function Save-GenStamp([string]$Port, [string]$Script, [string]$StampDir = 'recompiled', [string]$SpuScript = '',
                       [switch]$PpuPending, [switch]$SpuPending) {
    $dir = Join-Path $Port $StampDir
    if (-not (Test-Path -LiteralPath $dir)) { return }
    $root = [IO.Path]::GetFullPath($Port).TrimEnd('\') + '\'
    if (-not $SpuScript) {
        $h = Get-GenInputsHash $Port $Script
        if (-not $h) { return }
        $files = @(Get-GenInputFiles $Port $Script)
        $lines = @($h, "# Hash of the code generator's input files when this code was generated (Setup regenerates when it changes):") +
                 @($files | ForEach-Object { '#   ' + $_.Substring($root.Length) })
    } else {
        $p = Get-GenParts $Port $Script $SpuScript
        if (-not $p.Ppu -or -not $p.Spu) { return }
        $pp = if ($PpuPending) { '-' } else { $p.Ppu }
        $sp = if ($SpuPending) { '-' } else { $p.Spu }
        $lines = @('v2', "ppu $pp", "spu $sp",
                   "# Hashes of the code generator's input files when this code was generated; Setup regenerates the PPU code",
                   "# (recompiled\) or the SPU code (project\spu\) when its half changes. PPU inputs:") +
                 @($p.PpuFiles | ForEach-Object { '#   ' + $_.Substring($root.Length) }) + @('# SPU inputs:') +
                 @($p.SpuFiles | ForEach-Object { '#   ' + $_.Substring($root.Length) })
    }
    [IO.File]::WriteAllLines((Join-Path $dir $script:GenStampName), [string[]]$lines)
}

function Remove-GenStamp([string]$Port, [string]$StampDir = 'recompiled') {
    Remove-Item -LiteralPath (Join-Path $Port "$StampDir\$($script:GenStampName)") -Force -ErrorAction SilentlyContinue
}

function Get-FileMd5([string]$Path) {
    $md5 = [Security.Cryptography.MD5]::Create()
    $s = [IO.File]::OpenRead($Path)
    try { [BitConverter]::ToString($md5.ComputeHash($s)) } finally { $s.Dispose() }
}

# Regenerating rewrites every generated file. Files whose content did not change get their old date back
# afterwards, so the build only compiles what the new generator really changed (not the whole game again).
function Save-GenSnapshot([string]$Port, [string[]]$Dirs) {
    $snap = @{}
    foreach ($d in $Dirs) {
        $p = Join-Path $Port $d
        if (-not (Test-Path -LiteralPath $p)) { continue }
        foreach ($f in Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue) {
            if ($f.Name -eq $script:GenStampName) { continue }
            $snap[$f.FullName.ToLowerInvariant()] = @{ Len = $f.Length; Time = $f.LastWriteTimeUtc; Hash = (Get-FileMd5 $f.FullName) }
        }
    }
    $snap
}

function Restore-GenTimes([string]$Port, [string[]]$Dirs, $Snap) {
    $kept = 0; $changed = 0
    foreach ($d in $Dirs) {
        $p = Join-Path $Port $d
        if (-not (Test-Path -LiteralPath $p)) { continue }
        foreach ($f in Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue) {
            if ($f.Name -eq $script:GenStampName) { continue }
            $o = $Snap[$f.FullName.ToLowerInvariant()]
            if ($o -and $o.Len -eq $f.Length -and $o.Hash -eq (Get-FileMd5 $f.FullName)) {
                if ($f.LastWriteTimeUtc -ne $o.Time) { try { $f.LastWriteTimeUtc = $o.Time } catch { } }
                $kept++
            } else { $changed++ }
        }
    }
    [pscustomobject]@{ Kept = $kept; Changed = $changed }
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($ResetConfig) {
        Reset-BuildConfig $PortRoot
        & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fix_timestamps.ps1') -Root $PortRoot
        Write-Host "   Now run build.bat (or Setup) again."
    }
    if ($FirstError) { $e = Get-FirstBuildError $PortRoot; if ($e) { Write-Host $e } else { Write-Host "   No error found in logs\." } }
    if ($DiskUsage -or -not ($ResetConfig -or $FirstError)) { Show-DiskUsage $PortRoot }
}
