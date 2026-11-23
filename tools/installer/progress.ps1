# Progress, time estimates and step timings for setup.ps1 (the same file in every port; setup.ps1 dot-sources it).
#
# - The code generator prints "@@PROGRESS stage=<name> done=<n> total=<n>" lines; Invoke-SpRegen turns them into a
#   fraction of the whole generation, weighted by how long each stage took on this PC the last time.
# - The build is weighted by the size of the generated PPU source files still to compile (a 200 MB file takes far
#   longer than 400 small ones together, so Ninja's step count says little); the files compiling right now and how
#   long they have been at it come from the running clang-cl processes.
# - One overall bar over all steps, weighted by their durations from logs\setup_timings.json (the last run on this
#   PC), else by defaults measured on the PC the port was made on.
# - Setup.exe sets PORT_SETUP_PROGRESS=1 and reads "@@SETUP pct=<0..1000> eta=<s> idle=<s> <text>" lines (not
#   logged); otherwise a console window gets Write-Progress bars and everyone gets a readable line every minute.

$script:SpEmit = ($env:PORT_SETUP_PROGRESS -eq '1')
$script:SpInteractive = $true
$script:SpPort = $null
$script:SpLog = $null
$script:SpOld = $null
$script:SpRun = @{ steps = @{}; regen = @{}; build = @{} }
$script:SpSteps = New-Object System.Collections.Specialized.OrderedDictionary
$script:SpCur = $null
$script:SpCurStart = $null
$script:SpCurFrac = 0.0
$script:SpShown = 0.0
$script:SpLastEmit = [datetime]::MinValue
$script:SpLastHuman = [datetime]::MinValue

# Durations in seconds measured on the PC the port was made on (16 threads, 32 GB, NVMe) for a first install.
$script:SpDefaults = @{
    steps = @{ check = 15; tools = 60; game = 600; decrypt = 20; gen = 1000; deps = 120; rpcs3 = 600; game_build = 2700; finish = 60 }
    regen = [ordered]@{ ppu_analyze = 40; ppu_lift = 300; ppu_emit = 40; ppu_post = 620; ppu_hooks = 20; ppu_copy = 30
                        spu_lift = 15; spu_jobmods = 5; spu_post = 8; spu_copy = 2 }
    ppu_job_bytes_per_sec = 0.7e6 # generated PPU source compiled per second by one compile job (for files of unknown time)
    speed = 1.0                   # real / predicted compile time on this PC (measured by the last build)
    build_other_sec = 240         # the rest of a first game build (runtime, SPU modules, link) beyond the PPU files
}

function Format-SpDuration([double]$Sec) {
    if ($Sec -lt 0 -or [double]::IsNaN($Sec) -or [double]::IsInfinity($Sec)) { return '?' }
    $s = [int][math]::Round($Sec)
    if ($s -ge 3600) { return '{0}:{1:00}:{2:00}' -f [int][math]::Floor($s / 3600), [int][math]::Floor(($s % 3600) / 60), ($s % 60) }
    '{0}:{1:00}' -f [int][math]::Floor($s / 60), ($s % 60)
}
function Format-SpEta([double]$Sec) {
    if ($Sec -lt 0 -or [double]::IsNaN($Sec) -or [double]::IsInfinity($Sec)) { return '' }
    if ($Sec -lt 60) { return 'less than a minute left' }
    $m = [int][math]::Ceiling($Sec / 60)
    if ($m -lt 90) { return "about $m min left" }
    'about {0}:{1:00} h left' -f [int][math]::Floor($m / 60), ($m % 60)
}

