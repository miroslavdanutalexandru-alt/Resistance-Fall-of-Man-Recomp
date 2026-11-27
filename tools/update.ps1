# Updates this port folder to a newer GitHub release, keeping everything that belongs to the player.
#
# The launcher runs it ("Update now" / Updates: Auto-install):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\update.ps1 -Repo <owner/name> -Tag <vX.Y.Z> -WaitPid <launcher pid>
# By hand:  powershell -ExecutionPolicy Bypass -File tools\update.ps1             (newest release)
#           ... -ZipFile <release zip> -DryRun                                     (show what would change, change nothing)
#           ... -Rollback                                                          (undo an interrupted update)
#
# What it does:
#   1. finds the release zip (<Port>-<version>-Windows-x64.zip, the one scripts\package_beta.ps1 makes), downloads it
#      with a progress bar and checks its size and SHA-256 (SHA256SUMS.txt of the release, else GitHub's asset digest).
#      A release that publishes neither is refused before anything is downloaded or changed, unless -NoVerify is given
#      (a zip passed with -ZipFile is the player's own choice and is installed as given)
#   2. waits until the launcher / game / tools started from this folder have closed
#   3. unpacks it to .upd\new (inside this folder, so files are moved, not copied) and deletes the zip
#   4. puts the new files in place: unchanged files are not touched (their dates stay, so the build stays
#      incremental), changed and new files get the current time; every replaced file is first moved to .upd\old
#      and each step is written to .upd\journal.txt before it is done
#   5. deletes files the old version shipped and the new one no longer has, but only when they are listed in
#      install_manifest.txt (written by the previous update or shipped in the release zip) and unchanged since;
#      without a manifest nothing is deleted. Then writes the new install_manifest.txt
#   6. runs tools\fix_timestamps.ps1 and setup.ps1 -Unattended (only the steps that are needed: the game code is
#      regenerated only when its inputs changed, the build is incremental)
#   7. deletes .upd and starts Launcher.exe again
# Any failure (download, a locked file, Setup / the build) moves the old files back from .upd\old (journal order,
# reversed) and leaves the details in logs\update.log. An update cut short (window closed, power loss) is undone the
# next time this script runs, or with -Rollback.
#
# Never written or deleted (the player's data, see $Protected below): settings.ini / settings.bat, portable.txt,
# trophies.json, mouselook_r2.cfg / mouselook_r3.cfg, Launcher.exe (built locally), logs\, gamefiles*\, game\, gamedata\
# (saves, extracted game files), shadercache\, cache\, build*\ and recompiled*\ (the incremental build state), mods\,
# screenshots\, save_backups\, rpcs3\, lle\, downloaded SDKs and tools (tools\ffmpeg_bin, tools\dlss, tools\xess,
# tools\streamline, tools\shaderscan.exe, tools\key_vault.cpp, ps3recomp\third_party\rpcs3), generated code
# (project\spu\<folders>\, project\spu\jobmods_register.c) and .git. Shipped files the player may have edited
# (controls.txt, mouselook.cfg, rpcs3_config.yml) are kept when they differ from what the old version shipped.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Repo = '',       # owner/name on GitHub (default: "repo" in recomplauncher.json)
    [string]$Tag = '',        # release tag (default: the newest release, pre-releases included)
    [string]$ZipFile = '',    # use this release zip instead of downloading (kept afterwards)
    [string]$Root = '',       # the port folder (default: the folder above tools\)
    [int]$WaitPid = 0,        # wait for this process (the launcher) to exit first
    [switch]$DryRun,          # download / unpack to %TEMP% and list the changes; this folder is not changed (also -WhatIf)
    [switch]$NoSetup,         # do not run fix_timestamps.ps1 / setup.ps1 afterwards
    [switch]$NoRelaunch,      # do not start Launcher.exe at the end
    [switch]$Unattended,      # never wait for Enter
    [switch]$Rollback,        # only undo an interrupted update (.upd\journal.txt) and exit
    [switch]$Force,           # update a git checkout / install the same version again
    [switch]$NoVerify         # install a downloaded release even if it publishes no SHA-256 (only the size is checked)
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell started from a PowerShell 7 window (directly or through cmd / a .bat) inherits pwsh's module
# paths and then cannot load its own modules: Get-FileHash and Get-AuthenticodeSignature are 'not recognized', and
# Expand-Archive loads pwsh's copy and fails. Keep only Windows PowerShell's own paths. Get-PortFileHash and
# Expand-PortZip fall back to .NET if the cmdlets still cannot run (same output: upper-case hex / the same files).
# (Same block in setup.ps1, tools\update.ps1, tools\get_shaderscan.ps1, tools\installer\prereqs.ps1, scripts\*.ps1.)
if ($PSVersionTable.PSVersion.Major -lt 6) {
    $pmp = @(($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\(7[^\\]*\\)?Modules\\?$' })
    $pmpSys = Join-Path $PSHOME 'Modules'
    if (-not @($pmp | Where-Object { $_.TrimEnd('\') -eq $pmpSys }).Count) { $pmp += $pmpSys }
    $env:PSModulePath = $pmp -join ';'
}
$script:PortHashCmd = $null
function Get-PortFileHash([string]$LiteralPath, [string]$Algorithm = 'SHA256') {
    if ($script:PortHashCmd -ne $false) {
        try {
            $r = Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
            $script:PortHashCmd = $true
            if ($null -eq $r) { return $null }
            return "$($r.Hash)".ToUpperInvariant()
        } catch [System.Management.Automation.CommandNotFoundException] { $script:PortHashCmd = $false }
    }
    $a = switch ($Algorithm.ToUpperInvariant()) {
        'SHA1' { [Security.Cryptography.SHA1]::Create() }
        'SHA384' { [Security.Cryptography.SHA384]::Create() }
        'SHA512' { [Security.Cryptography.SHA512]::Create() }
        'MD5' { [Security.Cryptography.MD5]::Create() }
        default { [Security.Cryptography.SHA256]::Create() }
    }
    $s = [IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LiteralPath))
    try { return ([BitConverter]::ToString($a.ComputeHash($s)) -replace '-', '') } finally { $s.Dispose(); $a.Dispose() }
}
function Expand-PortZip([string]$LiteralPath, [string]$DestinationPath, [switch]$Force) {
    try { Expand-Archive -LiteralPath $LiteralPath -DestinationPath $DestinationPath -Force:$Force -ErrorAction Stop; return }
    catch {
        $e = $_.Exception
        while ($e -is [System.Management.Automation.ActionPreferenceStopException] -and $e.InnerException) { $e = $e.InnerException }
        if (-not ($e -is [System.Management.Automation.CommandNotFoundException] -or $e -is [System.Management.Automation.MethodException])) { Write-Error -ErrorRecord $_; return }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LiteralPath)
    $dst = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DestinationPath))
    [void][IO.Directory]::CreateDirectory($dst)
    $root = $dst.TrimEnd('\') + '\'
    $z = [IO.Compression.ZipFile]::OpenRead($src)
    try {
        foreach ($en in $z.Entries) {
            $t = [IO.Path]::GetFullPath([IO.Path]::Combine($dst, $en.FullName))
            if (-not $t.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe path in $src`: $($en.FullName)" }
            if ($en.FullName.EndsWith('/') -or $en.FullName.EndsWith('\')) { [void][IO.Directory]::CreateDirectory($t); continue }
            if (-not $Force -and [IO.File]::Exists($t)) { throw "$t already exists (Expand-Archive without -Force)" }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($t))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($en, $t, $true)
        }
    } finally { $z.Dispose() }
}
$ProgressPreference = 'Continue'
if ($WhatIfPreference) { $DryRun = $true; $WhatIfPreference = $false }
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
$Logs = Join-Path $Root 'logs'
if (-not (Test-Path -LiteralPath $Logs)) { New-Item -ItemType Directory -Path $Logs | Out-Null }
$LogFile = Join-Path $Logs 'update.log'
if ((Test-Path -LiteralPath $LogFile) -and (Get-Item -LiteralPath $LogFile).Length -gt 1MB) {
    Move-Item -LiteralPath $LogFile -Destination (Join-Path $Logs 'update.prev.log') -Force
}
$Upd = Join-Path $Root '.upd'
$Journal = Join-Path $Upd 'journal.txt'
$Utf8 = New-Object System.Text.UTF8Encoding $false

function Log([string]$m, [string]$color = '') {
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}{2}' -f (Get-Date), $(if ($DryRun) { '[dry run] ' } else { '' }), $m
    try { [IO.File]::AppendAllText($LogFile, $line + "`r`n", $Utf8) } catch {}
    if ($color) { Write-Host $m -ForegroundColor $color } else { Write-Host $m }
}
function Pause-End { if (-not $Unattended) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null } }

# ---------------------------------------------------------------- the player's files
# Paths relative to the port folder, '/' separated, compared case-insensitively.
$ProtectedTop = @('.git', '.upd', 'game', 'gamedata', 'logs', 'shadercache', 'cache', 'lle', 'rsxcap', 'scratch', 'dist', 'mods',
                  'screenshots', 'save_backups', 'rpcs3', '__pycache__')
$ProtectedTopLike = @('gamefiles*', 'recompiled*', 'build*', 'backup_build_*')   # folders only (build.bat is a file)
$ProtectedFiles = @('settings.ini', 'settings.bat', 'portable.txt', 'trophies.json', 'launcher.exe', 'mouselook_r2.cfg',
                    'mouselook_r3.cfg', 'controls.txt.bak', 'gamefiles.copying', 'install_manifest.txt')
$ProtectedLike = @('tools/ffmpeg_bin/*', 'tools/dlss/*', 'tools/xess/*', 'tools/streamline/*', 'tools/shaderscan.exe*',
                   'tools/key_vault.cpp', 'tools/ffx_bin/*.download', 'tools/ffx_bin/amd_fidelityfx_framegeneration_dx12.dll',
                   'ps3recomp/third_party/rpcs3/*', 'project/spu/*/*', 'project/spu/jobmods_register.c')
$UserEditable = @('controls.txt', 'mouselook.cfg', 'rpcs3_config.yml')
function Test-Protected([string]$rel) {
    $parts = $rel.Split('/')
    if ($parts.Count -gt 1) {
        if ($ProtectedTop -contains $parts[0]) { return $true }
        foreach ($p in $ProtectedTopLike) { if ($parts[0] -like $p) { return $true } }
    } elseif ($ProtectedFiles -contains $rel) { return $true }
    foreach ($p in $ProtectedLike) { if ($rel -like $p) { return $true } }
    return $false
}

# ---------------------------------------------------------------- helpers
$Sha = [Security.Cryptography.SHA256]::Create()
function Get-Sha([string]$path) {
    $s = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { return ([BitConverter]::ToString($Sha.ComputeHash($s)) -replace '-', '').ToLowerInvariant() } finally { $s.Dispose() }
}
function Get-Tree([string]$dir) {   # relative '/' paths of all files
    $base = $dir.TrimEnd('\').Length + 1
    @([IO.Directory]::EnumerateFiles($dir, '*', 'AllDirectories') | ForEach-Object { $_.Substring($base).Replace('\', '/') })
}
function Read-Manifest([string]$path) {
    $m = @{}
    if (Test-Path -LiteralPath $path) {
        foreach ($l in [IO.File]::ReadAllLines($path)) {
            if ($l -match '^([0-9a-fA-F]{64}) (.+)$') { $m[$Matches[2].Trim().ToLowerInvariant()] = $Matches[1].ToLowerInvariant() }
        }
    }
    return $m
}
function Native([string]$rel) { $rel.Replace('/', '\') }
function Ensure-Parent([string]$path) { $d = Split-Path -Parent $path; if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null } }
function Touch-Now([string]$path) {
    try { [IO.File]::SetLastWriteTime($path, (Get-Date)) }
    catch { [IO.File]::SetAttributes($path, [IO.File]::GetAttributes($path) -band -bnot [IO.FileAttributes]::ReadOnly); [IO.File]::SetLastWriteTime($path, (Get-Date)) }
}
function Move-Over([string]$from, [string]$to) {
    Ensure-Parent $to
    if (Test-Path -LiteralPath $to) { [IO.File]::SetAttributes($to, 'Normal'); [IO.File]::Delete($to) }
    [IO.File]::Move($from, $to)
}
function Get-Ver([string]$s) { if ($s -match '(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.-]+)?') { return $Matches } return $null }

# ---------------------------------------------------------------- undo
function Invoke-Rollback {
    if (-not (Test-Path -LiteralPath $Journal)) { return $true }
    Log 'Putting the previous version back (.upd\journal.txt) ...' 'Yellow'
    $lines = @([IO.File]::ReadAllLines($Journal))
    [array]::Reverse($lines)
    $old = Join-Path $Upd 'old'
    $bad = 0
    foreach ($l in $lines) {
        if ($l -notmatch '^([RAD]) (.+)$') { continue }
        $op = $Matches[1]; $rel = $Matches[2]
        $dst = Join-Path $Root (Native $rel); $bak = Join-Path $old (Native $rel)
        try {
            if ($op -eq 'A') { if (Test-Path -LiteralPath $dst -PathType Leaf) { [IO.File]::SetAttributes($dst, 'Normal'); [IO.File]::Delete($dst) } }
            elseif (Test-Path -LiteralPath $bak) { Move-Over $bak $dst; Touch-Now $dst }   # R / D; no backup: the step never ran
        } catch { $bad++; Log "  could not restore $rel : $($_.Exception.Message)" 'Red' }
    }
    if ($bad) { Log "Rollback incomplete ($bad file(s)); the old copies are still in .upd\old. Re-run: tools\update.ps1 -Rollback" 'Red'; return $false }
    Remove-Item -LiteralPath $Upd -Recurse -Force -ErrorAction SilentlyContinue
    Log 'The previous version is back in place.' 'Yellow'
    return $true
}

# ---------------------------------------------------------------- main
$stage = $null; $downloaded = $null; $applied = $false
try {
    Log "==== update.ps1 in $Root"
    if ($WaitPid -and -not $DryRun) {
        $p = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
        if ($p) { Log 'Waiting for the launcher to close ...'; if (-not $p.WaitForExit(120000)) { throw 'The launcher did not close.' } }
    }
    if (-not $DryRun -and (Test-Path -LiteralPath $Journal)) {
        Log 'An earlier update did not finish.' 'Yellow'
        if (-not (Invoke-Rollback)) { throw 'could not undo the earlier update' }
    }
    if ($Rollback) {
        Log 'Nothing else to do (-Rollback).'
        $launcher = Join-Path $Root 'Launcher.exe'
        if (-not $NoRelaunch -and (Test-Path -LiteralPath $launcher)) { Start-Process -FilePath $launcher -WorkingDirectory $Root } else { Pause-End }
        exit 0
    }
    if ((Test-Path -LiteralPath (Join-Path $Root '.git')) -and -not $Force) {
        throw 'This folder is a git checkout: update it with "git pull" and run Setup.bat (or pass -Force).'
    }
    $have = ''
    if (Test-Path -LiteralPath (Join-Path $Root 'VERSION')) { $have = ([IO.File]::ReadAllText((Join-Path $Root 'VERSION'))).Trim() }

    # ---- the release
    $asset = $null; $sumsHash = $null
    if (-not $ZipFile) {
        if (-not $Repo) {
            $rj = Join-Path $Root 'recomplauncher.json'
            if (Test-Path -LiteralPath $rj) { $Repo = (Get-Content -Raw -LiteralPath $rj | ConvertFrom-Json).repo }
        }
        if ($Repo -notmatch '^[\w.-]+/[\w.-]+$') { throw "No GitHub repository (-Repo owner/name): '$Repo'" }
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $hdr = @{ 'User-Agent' = 'ResistancePorts-Updater'; 'Accept' = 'application/vnd.github+json' }
        if ($Tag) {
            if ($Tag -notmatch '^[\w.+-]+$') { throw "Bad tag '$Tag'" }
            $rel = Invoke-RestMethod -UseBasicParsing -Headers $hdr -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag"
        } else {
            $all = @(Invoke-RestMethod -UseBasicParsing -Headers $hdr -Uri "https://api.github.com/repos/$Repo/releases?per_page=30")
            $rel = $all | Where-Object { -not $_.draft -and (Get-Ver $_.tag_name) } |
                   Sort-Object { $v = Get-Ver $_.tag_name; '{0:D6}.{1:D6}.{2:D6}{3}' -f [int]$v[1], [int]$v[2], [int]$v[3], $(if ($v[4]) { $v[4] } else { '~' }) } |
                   Select-Object -Last 1
            if (-not $rel) { throw "No release found in $Repo" }
            $Tag = $rel.tag_name
        }
        Log "Release $Tag of $Repo (this copy: $(if ($have) { $have } else { 'unknown' }))"
        $asset = @($rel.assets | Where-Object { $_.name -match '-Windows-x64\.zip$' -and $_.name -notmatch '(?i)source|debug|symbols' }) | Select-Object -First 1
        if (-not $asset) { throw "Release $Tag has no <Port>-<version>-Windows-x64.zip; download it from https://github.com/$Repo/releases/tag/$Tag" }
        $sums = @($rel.assets | Where-Object { $_.name -eq 'SHA256SUMS.txt' }) | Select-Object -First 1
        if ($sums) {
            $txt = (Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'ResistancePorts-Updater' } -Uri $sums.browser_download_url).Content
            if ($txt -is [byte[]]) { $txt = $Utf8.GetString($txt) }
            foreach ($l in ($txt -split "`n")) { if ($l -match '^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$' -and $Matches[2] -eq $asset.name) { $sumsHash = $Matches[1].ToLowerInvariant() } }
        }
        if (-not $sumsHash -and $asset.digest -and "$($asset.digest)" -match '^sha256:([0-9a-fA-F]{64})$') { $sumsHash = $Matches[1].ToLowerInvariant() }
    }
    $newVer = $Tag -replace '^[vV]', ''
    if ($Tag -and $have -and $have -eq $newVer -and -not $Force) { Log "This copy is already $have. Nothing to do (-Force installs it again)." 'Green'; Pause-End; exit 0 }
    if ($asset -and -not $sumsHash -and -not $NoVerify) {
        throw ("Release $Tag publishes no SHA-256 for $($asset.name) (no SHA256SUMS.txt, no GitHub digest), so the download " +
               "cannot be verified. Nothing was changed. Download the zip from https://github.com/$Repo/releases/tag/$Tag and run " +
               "tools\update.ps1 -ZipFile <zip>, or run it with -NoVerify to install it unverified.")
    }

    # ---- space: the zip plus the unpacked package (the old files are moved, not copied)
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Root))
    if ($asset -and $drive.AvailableFreeSpace -lt ([int64]$asset.size * 5 + 512MB)) {
        throw ('Not enough free space on {0}: about {1:N1} GB needed for the update, {2:N1} GB free.' -f $drive.Name, (([int64]$asset.size * 5 + 512MB) / 1GB), ($drive.AvailableFreeSpace / 1GB))
    }

    # ---- staging
    if ($DryRun) { $stage = Join-Path ([IO.Path]::GetTempPath()) ('port_update_' + [Guid]::NewGuid().ToString('N').Substring(0, 8)) }
    else { $stage = $Upd }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    if (-not $DryRun) { (Get-Item -LiteralPath $stage -Force).Attributes = 'Directory, Hidden' }

    # ---- download
    if (-not $ZipFile) {
        $downloaded = Join-Path $stage $asset.name
        for ($try = 1; ; $try++) {
            try {
                $req = [Net.HttpWebRequest]::Create($asset.browser_download_url)
                $req.UserAgent = 'ResistancePorts-Updater'; $req.Timeout = 30000; $req.ReadWriteTimeout = 60000
                $resp = $req.GetResponse()
                $in = $resp.GetResponseStream(); $out = [IO.File]::Create($downloaded)
                try {
                    $buf = New-Object byte[] (1MB); $got = [int64]0; $total = [int64]$asset.size; $last = [DateTime]::MinValue
                    while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                        $out.Write($buf, 0, $n); $got += $n
                        if (([DateTime]::Now - $last).TotalMilliseconds -gt 400) {
                            $last = [DateTime]::Now
                            $pct = if ($total) { [int](100 * $got / $total) } else { 0 }
                            Write-Progress -Activity "Downloading $($asset.name)" -Status ('{0:N0} / {1:N0} MB' -f ($got / 1MB), ($total / 1MB)) -PercentComplete ([Math]::Min($pct, 100))
                        }
                    }
                } finally { $out.Dispose(); $in.Dispose(); $resp.Dispose(); Write-Progress -Activity "Downloading $($asset.name)" -Completed }
                break
            } catch {
                if ($try -ge 3) { throw "download failed: $($_.Exception.Message)" }
                Log "  download attempt $try failed ($($_.Exception.Message)); retrying" 'Yellow'
                Start-Sleep -Seconds (5 * $try)
            }
        }
        $len = (Get-Item -LiteralPath $downloaded).Length
        if ($len -ne [int64]$asset.size) { throw "The download is $len bytes, the release says $($asset.size): incomplete." }
        $ZipFile = $downloaded
        Log ('Downloaded {0} ({1:N1} MB)' -f $asset.name, ($len / 1MB))
    }
    $ZipFile = (Resolve-Path -LiteralPath $ZipFile).Path
    $zipHash = Get-Sha $ZipFile
    if ($sumsHash) {
        if ($zipHash -ne $sumsHash) { throw "SHA-256 mismatch: the zip is $zipHash, the release publishes $sumsHash. Not installed." }
        Log "SHA-256 verified ($zipHash)"
    } elseif ($asset) {
        Log 'The release publishes no SHA-256 (no SHA256SUMS.txt, no GitHub digest): only the size was checked (-NoVerify).' 'Yellow'
    } else { Log "Zip $ZipFile, SHA-256 $zipHash" }

    # ---- unpack
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $za = [IO.Compression.ZipFile]::OpenRead($ZipFile)
    try { $unpacked = [int64]0; foreach ($e in $za.Entries) { $unpacked += $e.Length } } finally { $za.Dispose() }
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Root))
    if ($drive.AvailableFreeSpace -lt $unpacked + 256MB) {
        throw ('Not enough free space on {0} to unpack the update: {1:N1} GB needed, {2:N1} GB free.' -f $drive.Name, (($unpacked + 256MB) / 1GB), ($drive.AvailableFreeSpace / 1GB))
    }
    $newDir = Join-Path $stage 'new'
    if (Test-Path -LiteralPath $newDir) { Remove-Item -LiteralPath $newDir -Recurse -Force }
    Log ('Unpacking ({0:N0} MB) ...' -f ($unpacked / 1MB))
    [IO.Compression.ZipFile]::ExtractToDirectory($ZipFile, $newDir)
    if ($downloaded) { Remove-Item -LiteralPath $downloaded -Force; $downloaded = $null }   # disk space: the zip is no longer needed
    $pkg = $newDir
    $items = @(Get-ChildItem -LiteralPath $newDir -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $pkg = $items[0].FullName }   # the zip's <Port>-<version>\ folder

    # ---- is it this port, this version?
    $pkgVerFile = Join-Path $pkg 'VERSION'
    if (-not (Test-Path -LiteralPath $pkgVerFile) -or -not (Test-Path -LiteralPath (Join-Path $pkg 'setup.ps1'))) { throw 'The zip is not a port release (no VERSION / setup.ps1 in it).' }
    $pkgVer = ([IO.File]::ReadAllText($pkgVerFile)).Trim()
    if ($Tag -and $pkgVer -ne $newVer) { throw "The zip is version $pkgVer, the release tag is $Tag." }
    $tid = { param($f) if ((Test-Path -LiteralPath $f) -and ((Get-Content -Raw -LiteralPath $f) -match "\`$TitleId\s*=\s*'(\w+)'")) { $Matches[1] } else { '' } }
    $tidOld = & $tid (Join-Path $Root 'setup.ps1'); $tidNew = & $tid (Join-Path $pkg 'setup.ps1')
    if ($tidOld -and $tidNew -and $tidOld -ne $tidNew) { throw "The zip is for another game ($tidNew; this folder is $tidOld)." }
    if (-not $Tag -and $have -eq $pkgVer -and -not $Force) { Log "This copy is already $have. Nothing to do (-Force installs it again)." 'Green'; if (-not $DryRun) { Remove-Item -LiteralPath $Upd -Recurse -Force }; Pause-End; exit 0 }

    # ---- the plan
    Log 'Comparing files ...'
    $oldMan = Read-Manifest (Join-Path $Root 'install_manifest.txt')
    $newFiles = @(Get-Tree $pkg | Where-Object { $_ -ne 'install_manifest.txt' })
    $newSet = @{}; $newMan = New-Object System.Text.StringBuilder
    $plan = New-Object System.Collections.Generic.List[object]
    $kept = New-Object System.Collections.Generic.List[string]
    $same = 0
    foreach ($rel in $newFiles) {
        $key = $rel.ToLowerInvariant(); $newSet[$key] = $true
        $src = Join-Path $pkg (Native $rel); $dst = Join-Path $Root (Native $rel)
        $h = Get-Sha $src
        [void]$newMan.Append("$h $rel`n")
        if (Test-Protected $key) { $kept.Add("$rel (player data / local)"); continue }
        if (-not (Test-Path -LiteralPath $dst -PathType Leaf)) { $plan.Add(@('A', $rel)); continue }
        $cur = Get-Sha $dst
        if ($cur -eq $h) { $same++; continue }
        if ($UserEditable -contains $key -and (-not $oldMan.ContainsKey($key) -or $oldMan[$key] -ne $cur)) { $kept.Add("$rel (edited by you; the new default is in the release)"); continue }
        $plan.Add(@('R', $rel))
    }
    $orphans = 0
    foreach ($key in @($oldMan.Keys)) {
        if ($newSet.ContainsKey($key) -or (Test-Protected $key)) { continue }
        $dst = Join-Path $Root (Native $key)
        if (-not (Test-Path -LiteralPath $dst -PathType Leaf)) { continue }
        if ((Get-Sha $dst) -ne $oldMan[$key]) { $kept.Add("$key (no longer shipped, but changed by you: kept)"); continue }
        $relOnDisk = (Get-Item -LiteralPath $dst).FullName.Substring($Root.Length + 1).Replace('\', '/')
        $plan.Add(@('D', $relOnDisk)); $orphans++
    }
    if (-not $oldMan.Count) { Log 'No install_manifest.txt (first update of this copy): files removed from the port are left in place this time.' }
    $cnt = @{ A = 0; R = 0; D = 0 }; foreach ($p in $plan) { $cnt[$p[0]]++ }
    Log ("Update {0} -> {1}: {2} changed, {3} new, {4} removed, {5} unchanged, {6} kept" -f $(if ($have) { $have } else { '?' }), $pkgVer, $cnt.R, $cnt.A, $cnt.D, $same, $kept.Count)
    foreach ($k in $kept) { if ($k -notmatch 'player data') { Log "  kept: $k" } }
    if ($DryRun) {
        foreach ($p in $plan) { Log ("  {0} {1}" -f @{ A = 'new    '; R = 'changed'; D = 'removed' }[$p[0]], $p[1]) }
        Log 'Dry run: nothing in this folder was changed.' 'Green'
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
        Pause-End; exit 0
    }

    # ---- wait for the game / tools started from this folder
    $deadline = (Get-Date).AddMinutes(10); $said = $false
    while ($true) {
        $busy = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
            $_.ProcessId -ne $PID -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) })
        if (-not $busy.Count) { break }
        if ((Get-Date) -gt $deadline) { throw "Still running from this folder: $(($busy | ForEach-Object { $_.Name } | Sort-Object -Unique) -join ', '). Close them and try again." }
        if (-not $said) { Log "Waiting for $(($busy | ForEach-Object { $_.Name } | Sort-Object -Unique) -join ', ') to close (up to 10 minutes) ..." 'Yellow'; $said = $true }
        Start-Sleep -Seconds 2
    }

    # ---- apply (journal first, then the step)
    $old = Join-Path $Upd 'old'
    $jw = New-Object IO.StreamWriter($Journal, $true, $Utf8)
    $jw.AutoFlush = $true
    $applied = $true
    try {
        $i = 0
        $plan.Add(@('R', 'install_manifest.txt'))   # last: the new list of shipped files
        foreach ($p in $plan) {
            $op = $p[0]; $rel = $p[1]; $i++
            if ($i % 50 -eq 0) { Write-Progress -Activity 'Installing the update' -Status $rel -PercentComplete ([Math]::Min(100, [int](100 * $i / $plan.Count))) }
            $dst = Join-Path $Root (Native $rel); $bak = Join-Path $old (Native $rel)
            if ($rel -eq 'install_manifest.txt' -and -not (Test-Path -LiteralPath $dst)) { $op = 'A' }
            $jw.WriteLine("$op $rel")
            if ($op -ne 'A') { Ensure-Parent $bak; [IO.File]::SetAttributes($dst, 'Normal'); [IO.File]::Move($dst, $bak) }
            if ($rel -eq 'install_manifest.txt') { [IO.File]::WriteAllText($dst, $newMan.ToString(), $Utf8) }
            elseif ($op -ne 'D') { Move-Over (Join-Path $pkg (Native $rel)) $dst; Touch-Now $dst }   # now: Ninja must see the change
        }
    } finally { $jw.Dispose(); Write-Progress -Activity 'Installing the update' -Completed }
    # folders left empty by removed files
    foreach ($p in $plan) {
        if ($p[0] -ne 'D') { continue }
        $d = Split-Path -Parent (Join-Path $Root (Native $p[1]))
        while ($d.Length -gt $Root.Length -and (Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $d -Force; $d = Split-Path -Parent $d
        }
    }
    Remove-Item -LiteralPath $newDir -Recurse -Force -ErrorAction SilentlyContinue
    Log "Files installed ($($cnt.R) changed, $($cnt.A) new, $($cnt.D) removed)." 'Green'

    # ---- rebuild
    if (-not $NoSetup) {
        $ft = Join-Path $Root 'tools\fix_timestamps.ps1'
        if (Test-Path -LiteralPath $ft) {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ft -Root $Root
            if ($LASTEXITCODE) { Log "fix_timestamps.ps1 returned $LASTEXITCODE (build.bat runs it again)" 'Yellow' }
        }
        $setup = Join-Path $Root 'setup.ps1'
        $sa = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $setup, '-Unattended')
        if ((Get-Content -Raw -LiteralPath $setup) -match '\[switch\]\$Update\b') { $sa += '-Update' }
        Log 'Running Setup (only what the update needs; the log is logs\setup_script.log) ...'
        Push-Location $Root
        try { & powershell.exe @sa; $code = $LASTEXITCODE } finally { Pop-Location }
        if ($code) { throw "Setup / the build failed (exit code $code); see logs\setup_script.log" }
    }
    Remove-Item -LiteralPath $Upd -Recurse -Force -ErrorAction SilentlyContinue
    $applied = $false
    Log "Updated to $pkgVer." 'Green'
    $ok = $true
} catch {
    $ok = $false
    Log "UPDATE FAILED: $($_.Exception.Message)" 'Red'
    Log "  at $($_.InvocationInfo.PositionMessage -replace '\s+', ' ')"
    try {
        if ($applied) { [void](Invoke-Rollback) }
        elseif ($stage -and (Test-Path -LiteralPath $stage) -and -not (Test-Path -LiteralPath $Journal)) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
    } catch { Log "  rollback error: $($_.Exception.Message)" 'Red' }
    if ($applied -and -not $NoSetup) { Log 'If the launcher says the game needs building, run Setup.bat once.' 'Yellow' }
    Log "Details: logs\update.log.$(if ($Repo) { " Release page: https://github.com/$Repo/releases" })" 'Yellow'
}
$launcher = Join-Path $Root 'Launcher.exe'
if (-not $DryRun -and -not $NoRelaunch -and (Test-Path -LiteralPath $launcher)) {
    if (-not $ok) { Pause-End }
    Start-Process -FilePath $launcher -WorkingDirectory $Root
} elseif (-not $ok) { Pause-End }
if ($ok) { exit 0 } else { exit 1 }