# ---------------------------------------------------------------- timings file
function ConvertTo-SpHashtable($o) {
    if ($null -eq $o) { return $null }
    if ($o -is [System.Collections.IDictionary]) { return $o }
    if ($o -is [psobject] -and $o.PSObject.Properties) {
        $h = @{}
        foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = if ($p.Value -is [psobject] -and -not ($p.Value -is [string]) -and -not ($p.Value -is [ValueType])) { ConvertTo-SpHashtable $p.Value } else { $p.Value } }
        return $h
    }
    $o
}
function Read-SpTimings([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    try { $o = ConvertTo-SpHashtable ([IO.File]::ReadAllText($Path) | ConvertFrom-Json) } catch { return $null }
    if (-not $o -or $o['version'] -ne 1) { return $null }
    foreach ($k in 'steps', 'regen', 'build') { if (-not ($o[$k] -is [System.Collections.IDictionary])) { $o[$k] = @{} } }
    $o
}
# A number from logs\setup_timings.json: written with the invariant culture ("12.5"); a file written by a build that
# used the PC's culture ("12,5" on ro-RO and similar) is read too. $null when it is not a number.
function ConvertTo-SpNumber($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [double] -or $Value -is [int] -or $Value -is [long] -or $Value -is [decimal]) { return [double]$Value }
    $v = 0.0
    if ([double]::TryParse(([string]$Value).Trim().Replace(',', '.'), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { return $v }
    $null
}
# A file's compile time for logs\setup_timings.json: "<seconds> <bytes>", always with a '.' decimal point.
function Format-SpFileTime([double]$Sec, [double]$Bytes) {
    [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0} {1}', [math]::Round($Sec, 1), [long]$Bytes)
}
# The value measured last time on this PC, else the default.
function Get-SpTime([string]$Group, [string]$Key) {
    if ($script:SpOld -and $script:SpOld[$Group] -and $null -ne $script:SpOld[$Group][$Key]) {
        $v = ConvertTo-SpNumber $script:SpOld[$Group][$Key]; if ($null -ne $v -and $v -gt 0) { return $v }
    }
    if ($Group -eq 'build') { return [double]$script:SpDefaults[$Key] }
    $d = $script:SpDefaults[$Group][$Key]
    if ($null -eq $d) { return 0.0 }
    [double]$d
}
# Only what really ran this time replaces the old values (a skipped step keeps last time's duration).
function Save-SpTimings {
    if (-not $script:SpLog) { return }
    $o = if ($script:SpOld) { $script:SpOld } else { @{ steps = @{}; regen = @{}; build = @{} } }
    foreach ($g in 'steps', 'regen', 'build') { foreach ($k in @($script:SpRun[$g].Keys)) { $o[$g][$k] = [math]::Round([double]$script:SpRun[$g][$k], 3) } }
    if ($script:SpRun['files']) {
        if (-not ($o['files'] -is [System.Collections.IDictionary])) { $o['files'] = @{} }
        foreach ($k in @($script:SpRun['files'].Keys)) { $o['files'][$k] = $script:SpRun['files'][$k] }
    }
    $o['version'] = 1
    $o['updated'] = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    try { [IO.File]::WriteAllText($script:SpLog, ($o | ConvertTo-Json -Depth 4)) } catch { }
}
function Write-SpTimingSummary {
    $parts = foreach ($k in @($script:SpRun.steps.Keys)) { '{0} {1}' -f $k, (Format-SpDuration $script:SpRun.steps[$k]) }
    if ($parts) { Write-Host ("   Step timings: " + ($parts -join ', ')) }
}

# ---------------------------------------------------------------- overall bar
# $Estimates: step key -> estimated seconds (ordered: the order of the steps). Done steps count fully, the current
# one by its fraction; skipped steps simply finish quickly. The bar never goes backwards.
function Get-SpOverall($Estimates, [string[]]$DoneKeys, [string]$CurKey, [double]$CurFrac) {
    $tot = 0.0; $got = 0.0
    foreach ($k in @($Estimates.Keys)) {
        $w = [math]::Max(1.0, [double]$Estimates[$k])
        $tot += $w
        if ($DoneKeys -contains $k) { $got += $w } elseif ($k -eq $CurKey) { $got += $w * [math]::Min(1.0, [math]::Max(0.0, $CurFrac)) }
    }
    if ($tot -le 0) { return 0.0 }
    $got / $tot
}
# Ports whose build.bat builds RPCS3's renderer first (R2, R3; R1 has its own renderer).
function Test-SpHasRpcs3([string]$Port) {
    $b = Join-Path $Port 'build.bat'
    (Test-Path -LiteralPath $b) -and ([IO.File]::ReadAllText($b) -match 'build_rpcs3')
}
function Initialize-SpProgress([string]$Port, [switch]$Unattended) {
    $script:SpPort = $Port
    $script:SpInteractive = -not $Unattended
    $script:SpLog = Join-Path $Port 'logs\setup_timings.json'
    $script:SpOld = Read-SpTimings $script:SpLog
    foreach ($k in 'check', 'tools', 'game', 'decrypt', 'gen', 'build') { $script:SpSteps[$k] = (Get-SpTime 'steps' $k) }
    $measured = $script:SpOld -and $script:SpOld['steps'] -is [System.Collections.IDictionary] -and $script:SpOld['steps'].Contains('game_build')
    $def = Join-Path $Port 'tools\installer\ppu_compile_times.txt'
    if (-not $measured -and (Test-Path -LiteralPath $def)) {
        # first install on this PC: the port's own build time (its PPU files on 2 jobs, plus the rest) beats one default for all ports
        $sum = 0.0
        foreach ($l in [IO.File]::ReadAllLines($def)) { if ($l -match '^\s*[^#\s]\S*\s+([\d\.]+)') { $sum += [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) } }
        if ($sum -gt 0) { $script:SpDefaults['steps']['game_build'] = [math]::Round($sum / 2 + $script:SpDefaults['build_other_sec']) }
    }
    if (-not (Test-SpHasRpcs3 $Port)) { $script:SpDefaults['steps']['rpcs3'] = 0 }
    $script:SpSteps['build'] = (Get-SpTime 'steps' 'deps') + (Get-SpTime 'steps' 'rpcs3') + (Get-SpTime 'steps' 'game_build') + (Get-SpTime 'steps' 'finish')
}
function Set-SpEstimate([string]$Key, [double]$Sec) { if ($script:SpSteps.Contains($Key)) { $script:SpSteps[$Key] = [math]::Max(1.0, $Sec) } }
# Step key from setup.ps1's step title.
function Get-SpStepKey([string]$Title) {
    switch -regex ($Title) {
        '^Checking' { return 'check' }
        '^1/\d' { return 'tools' }
        '^2/\d' { return 'game' }
        '^3/\d' { return 'decrypt' }
        '^4/\d' { return 'gen' }
        '^5/\d' { return 'build' }
    }
    $null
}
function Enter-SpStep([string]$Title) {
    $k = Get-SpStepKey $Title
    Complete-SpStep
    if (-not $k) { return }
    $script:SpCur = $k; $script:SpCurStart = Get-Date; $script:SpCurFrac = 0.0
}
function Complete-SpStep {
    if (-not $script:SpCur) { return }
    $sec = ((Get-Date) - $script:SpCurStart).TotalSeconds
    $script:SpRun.steps[$script:SpCur] = $sec
    Write-Host ("   ({0} took {1})" -f $script:SpCur, (Format-SpDuration $sec))
    $script:SpCur = $null
}
function Get-SpDoneKeys {
    $done = @()
    foreach ($k in @($script:SpSteps.Keys)) { if ($k -eq $script:SpCur) { break }; $done += $k }
    $done
}
# One progress update: Text = what happens now, Frac = fraction of the current step, Eta = seconds left in it (-1: unknown),
# Idle = seconds since the last sign of life. -Human: also print a readable line now.
function Write-SpStatus([string]$Text, [double]$Frac = -1, [double]$Eta = -1, [double]$Idle = -1, [switch]$Human) {
    if ($Frac -ge 0) { $script:SpCurFrac = $Frac }
    $all = Get-SpOverall $script:SpSteps (Get-SpDoneKeys) $script:SpCur $script:SpCurFrac
    if ($all -gt $script:SpShown) { $script:SpShown = $all }
    $now = Get-Date
    $note = $Text
    if ($Eta -ge 0) { $note += ', ' + (Format-SpEta $Eta) }
    if ($Idle -ge 10) { $note += (' - still working, last activity {0} s ago' -f [int]$Idle) }
    if ($script:SpEmit) {
        if (($now - $script:SpLastEmit).TotalMilliseconds -ge 900) {
            $script:SpLastEmit = $now
            $line = '@@SETUP pct={0} eta={1} idle={2} {3}' -f [int]([math]::Min(1.0, $script:SpShown) * 1000), [int]$Eta, [int]$Idle, $note
            try { [Console]::Out.WriteLine($line); [Console]::Out.Flush() } catch { }
        }
    } elseif ($script:SpInteractive) {
        $ProgressPreference = 'Continue'
        Write-Progress -Id 1 -Activity 'Setup' -Status ('{0:0}% overall' -f ($script:SpShown * 100)) -PercentComplete ([int]([math]::Min(1.0, $script:SpShown) * 100))
        if ($Frac -ge 0) { Write-Progress -Id 2 -ParentId 1 -Activity $Text -Status $note -PercentComplete ([int]([math]::Min(1.0, $Frac) * 100)) }
    }
    # a readable line every minute (Setup.exe without @@SETUP support shows it; it also goes to the logs)
    if ($Human -or ($now - $script:SpLastHuman).TotalSeconds -ge 60) {
        $script:SpLastHuman = $now
        Write-Host ('   [{0:0}%] {1}' -f ($script:SpShown * 100), $note)
    }
}
function Clear-SpStatus {
    if (-not $script:SpEmit -and $script:SpInteractive) { $ProgressPreference = 'Continue'; Write-Progress -Id 2 -Activity ' ' -Completed }
    $script:SpLastHuman = [datetime]::MinValue
}

# ---------------------------------------------------------------- running a child process
# Runs a program with stdout (and stderr) read line by line, calling $OnLine for each line and $OnTick about every
# $TickMs while it runs (so the progress keeps moving and shows a heartbeat when the program prints nothing).
# Returns the exit code.
function Invoke-SpProcess([string]$FilePath, [string]$Arguments, [scriptblock]$OnLine, [scriptblock]$OnTick, [int]$TickMs = 1000,
                          [hashtable]$Environment = @{}, [switch]$Utf8) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = (Get-Location).Path
    if ($Utf8) { $psi.StandardOutputEncoding = $psi.StandardErrorEncoding = New-Object Text.UTF8Encoding($false) }
    foreach ($k in $Environment.Keys) { $psi.EnvironmentVariables[$k] = [string]$Environment[$k] }
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $task = $null
    $next = (Get-Date).AddMilliseconds($TickMs)
    while ($true) {
        if (-not $task) { $task = $p.StandardOutput.ReadLineAsync() }
        $wait = [int][math]::Max(50, [math]::Min($TickMs, ($next - (Get-Date)).TotalMilliseconds))
        if ($task.Wait($wait)) {
            $line = $task.Result; $task = $null
            if ($null -eq $line) { break }
            if ($OnLine) { & $OnLine $line }
        }
        if ((Get-Date) -ge $next) { $next = (Get-Date).AddMilliseconds($TickMs); if ($OnTick) { & $OnTick } }
    }
    $p.WaitForExit()
    $e = $err.Result
    if ($e) { foreach ($l in ($e -split "`r?`n")) { if ($l) { Write-Host $l } } }
    $code = $p.ExitCode
    $p.Dispose()
    $code
}

# ---------------------------------------------------------------- code generator
# "@@PROGRESS stage=<name> done=<n> total=<n>" -> @{ Stage; Done; Total }, else $null
function ConvertFrom-SpRegenLine([string]$Line) {
    if ($Line -match '^@@PROGRESS\s+stage=(\S+)\s+done=(\d+)\s+total=(\d+)') {
        return @{ Stage = $Matches[1]; Done = [double]$Matches[2]; Total = [double]$Matches[3] }
    }
    $null
}
# Fraction of the whole generation: $Expected = the stage names in order, $State = stage -> @{Done; Total},
# $Weights = stage -> seconds. A stage that printed nothing yet but comes before one that did counts as finished.
# A stage name may carry a prefix ("game:ppu_lift"); its weight is the one of the name after the colon.
function Get-SpRegenFraction([string[]]$Expected, [hashtable]$State, $Weights) {
    $order = New-Object System.Collections.Generic.List[string]
    foreach ($s in $Expected) { $order.Add($s) }
    foreach ($s in $State.Keys) { if (-not $order.Contains($s)) { $order.Add($s) } }
    $last = -1
    for ($i = 0; $i -lt $order.Count; $i++) { if ($State.ContainsKey($order[$i])) { $last = $i } }
    $tot = 0.0; $got = 0.0
    for ($i = 0; $i -lt $order.Count; $i++) {
        $s = $order[$i]
        $base = ($s -split ':')[-1]
        $w = 1.0
        if ($null -ne $Weights[$base]) { $w = [math]::Max(0.5, [double]$Weights[$base]) }
        $tot += $w
        if ($State.ContainsKey($s)) {
            $st = $State[$s]
            if ($i -lt $last) { $got += $w } elseif ($st.Total -gt 0) { $got += $w * [math]::Min(1.0, $st.Done / $st.Total) }
        } elseif ($i -lt $last) { $got += $w }
    }
    if ($tot -le 0) { return 0.0 }
    $got / $tot
}
$script:SpStageText = @{
    ppu_analyze = 'Analysing the game program'; ppu_lift = 'Translating the PPU code'; ppu_emit = 'Writing the PPU code'
    ppu_post = 'Optimising the PPU code (register cache, fast memory)'; ppu_hooks = "Applying the port's PPU changes"
    ppu_copy = 'Saving the PPU code (only changed files)'; spu_lift = 'Translating the SPU programs'
    spu_jobmods = 'Translating the SPU job modules'; spu_post = 'Optimising the SPU code'; spu_copy = 'Saving the SPU code (only changed files)'
}
# Runs the code generator: python <Script> <Arguments>, $Stages = the stages it is expected to go through.
function Invoke-SpRegen([string]$Python, [string]$Arguments, [string[]]$Stages) {
    $w = @{}
    foreach ($k in @($script:SpDefaults.regen.Keys)) { $w[$k] = Get-SpTime 'regen' $k }
    $st = @{ State = @{}; Start = @{}; Last = (Get-Date); T0 = (Get-Date); Stage = ''; W = $w; Stages = $Stages }
    $est = 0.0; foreach ($s in $Stages) { $base = ($s -split ':')[-1]; if ($w[$base]) { $est += $w[$base] } }
    Set-SpEstimate 'gen' $est
    $onLine = {
        param($l)
        $st.Last = Get-Date
        $p = ConvertFrom-SpRegenLine $l
        if (-not $p) { Write-Host $l; return }
        $now = Get-Date
        if (-not $st.State.ContainsKey($p.Stage)) {
            if ($st.Stage -and $st.Start[$st.Stage]) { $base = ($st.Stage -split ':')[-1]; $script:SpRun.regen[$base] = ([double]$script:SpRun.regen[$base]) + ($now - $st.Start[$st.Stage]).TotalSeconds }
            $st.Start[$p.Stage] = $now; $st.Stage = $p.Stage
        }
        $st.State[$p.Stage] = @{ Done = $p.Done; Total = $p.Total }
    }
    $onTick = {
        $f = Get-SpRegenFraction $st.Stages $st.State $st.W
        $el = ((Get-Date) - $st.T0).TotalSeconds
        $eta = -1
        if ($f -gt 0.03) { $eta = $el / $f * (1 - $f) } elseif ($el -lt 30) { $eta = ($st.W.Values | Measure-Object -Sum).Sum }
        $name = ($st.Stage -split ':')[-1]
        $txt = if ($name -and $script:SpStageText[$name]) { $script:SpStageText[$name] } else { 'Generating the game code' }
        $cur = $st.State[$st.Stage]
        if ($cur -and $cur.Total -gt 1 -and $name -ne 'ppu_post') { $txt += (' ({0} / {1})' -f [int]$cur.Done, [int]$cur.Total) }
        elseif ($cur -and $cur.Total -gt 1) { $txt += (' ({0:0}%)' -f (100 * $cur.Done / $cur.Total)) }
        Write-SpStatus ("Generating the game code: $txt") $f $eta ((Get-Date) - $st.Last).TotalSeconds
    }
    $code = Invoke-SpProcess $Python $Arguments $onLine $onTick 1000 @{ PYTHONUNBUFFERED = '1' } -Utf8
    if ($st.Stage -and $st.Start[$st.Stage]) { $base = ($st.Stage -split ':')[-1]; $script:SpRun.regen[$base] = ([double]$script:SpRun.regen[$base]) + ((Get-Date) - $st.Start[$st.Stage]).TotalSeconds }
    if ($code -ne 0) { $script:SpRun.regen = @{} }   # a failed run says nothing about how long it takes
    Clear-SpStatus
    $code
}

# ---------------------------------------------------------------- build
# The generated PPU files are most of the build, and their compile times differ a lot (in R3 a few files take
# 10+ minutes, most 1-2 minutes), so the build is counted per file: by the time each file took to compile the last
# time on this PC (logs\setup_timings.json, or the build folder's .ninja_log), else by the time measured on the PC
# the port was made on (tools\installer\ppu_compile_times.txt), else by its size. "Speed" scales those times to this
# PC and is corrected while the build runs.

# Known compile times: key ('recompiled\ppu_recomp_000.cpp') -> @{ Sec; Bytes } (Bytes = the file's size then, 0 = unknown)
function Read-SpNinjaLog([string]$Path, [string]$SrcLeaf) {
    $r = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $r }
    $re = '(?i)[\\/]' + [regex]::Escape($SrcLeaf) + '[\\/](ppu_recomp[^\\/]*\.cpp)\.obj$'
    foreach ($l in [IO.File]::ReadAllLines($Path)) {
        $p = $l -split "`t"
        if ($p.Count -lt 4 -or $p[3] -notmatch $re) { continue }
        $s = 0L; $e = 0L
        if ([long]::TryParse($p[0], [ref]$s) -and [long]::TryParse($p[1], [ref]$e) -and $e -gt $s) {
            $r[("$SrcLeaf\$($Matches[1])").ToLowerInvariant()] = @{ Sec = ($e - $s) / 1000.0; Bytes = 0.0 }   # later lines win
        }
    }
    $r
}
function Get-SpCompileTimes([string]$Port, $Pairs) {
    $t = @{}
    $def = Join-Path $Port 'tools\installer\ppu_compile_times.txt'
    if (Test-Path -LiteralPath $def) {
        foreach ($l in [IO.File]::ReadAllLines($def)) {
            if ($l -match '^\s*([^#\s]\S*)\s+([\d\.]+)\s+(\d+)') { $t[$Matches[1].ToLowerInvariant()] = @{ Sec = [double]$Matches[2]; Bytes = [double]$Matches[3] } }
        }
    }
    if ($script:SpOld -and $script:SpOld['files'] -is [System.Collections.IDictionary]) {
        foreach ($k in $script:SpOld['files'].Keys) {
            $v = ([string]$script:SpOld['files'][$k]).Trim() -split '\s+'   # "<seconds> <bytes>"
            $s = ConvertTo-SpNumber $v[0]; $b = 0.0
            if ($null -ne $s -and $s -gt 0) {
                if ($v.Count -ge 2) { $bv = ConvertTo-SpNumber $v[1]; if ($null -ne $bv) { $b = $bv } }
                $t[$k.ToLowerInvariant()] = @{ Sec = $s; Bytes = $b }
            }
        }
    }
    foreach ($pr in $Pairs) {
        $n = Read-SpNinjaLog (Join-Path $Port "$($pr[1])\.ninja_log") $pr[0]
        foreach ($k in $n.Keys) { if (-not $t.ContainsKey($k) -or -not $t[$k].Bytes) { $t[$k] = $n[$k] } }
    }
    $t
}
# Predicted seconds for one compile job of a file of $Bytes bytes, from the known times (scaled by size when the
# file changed size a lot), else from its size alone.
function Get-SpFileCost([string]$Key, [double]$Bytes, $Times) {
    $perJob = Get-SpTime 'build' 'ppu_job_bytes_per_sec'
    $k = $Key.ToLowerInvariant()
    if ($Times -and $Times.ContainsKey($k) -and $Times[$k].Sec -gt 0) {
        $s = [double]$Times[$k].Sec
        if ($Times[$k].Bytes -gt 0 -and $Bytes -gt 0) { $s *= [math]::Min(2.0, [math]::Max(0.5, $Bytes / $Times[$k].Bytes)) }
        return [math]::Max(1.0, $s)
    }
    [math]::Max(1.0, $Bytes / [math]::Max(1e4, $perJob))
}
# The generated PPU source files the build will compile: no object file yet, or the object is older than the source
# or than a generated header. key ('recompiled\ppu_recomp_000.cpp') -> @{ Bytes; Cost }. $Pairs: @(@('recompiled', 'build'), ...).
function Get-SpPpuBuildPlan([string]$Port, $Pairs, $Times = $null) {
    $plan = @{}
    foreach ($pr in $Pairs) {
        $src = Join-Path $Port $pr[0]; $bld = Join-Path $Port $pr[1]
        if (-not (Test-Path -LiteralPath $src)) { continue }
        $files = @(Get-ChildItem -LiteralPath $src -Filter 'ppu_recomp*.cpp' -File -ErrorAction SilentlyContinue)
        $hdr = Get-ChildItem -LiteralPath $src -Filter '*.h' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        $objs = @{}
        $cm = Join-Path $bld 'CMakeFiles'
        if (Test-Path -LiteralPath $cm) {
            foreach ($o in Get-ChildItem -LiteralPath $cm -Recurse -Filter 'ppu_recomp*.cpp.obj' -File -ErrorAction SilentlyContinue) {
                if ($o.FullName -match '(?i)\\' + [regex]::Escape((Split-Path -Leaf $src)) + '\\ppu_recomp[^\\]*\.cpp\.obj$') { $objs[$o.Name.ToLowerInvariant()] = $o.LastWriteTimeUtc }
            }
        }
        foreach ($f in $files) {
            $t = $objs[($f.Name + '.obj').ToLowerInvariant()]
            if (-not $t -or $t -lt $f.LastWriteTimeUtc -or ($hdr -and $t -lt $hdr.LastWriteTimeUtc)) {
                $k = ("$($pr[0])\$($f.Name)").ToLowerInvariant()
                $plan[$k] = @{ Bytes = [double]$f.Length; Cost = (Get-SpFileCost $k $f.Length $Times) }
            }
        }
    }
    $plan
}
# "[N/M] Building CXX object ...\recompiled\ppu_recomp_012.cpp.obj" -> @{ N; M; Key = 'recompiled\ppu_recomp_012.cpp' or $null }
function ConvertFrom-SpNinjaLine([string]$Line) {
    if ($Line -notmatch '^\s*\[(\d+)/(\d+)\]\s*(.*)$') { return $null }
    $r = @{ N = [int]$Matches[1]; M = [int]$Matches[2]; Key = $null; What = $Matches[3] }
    if ($r.What -match '^Building (?!\S+ object)') { return $null }   # build.bat's own "[1/2] Building front end..." (R1)
    if ($r.What -match '(?i)(?:^|[\\/])(recompiled(?:_game)?)[\\/](ppu_recomp[^\\/\s]*\.cpp)\.obj') { $r.Key = ("$($Matches[1])\$($Matches[2])").ToLowerInvariant() }
    $r
}
# Where the PPU part of the build stands. $Plan: key -> @{ Bytes; Cost }; $Done: key -> $true; $Running: key -> start
# time; $Jobs: how many PPU files compile at once; $Speed: real seconds per predicted second on this PC.
# Fraction = predicted work done (finished files, plus the running ones by their time so far) / all of it.
function Get-SpBuildEstimate($Plan, $Done, $Running, [datetime]$Now, [int]$Jobs, [double]$Speed) {
    $Jobs = [math]::Max(1, $Jobs); $Speed = [math]::Max(0.05, $Speed)
    $total = 0.0; $doneC = 0.0; $nDone = 0; $bytes = 0.0; $doneB = 0.0; $queued = 0.0
    foreach ($k in $Plan.Keys) {
        $total += $Plan[$k].Cost; $bytes += $Plan[$k].Bytes
        if ($Done.ContainsKey($k)) { $doneC += $Plan[$k].Cost; $doneB += $Plan[$k].Bytes; $nDone++ }
        elseif (-not $Running.ContainsKey($k)) { $queued += $Plan[$k].Cost }
    }
    $part = 0.0; $runLeft = 0.0; $runMax = 0.0; $nRun = 0; $cur = $null; $curT = $null
    foreach ($k in $Running.Keys) {
        if (-not $Plan.ContainsKey($k) -or $Done.ContainsKey($k)) { continue }
        $nRun++
        $c = $Plan[$k].Cost
        $el = [math]::Max(0.0, ($Now - $Running[$k]).TotalSeconds)
        $part += [math]::Min(0.95 * $c, $el / $Speed)
        # a file taking longer than predicted: assume it needs a bit more, never "0 s left"
        $left = [math]::Max($c * $Speed - $el, 0.1 * [math]::Max($c * $Speed, $el))
        $runLeft += $left; if ($left -gt $runMax) { $runMax = $left }
        if (-not $cur -or $Running[$k] -lt $curT) { $cur = $k; $curT = $Running[$k] }
    }
    $frac = if ($total -gt 0) { [math]::Min(1.0, ($doneC + $part) / $total) } else { 1.0 }
    $eta = [math]::Max($runMax, ($runLeft + $queued * $Speed) / $Jobs)
    [pscustomobject]@{
        Fraction = $frac; Eta = $eta; Total = $Plan.Count; DoneCount = $nDone; Running = $nRun; TotalBytes = $bytes; DoneBytes = $doneB
        TotalCost = $total; Current = $cur; CurrentSec = $(if ($curT) { ($Now - $curT).TotalSeconds } else { 0 })
    }
}
# The speed factor from the files compiled so far in this build: $Pairs of predicted / real seconds, weighted
# against the previous value as if it had been measured over $Prior seconds.
function Get-SpSpeed([double]$Prior, [double]$Pred, [double]$Real, [double]$PriorWeight = 600) {
    if ($Pred -le 0) { return $Prior }
    ($Prior * $PriorWeight + $Real) / ($PriorWeight + $Pred)
}
# clang-cl processes compiling this port's generated PPU files: key -> start time
function Get-SpCompiling([string]$Port) {
    $r = @{}
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name='clang-cl.exe' OR Name='clang.exe'" -Property CommandLine, CreationDate -ErrorAction Stop
    } catch { return $r }
    $root = [regex]::Escape($Port.TrimEnd('\'))
    foreach ($p in $procs) {
        $c = "$($p.CommandLine)"
        if ($c -match "(?i)$root\\(recompiled(?:_game)?)\\(ppu_recomp[^\\\s`"]*\.cpp)") {
            $k = ("$($Matches[1])\$($Matches[2])").ToLowerInvariant()
            if (-not $r.ContainsKey($k) -or $p.CreationDate -lt $r[$k]) { $r[$k] = $p.CreationDate }
        }
    }
    $r
}
# The last "[N/M]" line of a log another process is writing (opened shared): @{ N; M } or $null
function Get-SpLogNinja([string]$Path, [ref]$Offset) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try {
            if ($fs.Length -lt $Offset.Value) { $Offset.Value = 0 }
            $start = [math]::Max($Offset.Value, $fs.Length - 65536)
            $fs.Position = $start
            $buf = New-Object byte[] ($fs.Length - $start)
            $n = $fs.Read($buf, 0, $buf.Length)
            $Offset.Value = $start + $n
            $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        } finally { $fs.Dispose() }
    } catch { return $null }
    $m = [regex]::Matches($text, '\[(\d+)/(\d+)\]')
    if (-not $m.Count) { return $null }
    $x = $m[$m.Count - 1]
    @{ N = [int]$x.Groups[1].Value; M = [int]$x.Groups[2].Value }
}

# Runs build.bat (or another build script) with the progress: the RPCS3 renderer (from logs\build_rpcs3.log), then the
# game (Ninja's lines on stdout; the generated PPU files counted by their compile times), then the rest. Sets
# $LASTEXITCODE. $Pairs: the generated PPU folders and their build folders, e.g. @(, @('recompiled', 'build')).
function Invoke-SpBuild([string]$Port, [string]$Command, $Pairs, [string]$Rpcs3Log = 'logs\build_rpcs3.log') {
    $times = Get-SpCompileTimes $Port $Pairs
    $plan = Get-SpPpuBuildPlan $Port $Pairs $times
    $speed = Get-SpTime 'build' 'speed'
    $jobs = 0; if (-not [int]::TryParse("$env:PORT_PPU_JOBS", [ref]$jobs) -or $jobs -lt 1) { $jobs = 2 }
    $planCost = 0.0; $planBytes = 0.0; foreach ($v in $plan.Values) { $planCost += $v.Cost; $planBytes += $v.Bytes }
    $rpcs3Est = if (Test-Path -LiteralPath (Join-Path $Port 'build_rpcs3\build.ninja')) { 60.0 } else { Get-SpTime 'steps' 'rpcs3' }
    if (-not $Rpcs3Log -or -not (Test-SpHasRpcs3 $Port)) { $rpcs3Est = 0.0; $Rpcs3Log = '' }
    $first = -not (Test-Path -LiteralPath (Join-Path $Port "$($Pairs[0][1])\build.ninja"))
    $otherEst = if ($first) { Get-SpTime 'build' 'build_other_sec' } else { 60.0 }
    $gameEst = $planCost * $speed / $jobs + $otherEst
    $finishEst = Get-SpTime 'steps' 'finish'
    $st = @{
        Plan = $plan; Done = @{}; Running = @{}; Seen = @{}; Phase = 'start'; Last = (Get-Date); PpuT0 = $null; PpuEnd = $null
        RpcsOff = 0L; Rpcs = $null; Ninja = $null; Ninja0 = $null; NextScan = (Get-Date); Speed = $speed; Pred = 0.0; Real = 0.0
        GameT0 = $null; FinishT0 = $null; Started = (Get-Date); Jobs = $jobs
    }
    $deps = if ($script:SpCurStart) { ((Get-Date) - $script:SpCurStart).TotalSeconds } else { 0 }
    if (-not $script:SpRun.steps.Contains('deps')) { $script:SpRun.steps['deps'] = $deps }   # FFmpeg, SDKs, RPCS3 source
    Set-SpEstimate 'build' ($deps + $rpcs3Est + $gameEst + $finishEst)
    if ($plan.Count) {
        Write-Host ('   Game code to compile: {0} file(s), {1:0.00} GB of generated source, {2} at a time (estimated {3}).' -f $plan.Count, ($planBytes / 1e9), $jobs, (Format-SpDuration ($planCost * $speed / $jobs)))
    } else { Write-Host '   The generated PPU code is already compiled; only what changed is built.' }
    $onLine = {
        param($l)
        $now = Get-Date
        $st.Last = $now
        Write-Host $l
        if ($l -match '^Building the RPCS3') { $st.Phase = 'rpcs3' }
        elseif ($l -match '^Configuring|^Building the game') { if ($st.Phase -ne 'game') { $st.Phase = 'game'; $st.GameT0 = $now } }
        elseif ($l -match '^Building the launcher|^BUILD SUCCEEDED') { if ($st.Phase -ne 'finish') { $st.Phase = 'finish'; $st.FinishT0 = $now; if (-not $st.PpuEnd) { $st.PpuEnd = $now } } }
        $n = ConvertFrom-SpNinjaLine $l
        if ($n) {
            if (-not $st.Ninja0 -or $n.M -ne $st.Ninja0.M) { $st.Ninja0 = @{ N = $n.N; M = $n.M; T = $now } }
            $st.Ninja = $n
            if ($st.Phase -ne 'game') { $st.Phase = 'game'; if (-not $st.GameT0) { $st.GameT0 = $now } }
            if ($n.Key) {
                if (-not $st.Plan.ContainsKey($n.Key)) {   # not foreseen (e.g. new compiler flags): count it from now on
                    $f = Join-Path $Port $n.Key; $b = if (Test-Path -LiteralPath $f) { [double](Get-Item -LiteralPath $f).Length } else { 1e8 }
                    $st.Plan[$n.Key] = @{ Bytes = $b; Cost = (Get-SpFileCost $n.Key $b $times) }
                }
                $st.Done[$n.Key] = $true
                # the real compile time of this file (when its start was seen) corrects the speed for the rest
                if ($st.Seen.ContainsKey($n.Key)) {
                    $st.Pred += $st.Plan[$n.Key].Cost; $st.Real += ($now - $st.Seen[$n.Key]).TotalSeconds
                    $st.Speed = Get-SpSpeed $speed $st.Pred $st.Real
                }
                if (-not $st.PpuT0) { $st.PpuT0 = $st.GameT0 }
                $all = $true; foreach ($k in $st.Plan.Keys) { if (-not $st.Done.ContainsKey($k)) { $all = $false; break } }
                if ($all) { $st.PpuEnd = $now }
            }
        }
    }
    $onTick = {
        $now = Get-Date
        $idle = ($now - $st.Last).TotalSeconds
        if ($st.Phase -eq 'rpcs3' -or ($st.Phase -eq 'start' -and $Rpcs3Log)) {
            $off = [long]$st.RpcsOff
            $r = Get-SpLogNinja (Join-Path $Port $Rpcs3Log) ([ref]$off)
            if ($off -ne $st.RpcsOff) { $st.RpcsOff = $off; $st.Last = $now; $idle = 0 }
            if ($r) { $st.Rpcs = $r; $st.Phase = 'rpcs3' }
        }
        if ($now -ge $st.NextScan -and $st.Phase -eq 'game' -and $st.Plan.Count) {
            $st.NextScan = $now.AddSeconds(3)
            $run = Get-SpCompiling $Port
            foreach ($k in $run.Keys) {
                if (-not $st.Seen.ContainsKey($k)) { $st.Seen[$k] = $run[$k]; $st.Last = $now; $idle = 0 }
                if (-not $st.Plan.ContainsKey($k)) { $f = Join-Path $Port $k; $b = if (Test-Path -LiteralPath $f) { [double](Get-Item -LiteralPath $f).Length } else { 1e8 }; $st.Plan[$k] = @{ Bytes = $b; Cost = (Get-SpFileCost $k $b $times) } }
                if (-not $st.PpuT0) { $st.PpuT0 = $run[$k] }
            }
            if ($run.Count -gt $st.Jobs) { $st.Jobs = $run.Count }
            $st.Running = $run
        }
        $eRp = $rpcs3Est; $eFin = $finishEst
        $eGame = [math]::Max(1.0, $gameEst)
        $tot = [math]::Max(1.0, $deps + $eRp + $eGame + $eFin)
        $text = 'Building'; $eta = -1; $frac = 0.0
        switch ($st.Phase) {
            'start' { $text = 'Getting the build ready'; $frac = $deps / $tot; $eta = $eRp + $eGame + $eFin }
            'rpcs3' {
                $f = 0.0
                if ($st.Rpcs -and $st.Rpcs.M -gt 0) { $f = $st.Rpcs.N / $st.Rpcs.M; $text = 'Building the RPCS3 renderer: {0} / {1} files' -f $st.Rpcs.N, $st.Rpcs.M } else { $text = 'Building the RPCS3 renderer' }
                $el = ($now - $st.Started).TotalSeconds
                $left = if ($f -gt 0.05 -and $el -gt 20) { $el / $f * (1 - $f) } else { [math]::Max(10, $eRp - $el) }
                $frac = ($deps + $eRp * [math]::Min(1.0, $f)) / $tot; $eta = $left + $eGame + $eFin
            }
            'game' {
                $ninjaF = if ($st.Ninja -and $st.Ninja.M -gt 0) { $st.Ninja.N / $st.Ninja.M } else { 0.0 }
                # the rest of the game build from Ninja's step rate (it runs alongside the PPU files)
                $otherLeft = $otherEst * (1 - $ninjaF)
                if ($st.Ninja -and $st.Ninja0 -and $st.Ninja.N - $st.Ninja0.N -ge 5) {
                    $el = ($now - $st.Ninja0.T).TotalSeconds
                    $otherLeft = $el / ($st.Ninja.N - $st.Ninja0.N) * ($st.Ninja.M - $st.Ninja.N)
                }
                if ($st.Plan.Count) {
                    $e = Get-SpBuildEstimate $st.Plan $st.Done $st.Running $now $st.Jobs $st.Speed
                    $ppuShare = [math]::Min(0.97, ($e.TotalCost * $st.Speed / $st.Jobs) / [math]::Max(1.0, $e.TotalCost * $st.Speed / $st.Jobs + $otherEst))
                    $gf = $ppuShare * $e.Fraction + (1 - $ppuShare) * $ninjaF
                    if ($e.DoneCount -lt $e.Total) {
                        $x = [math]::Min($e.Total, $e.DoneCount + [math]::Max(1, $e.Running))
                        $text = 'Building the game: game code file {0} of {1}' -f $x, $e.Total
                        if ($e.Current) { $text += ' ({0}, {1} so far)' -f (Split-Path -Leaf $e.Current), (Format-SpDuration $e.CurrentSec) }
                        $eta = $e.Eta + 30 + $eFin
                    } else {
                        $text = if ($st.Ninja) { 'Building the game: {0} / {1} steps' -f $st.Ninja.N, $st.Ninja.M } else { 'Building the game' }
                        $eta = [math]::Min($otherLeft, 120) + $eFin
                    }
                } else {
                    $gf = $ninjaF
                    $text = if ($st.Ninja) { 'Building the game: {0} / {1} steps' -f $st.Ninja.N, $st.Ninja.M } else { 'Configuring the game build' }
                    $eta = [math]::Max(5, $otherLeft) + $eFin
                }
                $frac = ($deps + $eRp + $eGame * $gf) / $tot
            }
            'finish' { $text = 'Building the launcher and tools'; $el = ($now - $st.FinishT0).TotalSeconds; $frac = ($deps + $eRp + $eGame + [math]::Min($eFin, $el)) / $tot; $eta = [math]::Max(5, $eFin - $el) }
        }
        Write-SpStatus $text $frac $eta $idle
    }
    $code = Invoke-SpProcess $env:ComSpec ('/d /c "' + $Command + ' 2>&1"') $onLine $onTick 1000
    $now = Get-Date
    if ($code -eq 0) {
        if ($st.GameT0 -and -not $st.FinishT0) { $st.FinishT0 = $now }
        if ($st.GameT0 -and $first) { $script:SpRun.build['build_other_sec'] = [math]::Max(30.0, ($st.FinishT0 - $st.GameT0).TotalSeconds - $planCost * $st.Speed / $st.Jobs) }
        if ($st.FinishT0) { $script:SpRun.steps['finish'] = ($now - $st.FinishT0).TotalSeconds }
        $rpEnd = if ($st.GameT0) { $st.GameT0 } else { $now }
        if ($st.Rpcs -and $st.Rpcs.M -gt 50) { $script:SpRun.steps['rpcs3'] = ($rpEnd - $st.Started).TotalSeconds }
        if ($st.Pred -gt 120) { $script:SpRun.build['speed'] = $st.Real / $st.Pred }
        # each PPU file's compile time on this PC (from Ninja's own log), for the next estimate
        $files = @{}
        foreach ($pr in $Pairs) {
            $n = Read-SpNinjaLog (Join-Path $Port "$($pr[1])\.ninja_log") $pr[0]
            foreach ($k in $n.Keys) {
                $fp = Join-Path $Port $k
                if (Test-Path -LiteralPath $fp) { $files[$k] = Format-SpFileTime $n[$k].Sec (Get-Item -LiteralPath $fp).Length }
            }
        }
        if ($files.Count) { $script:SpRun['files'] = $files }
        if ($st.PpuT0 -and $st.Done.Count) {
            $end = if ($st.PpuEnd) { $st.PpuEnd } else { $now }
            Write-Host ('   Compiled {0} game code file(s) in {1}.' -f $st.Done.Count, (Format-SpDuration ($end - $st.PpuT0).TotalSeconds))
        }
    }
    Clear-SpStatus
    $global:LASTEXITCODE = $code
}
