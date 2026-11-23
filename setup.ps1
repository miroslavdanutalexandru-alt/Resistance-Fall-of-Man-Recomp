# Resistance: Fall of Man - native PC port: one-time setup.
#
# 1. installs the free build tools (Visual Studio Build Tools, LLVM, CMake,
#    Ninja, Python) with winget, if they are missing
# 2. asks for YOUR copy of the game (a disc image or disc dump folder), checks it and copies it
#    into this folder (gamefiles\)
# 3. decrypts the game's two program files and regenerates the port's game
#    code from them (tools\rfom_regen.py)
# 4. builds the game (build.bat)
#
# Nothing from the game is downloaded: everything game-specific comes from the
# copy you point it at. Run it again any time; finished steps are skipped.
#
# Options (Setup.bat passes them on):
#   -CheckOnly            only checks this PC and lists what Setup would install or do; changes nothing
#   -Game "<path>"        the game copy (.iso or folder); skips the question
#   -FFmpegZip "<zip>"    an FFmpeg 7.1 LGPL shared win64 zip you downloaded yourself (if the pinned link is gone)
#   -NoOptionalSdks       skips the optional NVIDIA DLSS / Intel XeSS downloads (AMD FSR 3.1 still works)
#   -AcceptSdkLicenses    accepts the DLSS / XeSS licences without asking (unattended setup)
#   -Unattended           never waits for keyboard input (Setup.exe runs it so); needs -Game when the game copy
#                         is still missing, skips the optional SDKs unless -AcceptSdkLicenses, makes no shortcut,
#                         and installs no build tools without -InstallTools / -PortableTools (it stops and lists them;
#                         when the portable tools were chosen before, it fetches the missing ones into tools_cache\)
#   -GameCopyMode Copy|Move   with -Unattended: copy or move a game folder on the same drive (default Copy)
#   Build tools (Python, CMake, Ninja, LLVM, Visual Studio Build Tools, Git): Setup lists the missing ones, why they are
#   needed and where they come from, and asks before it installs anything. What it installs is recorded in
#   tools_cache\installed_by_setup.txt (tools that were already on the PC are never recorded or removed).
#   -InstallTools         install the missing build tools system-wide without asking (winget / official installers)
#   -PortableTools        EXPERIMENTAL: put the missing build tools into tools_cache\ in this folder instead of installing
#                         them (no administrator rights, nothing added to Windows; see docs\INSTALL.md)
#   -AcceptMicrosoftLicense   with -PortableTools: accept Microsoft's licence for the MSVC libraries / Windows SDK
#   -RemoveTools          remove the build tools Setup installed (asks first); with -CheckOnly only lists them
#   -GetSdks              add the optional SDKs (DLSS / XeSS / Streamline / FSR 3 FG) to an installed port later: downloads the
#                         missing ones (after their licences are accepted) and builds again, compiling only what uses them
#
# Progress: every step shows what it is doing, how far it is and an estimate of the time left (from the step times
# measured on this PC the last time, in logs\setup_timings.json); the build counts the generated game code by size.

param(
    [string]$Game = '',         # Setup.bat -Game "<.iso or folder>" skips the question (the installer uses it)
    [switch]$CheckOnly,         # check this PC and report what Setup would do; nothing is installed or downloaded
    [string]$FFmpegZip = '',    # a local FFmpeg 7.1 LGPL shared win64 zip, if the pinned download is gone
    [switch]$NoOptionalSdks,    # don't download the optional NVIDIA DLSS / Intel XeSS SDKs
    [switch]$AcceptSdkLicenses, # accept the DLSS / XeSS licences without asking
    [switch]$Unattended,        # Setup.exe: no prompts (see above)
    [ValidateSet('', 'Copy', 'Move')][string]$GameCopyMode = '',
    [switch]$InstallTools,      # install missing build tools system-wide without asking
    [switch]$PortableTools,     # put missing build tools into tools_cache\ instead (experimental)
    [switch]$AcceptMicrosoftLicense,   # with -PortableTools: Microsoft's licence for the MSVC libraries / Windows SDK
    [switch]$RemoveTools,       # remove the build tools Setup installed (-CheckOnly: only list them)
    [switch]$GetSdks            # only download the optional SDKs now (after the install) and rebuild with them
)

# 'Continue': tools like python/winget print warnings on stderr, which
# PowerShell 5 would otherwise turn into a fatal error. Every step checks
# its own result instead.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
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
$Port = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Port
$env:PYTHONUTF8 = '1'
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'logs') | Out-Null
$LogName = if ($CheckOnly) { 'setup_check.log' } elseif ($Unattended) { 'setup_script.log' } else { 'setup.log' }   # Setup.exe keeps logs\setup.log
try { Start-Transcript -Path (Join-Path $Port "logs\$LogName") -Force | Out-Null } catch {}
trap { Write-Host ""; Write-Host "SETUP STOPPED with an unexpected error: $_" -ForegroundColor Red; Write-Host "The details are in logs\$LogName."; try { Stop-Transcript | Out-Null } catch {}; if (-not $CheckOnly -and -not $Unattended) { Read-Host "Press Enter to close" }; exit 1 }

# The supported version: Resistance: Fall of Man, BCUS98107 (US disc), v1.00.
$TitleId = 'BCUS98107'
$Sha1 = @{ 'EBOOT.elf' = 'a33494999e0bc60296bb7ab159d4da420f4e25e5'; 'game.elf' = '45cca7e7ec8e3cec0b9bccc1d204f3cf7159b468' }

$script:Clock = [Diagnostics.Stopwatch]::StartNew()
function Step($t) { if ($script:SpPort) { Enter-SpStep $t }; Write-Host ""; Write-Host ("== {0}   [{1:hh\:mm\:ss} since Setup started, {2:HH:mm}]" -f $t, $script:Clock.Elapsed, (Get-Date)) -ForegroundColor Cyan }
function Info($t) { Write-Host "   $t" }
function Fail($t) {
    Write-Host ""; Write-Host "SETUP STOPPED: $t" -ForegroundColor Red
    Write-Host "Nothing is lost - fix the problem above and run Setup.bat again. (Full log: logs\$LogName)"
    Dismount-GameSource
    if ($script:SpPort) { Write-SpTimingSummary; Save-SpTimings }   # what finished still makes the next estimate better
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $CheckOnly -and -not $Unattended) { Read-Host "Press Enter to close" }; exit 1
}
# Finding, checking and copying the player's game copy (shared with the installer and the other ports).
. (Join-Path $Port 'tools\installer\gamefiles.ps1')
# Build repair (fresh configuration, first error) and disk-space helpers.
. (Join-Path $Port 'tools\build_doctor.ps1')
# Progress bars, time estimates and logs\setup_timings.json (the step durations on this PC, for the next estimates).
. (Join-Path $Port 'tools\installer\progress.ps1')
# What the build tools are for, the record of what Setup installed, the portable tools and their removal.
$script:BtPort = $Port
. (Join-Path $Port 'tools\installer\buildtools.ps1')
$GameProfile = Import-GameProfile (Join-Path $Port 'tools\installer\game_profile.psd1')
function Refresh-Path {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine'); $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u;$env:LOCALAPPDATA\Microsoft\WinGet\Links;C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin"
    [void](Use-PortableTools)   # tools_cache\ (portable mode) first, as in build.bat
}
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
# System-wide installs need the player's yes: the [S] answer in step 1, or -InstallTools (Setup.exe passes it after its own question).
$script:SystemInstallOk = [bool]$InstallTools; $script:ToolMode = $(if ($PortableTools) { 'P' } else { '' })
function Confirm-SystemInstall($id, $label) {
    if ($script:SystemInstallOk) { return }
    if ($Unattended) { Fail "Setup needs to install $label system-wide, but this unattended run has no permission to install anything. In Setup.exe click Install/Resume: it checks the tools again and asks first. Or run Setup.bat (it asks first); on the command line, add -InstallTools." }
    Get-BtPlanText @($(if ($script:BtIdToKey[$id]) { $script:BtIdToKey[$id] })) | ForEach-Object { Info "  $_" }
    if ((Read-Host "Install $label system-wide now (winget / the official installer)? [y/N]") -notmatch '^\s*y') { Fail "$label was not installed. Install it yourself (docs\INSTALL.md, `"Manual install`") and run Setup.bat again." }
    $script:SystemInstallOk = $true
}
function Winget-Install($id, $label, $extra) {
    Confirm-SystemInstall $id $label
    # decided before installing: a package that was there already (winget may upgrade it) is the user's, never removed by Setup
    $fresh = -not (Test-BtPresentBefore $id)
    Info "Installing $label (this can take several minutes)..."
    if (Have 'winget') {
        $a = @('install', '--id', $id, '-e', '--accept-package-agreements', '--accept-source-agreements', '--silent')
        if ($extra) { $a += $extra }
        & winget @a | Out-Host
        $c = $LASTEXITCODE
        Refresh-Path
        if ($fresh -and (Test-BtFreshInstall $c)) { Add-BtManifest "winget|$id" }   # for Setup.bat -RemoveTools (not when it was already there)
        # 0, already installed, no newer version, reboot needed
        if ($c -eq 0 -or $c -eq 3010 -or $c -eq -1978335135 -or $c -eq -1978335189 -or $c -eq -1978334967) { return }
        Info "winget failed (exit code $c). Downloading the official installer instead (Windows asks for permission)..."
    } else {
        Info "winget is not available. Downloading the official installer instead (Windows asks for permission)..."
    }
    # the same pinned, SHA-256 / signature checked downloads Setup.exe uses (tools\installer\prereqs.ps1)
    $pr = Join-Path $Port 'tools\installer\prereqs.ps1'
    $lg = Join-Path $Port 'logs\prereqs_install.log'
    try {
        Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -Wait -ArgumentList `
            "-NoProfile -ExecutionPolicy Bypass -File `"$pr`" -Install -NoWinget -Ids $id -Log `"$lg`""
    } catch { Fail "Could not install $label (the permission prompt was declined?). Install it yourself and run Setup again." }
    if (Test-Path $lg) { Get-Content -LiteralPath $lg -Tail 5 | ForEach-Object { Info $_ } }
    Refresh-Path
}
function Sha1Of($p) { (Get-PortFileHash $p SHA1).ToLower() }

# Parallel jobs for build.bat, planned like Setup.exe does (tools\installer\setup_exe\setup.cpp, PlanJobs): one big
# lifted ppu_recomp_*.cpp needs up to 4 GB of RAM in clang-cl, the other files ~2 GB each. build.bat alone uses
# -j 4 and no out-of-memory retry. Variables the player set (PORT_PPU_JOBS, RFOM_JOBS) are kept.
function Set-BuildJobs([int]$Halve = 0) {
    if (-not $Halve) { $script:UserJobs = [bool]($env:PORT_PPU_JOBS -or $env:RFOM_JOBS) }
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $total = if ($os) { $os.TotalVisibleMemorySize / 1MB } else { 16 }
    $avail = if ($os) { [math]::Max($os.FreePhysicalMemory / 1MB, 1) } else { 8 }
    $cores = [math]::Max([Environment]::ProcessorCount, 1)
    $ppuGB = if ("$env:PORT_PPU_GB" -as [double]) { [double]$env:PORT_PPU_GB } else { 4 }
    $ppu  = [math]::Max(1, [math]::Min([math]::Min([math]::Floor(($total - 6) / $ppuGB), [math]::Floor(($avail - 2) / $ppuGB)), $cores))
    $game = [math]::Max(1, [math]::Min($ppu + [math]::Floor(($avail - $ppuGB * $ppu - 2) / 2), $cores))
    $ppu  = [math]::Min($ppu, $game)
    if (-not $Halve) {
        # never fewer jobs than build.bat / CMake use on their own (-j 4, (RAM - 6 GB) / 8 GB big files): the halving below handles out of memory
        $ppu = [math]::Max($ppu, [math]::Min([math]::Max(1, [math]::Floor(($total - 6) / 8)), $cores))
        $game = [math]::Max($game, 4); $ppu = [math]::Min($ppu, $game)
    }
    for ($i = 0; $i -lt $Halve; $i++) { $ppu = [math]::Max(1, [math]::Floor($ppu / 2)); $game = [math]::Max(1, [math]::Floor($game / 2)) }
    foreach ($v in @(@('PORT_PPU_JOBS', $ppu), @('RFOM_JOBS', $game))) {
        if ($Halve -or -not [Environment]::GetEnvironmentVariable($v[0])) { [Environment]::SetEnvironmentVariable($v[0], "$($v[1])", 'Process') }
    }
    $p = [int]$env:PORT_PPU_JOBS; $g = [int]$env:RFOM_JOBS
    Info ("{0:N0} GB RAM ({1:N1} GB free), {2} logical cores: {3} big game-code file(s) at a time, -j {4}" -f $total, $avail, $cores, $p, $g)
    if (-not (Test-Path (Join-Path $Port 'build\build.ninja'))) {
        # CPU-minutes measured on a 12-thread desktop CPU (docs\INSTALL.md); single big files take up to 3 minutes
        $est = [math]::Max(40 / [math]::Max($p, 1), 3) + 15 / [math]::Max($g, 1)
        Info ("First build: roughly {0:N0}-{1:N0} minutes on this PC (slower CPUs and antivirus scanning take longer). Leave it running;" -f $est, ($est * 1.75))
        Info "later updates only rebuild what changed. More free RAM = more files at once: close browsers and games."
    } else { Info "Rebuilding only what changed." }
}
function Test-BuildOom {
    foreach ($l in @('logs\build.log', 'logs\build_game.log')) {
        $f = Join-Path $Port $l
        if (-not (Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).LastWriteTime -lt $script:BuildStart) { continue }
        if (Get-Content -LiteralPath $f -Tail 400 | Where-Object { $_ -notmatch '^\s*\d*\s*\|' } | Select-String -SimpleMatch -Pattern 'out of memory', 'bad_alloc', 'allocation failed', 'not enough memory', 'paging file', 'insufficient memory', '0xc0000017', 'cannot allocate memory' -Quiet) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------- pinned downloads
# FFmpeg: an LGPL v3 shared build (configured without --enable-gpl). The port only loads avcodec/avutil at run
# time to decode the game's H.264 and MPEG-2 video and AC3 audio; all three decoders are in the LGPL build.
$FFmpegUrl    = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1.zip'
$FFmpegSha256 = '0F376F96FB38554CCEFB1B2AE9C7C6A7B351F0E60A372B38262C320E8392C5D0'
# Optional upscaler SDKs: downloaded from the vendor to this PC only after the player accepts the vendor's licence.
$DlssLicense  = 'https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt'
$XessUrl      = 'https://github.com/intel/xess/releases/download/v2.1.1/XeSS_SDK_2.1.1.zip'
$XessSha256   = 'AFBCD939160CBD7C5C6DB4C4C6F79CAB482492BE23582B938F280D94561C9D40'
$XessLicense  = 'https://github.com/intel/xess/blob/v2.1.1/LICENSE.txt'
# NVIDIA Streamline (DLSS Frame Generation): the SDK is MIT, its DLSS-G / Reflex binaries are under NVIDIA's licence.
$SlUrl        = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
$SlSha256     = '32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B'
$SlLicense    = 'https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/license.txt (and bin/x64/nvngx_dlss.license.txt, reflex.license.txt in the SDK)'
# AMD FidelityFX SDK 2.3.0 frame generation (FSR 3 Frame Generation, the fallback for GPUs without DLSS-G): AMD's signed DLL, MIT.
$FfxFgUrl     = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll'
$FfxFgSha256  = '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18'
# What this port needs from the PC.
$NeedGit   = $false
$DiskGB    = 25          # free space for a first setup (game copy + tools + build), as in README.md
$GameGB    = 16          # of which the game copy
$GpuApi    = 'D3D12'
$GenMarker = 'recompiled_game\ppu_recomp.h'
$UsrFiles  = @('EBOOT.BIN', 'game.self')

function Find-Python {
    # buildtools.ps1 Find-BtPython: py launcher, PATH (not the Store alias stub), registry, install folders, tools_cache\python
    $r = Find-BtPython
    $script:PyFind = $r
    if (-not $r.Exe) { return $null }
    # found outside PATH (registry / install folder): the build (CMake's FindPython3) and the scripts need it on PATH
    $d = Split-Path -Parent $r.Exe
    if (-not (Test-BtWindowsAppsPath $r.Exe) -and (($env:Path -split ';') -notcontains $d)) { $env:Path = "$d;$d\Scripts;$env:Path" }
    return $r.Exe
}
# Python 3.10+ when Find-Python found none: winget / the official installer (or the portable one in portable mode),
# then the portable Python when the installed one still cannot be found
function Install-PythonTool {
    if (-not ($script:ToolMode -eq 'P' -or (-not $script:ToolMode -and (Test-BtPortableChosen)))) {
        Winget-Install 'Python.Python.3.12' 'Python 3.12'
        Refresh-Path
        $p = Find-Python
        if ($p) { return $p }
        Info ("Python 3.12 was installed, but Setup still cannot find it{0}. Using a portable Python in tools_cache\ instead (nothing more is installed in Windows)." -f $(if ($script:PyFind.Stub) { ' (the "python" command is only the Microsoft Store shortcut)' } else { '' }))
    }
    $script:BtLog = Join-Path $Port 'logs\portable_tools.log'
    try { Install-PortableTools @('python') } catch { Info "Could not set up the portable Python: $_ (log: logs\portable_tools.log)" }
    Refresh-Path
    return (Find-Python)
}
function Find-VS {
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vw)) { return $null }
    return (& $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
}
# An MSYS2/Cygwin cmake on PATH cannot build the port (build.bat checks it too); C:\Program Files\CMake goes first in build.bat
function Test-NativeCmake { $c = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $c) { return $false }; return (Test-BtCmakeNative $c.Source) }
# same rule as prereqs.ps1 -Check: a usable cmake is a native one on PATH (tools_cache\ first), or C:\Program Files\CMake
function Test-CmakeOk { if (-not (Have 'cmake')) { return $false }; return ((Test-NativeCmake) -or (Test-Path -LiteralPath "$env:ProgramFiles\CMake\bin\cmake.exe")) }
# the build tools still missing (buildtools.ps1 keys); the portable ones in tools_cache\ count (Refresh-Path adds them)
# clang-cl $script:BtClangMin+ (buildtools.ps1): the committed PGO profile needs it; an older LLVM counts as missing (updated)
# (Get-BtClangMajor checks the clang-cl the build uses: tools_cache\llvm, then Program Files\LLVM, then PATH)
function Test-ClangOk { return ((Get-BtClangMajor) -ge $script:BtClangMin) }
function Get-MissingToolKeys {
    $k = @()
    if (-not (Find-Python)) { $k += 'python' }
    if (-not (Test-CmakeOk)) { $k += 'cmake' }
    if (-not (Have 'ninja')) { $k += 'ninja' }
    if (-not (Test-ClangOk)) { $k += 'clang' }
    if ($NeedGit -and -not (Have 'git')) { $k += 'git' }
    if (-not (Find-VS) -and -not (Get-BtPortableEnv).Msvc) { $k += 'vs' }
    return $k
}
function Test-Admin { return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
# Setup.bat -RemoveTools and the offer at the end: undo what tools_cache\installed_by_setup.txt lists. $true = all removed.
function Invoke-ToolRemoval([switch]$Ask) {
    $lines = @(Get-BtManifest)
    if (-not $lines.Count) { Info "Setup has not installed any build tools on this PC (tools that were already here are never removed)."; return $true }
    Info "Setup installed or changed these on this PC (tools that were already here are not listed and stay):"
    [void](Remove-SetupTools -DryRun)
    if ($Ask) {
        $a = Read-Host "Remove them now? Rebuilding or updating the port needs them again (Setup installs them again then). [y/N]"
        if ($a -notmatch '^\s*y') { Info "Kept."; return $true }
    }
    if ((Test-BtNeedsAdmin $lines) -and -not (Test-Admin)) {
        Info "Windows asks once for administrator permission (the tools were installed for all users)..."
        $lg = Join-Path $Port 'logs\prereqs_uninstall.log'
        try {
            Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -Wait -ArgumentList `
                "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Port 'tools\installer\prereqs.ps1')`" -Uninstall -Log `"$lg`""
        } catch { Info "The permission prompt was declined: nothing was removed."; return $false }
        if (Test-Path -LiteralPath $lg) { Get-Content -LiteralPath $lg -Tail 15 | ForEach-Object { Info $_ } }
    } else {
        $script:BtLog = Join-Path $Port 'logs\prereqs_uninstall.log'
        [void](Remove-SetupTools)
    }
    $left = @(Get-BtManifest)
    if ($left.Count) { Info "Not removed (still listed in tools_cache\installed_by_setup.txt; remove them in Windows Settings > Apps, or run Setup.bat -RemoveTools again): $($left -join ', ')"; return $false }
    Info "Done: the build tools Setup installed are removed."
    return $true
}
function Find-WinSdk {
    try { $root = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction Stop).InstallationFolder } catch { return $null }
    if (-not $root) { return $null }
    $rc = Get-ChildItem (Join-Path $root 'bin\*\x64\rc.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($rc) { return $rc.FullName }; return $null
}
function Test-Avx2 {
    # PF_AVX2_INSTRUCTIONS_AVAILABLE (40). Older Windows 10 builds don't know it and return false.
    try {
        if (-not ('PortSetup.Cpu' -as [type])) {
            Add-Type -Namespace PortSetup -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint f);'
        }
        return [PortSetup.Cpu]::IsProcessorFeaturePresent(40)
    } catch { return $false }
}
function Test-Sse41 {
    # PF_SSE4_1_INSTRUCTIONS_AVAILABLE (37). Older Windows 10 builds don't know it and return false.
    try {
        if (-not ('PortSetup.Cpu' -as [type])) {
            Add-Type -Namespace PortSetup -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint f);'
        }
        return [PortSetup.Cpu]::IsProcessorFeaturePresent(37)
    } catch { return $false }
}
function Test-Sse41Missing {
    # True only when SSE4.1 is known to be missing: Windows reports SSSE3 (PF 36) but not SSE4.1 (PF 37). Older
    # Windows 10 builds don't know PF 36-41 and return false for all of them; that counts as unknown, not missing.
    try {
        if (-not ('PortSetup.Cpu' -as [type])) {
            Add-Type -Namespace PortSetup -Name Cpu -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint f);'
        }
        return ([PortSetup.Cpu]::IsProcessorFeaturePresent(36) -and -not [PortSetup.Cpu]::IsProcessorFeaturePresent(37))
    } catch { return $false }
}
function Test-FFmpegLgpl([string]$dir) {
    # 'lgpl' = the LGPL build Setup installs; 'other' = DLLs from an older setup (a GPL build); 'missing'
    if (-not (Test-Path (Join-Path $dir 'avcodec-61.dll'))) { return 'missing' }
    $lic = Join-Path $dir 'LICENSE.txt'
    if ((Test-Path $lic) -and ((Get-Content -LiteralPath $lic -TotalCount 1) -cmatch 'GNU LESSER GENERAL PUBLIC LICENSE')) { return 'lgpl' }
    return 'other'
}
function Test-Url([string]$u) {
    for ($i = 0; $i -lt 2; $i++) {
        try { Invoke-WebRequest -UseBasicParsing -Method Head -Uri $u -TimeoutSec 30 | Out-Null; return $true } catch { Start-Sleep -Seconds 2 }
    }
    return $false
}
function Confirm-SdkLicense([string]$name, [string]$url) {
    if ($AcceptSdkLicenses) { Info "$name licence accepted with -AcceptSdkLicenses: $url"; return $true }
    if ($Unattended) { Info "Skipping $name (not accepted in Setup). That option will use AMD FSR 3.1 instead."; return $false }
    Write-Host ""
    Write-Host "   Optional download: $name" -ForegroundColor Yellow
    Info "It comes straight from the vendor's GitHub to this PC. It is not part of the port, and using it means"
    Info "accepting the vendor's licence (read it here): $url"
    $a = Read-Host "   Accept that licence and download $name? [Y/n]"
    if ($a -match '^\s*n') { Info "Skipped. That option will use AMD FSR 3.1 instead. Run Setup.bat again to change your mind."; return $false }
    return $true
}

# ---------------------------------------------------------------- PC check (also: Setup.bat -CheckOnly)
$script:Chk = $null
function Chk-Line([string]$tag, [string]$t, [string]$col) { Write-Host ('   [{0,-7}] {1}' -f $tag, $t) -ForegroundColor $col }
function Chk-Ok($t)   { Chk-Line 'OK' $t 'Green' }
function Chk-Note($t) { Chk-Line 'INFO' $t 'Gray' }
function Chk-Warn($t) { $script:Chk.Warnings++; Chk-Line 'WARN' $t 'Yellow' }
function Chk-Err($t)  { $script:Chk.Errors++; if (-not $script:Chk.FirstError) { $script:Chk.FirstError = $t }; Chk-Line 'ERROR' $t 'Red' }
function Chk-Todo($t) { $script:Chk.Todo++; Chk-Line 'WILL DO' $t 'Cyan' }
function Test-ThisPC([switch]$Full) {
    $script:Chk = @{ Errors = 0; Warnings = 0; Todo = 0; FirstError = '' }
    $os = [Environment]::OSVersion.Version
    if (-not [Environment]::Is64BitOperatingSystem) { Chk-Err "Windows is 32-bit. The port needs 64-bit Windows 10 or 11." }
    elseif ($os.Major -lt 10) { Chk-Err "Windows $os is too old. The port needs 64-bit Windows 10 or 11." }
    elseif ($os.Build -lt 17763) { Chk-Warn "Windows 10 build $($os.Build) is old: winget (used to install the build tools) needs build 17763 or newer. Run Windows Update first." }
    else { Chk-Ok ("Windows {0} 64-bit (build {1})" -f $(if ($os.Build -ge 22000) { '11' } else { '10' }), $os.Build) }
    if (Test-IsWine) { Chk-Warn "Wine/Proton detected: Setup and the build are only tested on Windows." }

    $cpu = "$((Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Name)".Trim()
    if (Test-Avx2) { Chk-Ok "CPU with AVX2: $cpu" }
    else { Chk-Warn "Could not confirm AVX2 on this CPU ($cpu). The port is built for AVX2 (Intel 4th gen or newer, any AMD Ryzen); Setup also builds a slower variant without AVX2, which the launcher starts on such CPUs. Older Windows 10 versions can't report AVX2, so this may be fine." }
    if (-not (Test-Avx2) -and (Test-Sse41Missing)) { Chk-Warn "This CPU ($cpu) has no SSE4.1. Every build of the port needs at least SSE4.1 (Intel Core 2 Penryn / 45 nm or newer, AMD Bulldozer / FX or newer); the game says the processor is below the minimum and exits, and Setup does not build the variant without AVX2." }
    elseif (-not (Test-Avx2) -and -not (Test-Sse41)) { Chk-Warn "Could not confirm SSE4.1 on this CPU ($cpu). Every build of the port needs at least SSE4.1 (Intel Core 2 Penryn / 45 nm or newer, AMD Bulldozer / FX or newer); on an older CPU the game says the processor is below the minimum and exits. Older Windows 10 versions can't report SSE4.1, so this may be fine (Setup still builds the variant without AVX2)." }
    $ram = [math]::Round(((Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).TotalPhysicalMemory) / 1GB)
    if ($ram -ge 16) { Chk-Ok "$ram GB RAM" } elseif ($ram -gt 0) { Chk-Warn "$ram GB RAM. The build needs a lot of memory: 16 GB or more is recommended; close other programs while it builds." }

    $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)".Trim() } | Where-Object { $_ -and $_ -notmatch 'Basic Display|Basic Render|Remote|Virtual|Parsec|Meta Virtual' })
    if ($gpus.Count) { Chk-Ok "GPU: $($gpus -join ', ')" } else { Chk-Warn "No graphics driver found (only Windows' basic display driver). Install the latest driver for your GPU." }
    if ($GpuApi -eq 'Vulkan') {
        if (Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll')) { Chk-Ok "Vulkan runtime (vulkan-1.dll)" }
        else { Chk-Warn "vulkan-1.dll is missing, so the GPU driver has no Vulkan support. The game needs a Vulkan 1.3 driver: install the latest one from NVIDIA, AMD or Intel." }
    }
    if (-not ($gpus -match 'RTX')) { Chk-Note "NVIDIA DLSS needs a GeForce RTX card; on this PC use AMD FSR 3.1 or Intel XeSS." }

    $haveGame = $true
    foreach ($n in $UsrFiles) { if (-not (Test-Path (Join-Path $Port "gamefiles\PS3_GAME\USRDIR\$n"))) { $haveGame = $false } }
    # the rest of the game too (PARAM.SFO, game.psarc): a copy with only the EBOOT starts to a black screen
    if (@($GameProfile.Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Port "gamefiles\$_")) }).Count) { $haveGame = $false }
    try {
        $drv = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Port))
        $free = [math]::Floor($drv.AvailableFreeSpace / 1GB)
        $need = if ($haveGame) { $DiskGB - $GameGB } else { $DiskGB }
        if ($free -ge $need) { Chk-Ok "$free GB free on $($drv.Name) (about $need GB needed)" }
        else { Chk-Warn "Only $free GB free on $($drv.Name); about $need GB is needed$(if (-not $haveGame) { " ($GameGB GB of it for the game copy)" }). Free some space or put the port on another drive." }
    } catch { Chk-Warn "Could not read the free space of this drive." }
    if ($Port -match '[^\x20-\x7E]') { Chk-Warn "The folder path has non-English characters. If the build fails, move the port to a plain path like C:\Games\$(Split-Path $Port -Leaf)." }
    if ($Port -match '\\OneDrive') { Chk-Warn "The port is inside OneDrive. Syncing the large build and game folders is slow and can break the build; a folder like C:\Games is better." }
    if ($env:ProgramFiles -and $Port.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) { Chk-Warn "The port is under Program Files, where Windows may block writing. A folder like C:\Games is better." }
    try {
        $pd = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Port))
        if ($pd.DriveType -eq 'Network') { Chk-Warn "The port is on a network drive. The build is much slower there; a local NTFS drive like C:\Games is better." }
        elseif ($pd.DriveFormat -match '^FAT') { Chk-Warn "$($pd.Name) is formatted $($pd.DriveFormat), which can't hold files over 4 GB and makes the build slow. Use an NTFS drive (most internal drives)." }
        elseif ($pd.DriveFormat -notmatch '^(NTFS|ReFS)$') { Chk-Note "$($pd.Name) is formatted $($pd.DriveFormat) (external/USB drives often are). That works, but the build is faster on an NTFS drive." }
    } catch {}
    try {
        $mp = Get-MpComputerStatus -ErrorAction Stop
        if ($mp.RealTimeProtectionEnabled) {
            $ex = @(try { (Get-MpPreference -ErrorAction Stop).ExclusionPath } catch {}) | Where-Object { $_ -and $_ -notmatch '^N/A' -and $Port.StartsWith($_.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase) }
            if (-not $ex) { Chk-Note "Microsoft Defender scans every file the build writes, which can make the first build much slower. Optional: exclude this folder in Windows Security (Virus & threat protection > Manage settings > Exclusions), or in an administrator PowerShell: Add-MpPreference -ExclusionPath '$Port'" }
        }
    } catch {}

    $missing = 0
    $script:py = Find-Python
    if ($script:py) {
        Chk-Ok "Python: $("$(& $script:py --version 2>&1)".Trim())"
        & $script:py -c "import Crypto" 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { Chk-Ok "Python package pycryptodome" } else { Chk-Todo "Install the Python package pycryptodome (pip)." }
    } else { $missing++; Chk-Todo "Install Python 3.12 (winget). $(if ($script:PyFind.Stub) { 'Note: the "python" command is only the Microsoft Store shortcut (Settings > Apps > Advanced app settings > App execution aliases).' } elseif ($script:PyFind.Old) { "Python $($script:PyFind.Old) is too old." })" }
    $tools = @(@('cmake', 'CMake'), @('ninja', 'Ninja'), @('clang-cl', 'LLVM (clang-cl)'))
    if ($NeedGit) { $tools += ,@('git', 'Git') }
    foreach ($t in $tools) {
        $cmd = Get-Command $t[0] -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { Chk-Ok "$($t[1]): $($cmd.Source)" } else { $missing++; Chk-Todo "Install $($t[1]) (winget)." }
    }
    $vsPath = Find-VS
    $pe = Get-BtPortableEnv
    if (-not $vsPath -and $pe.Msvc) { Chk-Ok "MSVC libraries + Windows SDK (portable): tools_cache\msvc" }
    elseif ($vsPath) {
        Chk-Ok "Visual Studio C++ build tools: $vsPath"
        if (Find-WinSdk) { Chk-Ok "Windows SDK" }
        else { Chk-Warn "The Windows 10/11 SDK was not found. The build needs it: open Visual Studio Installer, Modify, and tick a Windows 11 SDK." }
    } else { $missing++; Chk-Todo "Install Visual Studio 2022 Build Tools with the C++ workload (winget; several GB, can take 15+ minutes)." }
    if (Have 'winget') { Chk-Ok "winget" }
    elseif ($missing) { Chk-Note "winget is not available: Setup downloads the official installers for the tools above directly." }
    if ($missing) { Chk-Note "Setup explains each missing tool and asks before installing it: system-wide, or (experimental) portable inside this folder - Setup.bat -PortableTools." }
    $mine = @(Get-BtManifest)
    if ($mine.Count) { Chk-Note "Installed by Setup earlier (Setup.bat -RemoveTools removes them): $($mine -join '; ')" }
    if ($pe.Path.Count) { Chk-Note "Portable build tools in use: $($pe.Path -join '; ')" }

    if (-not $Full) { return }
    if (Test-Url 'https://github.com') { Chk-Ok "Internet (github.com)" } else { Chk-Err "github.com can't be reached. Setup downloads tools and libraries from GitHub: check the internet connection, proxy or firewall." }
    $ff = Test-FFmpegLgpl (Join-Path $Port 'tools\ffmpeg_bin')
    if ($ff -eq 'lgpl') { Chk-Ok "FFmpeg (LGPL build)" }
    else {
        Chk-Todo $(if ($ff -eq 'other') { "Replace the FFmpeg DLLs from an older setup with the LGPL build (~60 MB download)." } else { "Download FFmpeg 7.1, LGPL build (~60 MB, for the game's videos)." })
        if (-not $FFmpegZip -and -not (Test-Url $FFmpegUrl)) { Chk-Err "The pinned FFmpeg download is not available any more: $FFmpegUrl. Download a 'win64-lgpl-shared-7.1' zip from https://github.com/BtbN/FFmpeg-Builds/releases and run Setup.bat -FFmpegZip <zip>." }
    }
    if ((Test-Path (Join-Path $Port 'tools\dlss\lib\nvsdk_ngx_s.lib')) -and (Test-Path (Join-Path $Port 'tools\dlss\nvngx_dlss.dll'))) { Chk-Ok "NVIDIA DLSS SDK (optional)" }
    elseif ($NoOptionalSdks) { Chk-Note "NVIDIA DLSS SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download the NVIDIA DLSS SDK after you accept NVIDIA's licence ($DlssLicense)." }
    if (Test-Path (Join-Path $Port 'tools\xess\bin\libxess.dll')) { Chk-Ok "Intel XeSS SDK (optional)" }
    elseif ($NoOptionalSdks) { Chk-Note "Intel XeSS SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download the Intel XeSS SDK 2.1.1 (~90 MB) after you accept Intel's licence ($XessLicense)." }
    if (Test-Path (Join-Path $Port 'tools\streamline\bin\x64\sl.interposer.dll')) { Chk-Ok "NVIDIA Streamline SDK (optional, DLSS Frame Generation)" }
    elseif ($NoOptionalSdks) { Chk-Note "NVIDIA Streamline SDK: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download NVIDIA Streamline 2.14.16 (~280 MB, DLSS Frame Generation) after you accept NVIDIA's licences ($SlLicense)." }
    if (Test-Path (Join-Path $Port 'tools\ffx_bin\amd_fidelityfx_framegeneration_dx12.dll')) { Chk-Ok "AMD FidelityFX frame generation (optional, FSR 3 Frame Generation)" }
    elseif ($NoOptionalSdks) { Chk-Note "AMD FidelityFX frame generation: skipped (-NoOptionalSdks)." }
    else { Chk-Todo "Optional: download AMD FidelityFX frame generation 2.3.0 (~40 MB, MIT, FSR 3 Frame Generation)." }

    if ($haveGame) { Chk-Ok "Game files: gamefiles\" } else { Chk-Todo "Ask for your copy of the game ($TitleId) and copy it into gamefiles\ ($GameGB GB)." }
    $dec = $true
    foreach ($n in $Sha1.Keys) { $p = Join-Path $Port "game\$n"; if (-not (Test-Path $p) -or (Sha1Of $p) -ne $Sha1[$n]) { $dec = $false } }
    if ($dec) { Chk-Ok "Decrypted program file(s): game\ (version checked)" } else { Chk-Todo "Decrypt the game's program file(s) into game\ and check the version." }
    if (-not (Test-Path (Join-Path $Port $GenMarker))) { Chk-Todo "Generate the game code from your copy." }
    elseif (Test-GenStale $Port 'rfom_regen.py') { Chk-Todo "Generate the game code again (this version's code generator differs from the one that made it)." }
    else { Chk-Ok "Game code generated" }
    if (Test-Path (Join-Path $Port 'Launcher.exe')) { Chk-Todo "Rebuild (only what changed)." } else { Chk-Todo "Build the port and the launcher (the first build takes 20-60 minutes)." }
}

Write-Host "Resistance: Fall of Man - PC port setup" -ForegroundColor Yellow
Write-Host "Folder: $Port"
if ($Port -match ' ') { Info "Note: this folder's path has spaces in it. That works, but a path without spaces (like C:\Games\ResistancePort) is safer." }

# The optional SDKs (NVIDIA DLSS, Intel XeSS, NVIDIA Streamline, AMD FSR 3 frame generation): each downloaded (after its
# licence is accepted) unless it is already there or -NoOptionalSdks. Also Setup.bat -GetSdks, to add them to an installed
# port later.
function Get-OptionalSdks {
    $dl = Join-Path $Port 'tools\dlss'
    $needDlss = -not (Test-Path (Join-Path $dl 'lib\nvsdk_ngx_s.lib')) -or -not (Test-Path (Join-Path $dl 'nvngx_dlss.dll'))
    if ($needDlss -and $NoOptionalSdks) { Info "Skipping the NVIDIA DLSS SDK (-NoOptionalSdks): the DLSS option will use AMD FSR 3.1 instead." }
    elseif ($needDlss -and (Confirm-SdkLicense 'the NVIDIA DLSS SDK v310.9.16' $DlssLicense)) {
        Info "Fetching the NVIDIA DLSS SDK v310.9.16 (optional: the NVIDIA DLSS / DLAA option)..."
        $base = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'
        $hdrs = 'nvsdk_ngx.h','nvsdk_ngx_defs.h','nvsdk_ngx_defs_dlssd.h','nvsdk_ngx_defs_dlssg.h','nvsdk_ngx_defs_vk.h',
                'nvsdk_ngx_helpers.h','nvsdk_ngx_helpers_cuda.h','nvsdk_ngx_helpers_d3d.h','nvsdk_ngx_helpers_dlssd.h',
                'nvsdk_ngx_helpers_dlssd_cuda.h','nvsdk_ngx_helpers_dlssd_d3d.h','nvsdk_ngx_helpers_dlssd_vk.h',
                'nvsdk_ngx_helpers_dlssg.h','nvsdk_ngx_helpers_dlssg_d3d.h','nvsdk_ngx_helpers_dlssg_vk.h','nvsdk_ngx_helpers_vk.h',
                'nvsdk_ngx_loader.h','nvsdk_ngx_params.h','nvsdk_ngx_params_dlssd.h','nvsdk_ngx_params_dlssg.h',
                'nvsdk_ngx_standalone_common.h','nvsdk_ngx_standalone_cuda.h','nvsdk_ngx_vk.h'
        $files = @(@('LICENSE.txt', 'LICENSE.txt'),
                   @('lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', 'lib\nvsdk_ngx_s.lib'),
                   @('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'nvngx_dlss.dll')) + ($hdrs | ForEach-Object { ,@("include/$_", "include\$_") })
        try {
            foreach ($f in $files) {
                $out = Join-Path $dl $f[1]
                New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
                Invoke-WebRequest -UseBasicParsing -Uri "$base/$($f[0])" -OutFile $out
            }
        } catch {
            Info "Could not download the DLSS SDK - building without the NVIDIA DLSS option (FSR still works)."
            if (Test-Path (Join-Path $dl 'lib')) { Remove-Item (Join-Path $dl 'lib') -Recurse -Force }
        }
    }
    $xd = Join-Path $Port 'tools\xess'
    if (-not (Test-Path (Join-Path $xd 'bin\libxess.dll'))) {
        if ($NoOptionalSdks) { Info "Skipping the Intel XeSS SDK (-NoOptionalSdks): the XeSS option will use AMD FSR 3.1 instead." }
        elseif (Confirm-SdkLicense 'the Intel XeSS SDK 2.1.1' $XessLicense) {
            Info "Fetching the Intel XeSS SDK 2.1.1 (~90 MB, optional: the Intel XeSS option)..."
            $zip = Join-Path $env:TEMP "$($TitleId)_xess.zip"; $tmp = Join-Path $env:TEMP "$($TitleId)_xess"
            try {
                Invoke-WebRequest -UseBasicParsing -Uri $XessUrl -OutFile $zip
                if ((Get-PortFileHash $zip SHA256) -ne $XessSha256) { throw "the download is incomplete or not the expected file (SHA-256 mismatch)" }
                if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
                Expand-PortZip -LiteralPath $zip -DestinationPath $tmp
                $dll = Get-ChildItem $tmp -Recurse -Filter 'libxess.dll' | Where-Object { $_.Directory.Name -eq 'bin' } | Sort-Object { $_.FullName.Length } | Select-Object -First 1
                if (-not $dll) { throw "libxess.dll is not in the zip" }
                $src = $dll.Directory.Parent.FullName
                New-Item -ItemType Directory -Force -Path (Join-Path $xd 'bin'), (Join-Path $xd 'inc') | Out-Null
                Copy-Item -LiteralPath (Join-Path $src 'inc\xess') -Destination (Join-Path $xd 'inc') -Recurse -Force
                foreach ($n in 'LICENSE.txt', 'third-party-programs.txt') { Copy-Item -LiteralPath (Join-Path $src $n) -Destination $xd -Force }
                Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $xd 'bin') -Force   # last: marks a complete copy
                Info "Intel XeSS SDK: OK"
            } catch { Info "Could not get the Intel XeSS SDK ($_) - building without the Intel XeSS option (FSR 3.1 is used instead)." }
            Remove-Item $zip, $tmp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    $sd = Join-Path $Port 'tools\streamline'
    if (-not (Test-Path (Join-Path $sd 'bin\x64\sl.interposer.dll'))) {
        if ($NoOptionalSdks) { Info "Skipping NVIDIA Streamline (-NoOptionalSdks): the Frame Generation option stays off." }
        elseif (Confirm-SdkLicense 'NVIDIA Streamline 2.14.16 (DLSS Frame Generation)' $SlLicense) {
            Info "Fetching NVIDIA Streamline 2.14.16 (~280 MB, optional: the Frame Generation option)..."
            $zip = Join-Path $env:TEMP "$($TitleId)_streamline.zip"
            try {
                Invoke-WebRequest -UseBasicParsing -Uri $SlUrl -OutFile $zip
                if ((Get-PortFileHash $zip SHA256) -ne $SlSha256) { throw "the download is incomplete or not the expected file (SHA-256 mismatch)" }
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                $za = [IO.Compression.ZipFile]::OpenRead($zip)
                try {
                    $want = '^(include/[^/]+\.h|license\.txt|3rd-party-licenses\.md|bin/x64/(nvngx_dlss|reflex)\.license\.txt|bin/x64/(sl\.(common|dlss_g|reflex|pcl)|nvngx_dlssg)\.dll)$'
                    $last = $null
                    foreach ($e in $za.Entries) {
                        if ($e.FullName -eq 'bin/x64/sl.interposer.dll') { $last = $e; continue }
                        if ($e.FullName -notmatch $want) { continue }
                        $out = Join-Path $sd ($e.FullName -replace '/', '\')
                        New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
                        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
                    }
                    if (-not $last) { throw "sl.interposer.dll is not in the zip" }
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($last, (Join-Path $sd 'bin\x64\sl.interposer.dll'), $true)   # last: marks a complete copy
                } finally { $za.Dispose() }
                Info "NVIDIA Streamline: OK"
            } catch { Info "Could not get NVIDIA Streamline ($_) - building without the Frame Generation option." }
            Remove-Item $zip -Force -ErrorAction SilentlyContinue
        }
    }
    $fgDll = Join-Path $Port 'tools\ffx_bin\amd_fidelityfx_framegeneration_dx12.dll'
    if (-not (Test-Path $fgDll)) {
        if ($NoOptionalSdks) { Info "Skipping AMD FSR 3 frame generation (-NoOptionalSdks): the FSR 3 Frame Generation choice stays off." }
        else {
            Info "Fetching AMD FidelityFX frame generation 2.3.0 (~40 MB, MIT, optional: the FSR 3 Frame Generation choice)..."
            $tmp = "$fgDll.download"
            try {
                Invoke-WebRequest -UseBasicParsing -Uri $FfxFgUrl -OutFile $tmp
                if ((Get-PortFileHash $tmp SHA256) -ne $FfxFgSha256) { throw "the download is incomplete or not the expected file (SHA-256 mismatch)" }
                Move-Item -Force $tmp $fgDll
                Info "AMD FSR 3 frame generation: OK"
            } catch { Info "Could not get AMD FSR 3 frame generation ($_) - the FSR 3 Frame Generation choice stays off." }
            Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($RemoveTools) {
    Refresh-Path   # with the portable tools: the portable git undoes a safe.directory setting
    Step $(if ($CheckOnly) { "Build tools Setup installed (nothing is removed with -CheckOnly)" } else { "Removing the build tools Setup installed" })
    $ok = if ($CheckOnly) { $l = @(Get-BtManifest); if ($l.Count) { [void](Remove-SetupTools -DryRun) } else { Info "Setup has not installed any build tools on this PC." }; $true } else { Invoke-ToolRemoval -Ask:(-not $Unattended) }
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $Unattended -and -not $CheckOnly) { Read-Host "Press Enter to close" }
    exit $(if ($ok) { 0 } else { 1 })
}
if ($CheckOnly) {
    Step "Checking this PC (nothing will be installed, downloaded or changed)"
    Refresh-Path
    Test-ThisPC -Full
    Write-Host ""
    if ($script:Chk.Errors) {
        Write-Host "CHECK: $($script:Chk.Errors) problem(s) marked ERROR must be fixed before Setup can finish." -ForegroundColor Red
    } else {
        Write-Host "CHECK PASSED ($($script:Chk.Warnings) warning(s)). Setup.bat (without -CheckOnly) would do the WILL DO items above." -ForegroundColor Green
    }
    try { Stop-Transcript | Out-Null } catch {}
    exit $(if ($script:Chk.Errors) { 1 } else { 0 })
}
Initialize-SpProgress $Port -Unattended:$Unattended
if ($GetSdks) {
    # The optional SDKs after the install: download what is missing, then build again. build.bat builds the DLSS and
    # Frame Generation bridges and copies the SDK DLLs whenever their files are there, so the game itself is not
    # compiled again.
    Step "Optional SDKs (NVIDIA DLSS, Intel XeSS, NVIDIA Streamline, AMD FSR 3 frame generation)"
    Refresh-Path
    $sdkMarks = @('tools\dlss\lib\nvsdk_ngx_s.lib', 'tools\xess\bin\libxess.dll', 'tools\streamline\bin\x64\sl.interposer.dll', 'tools\ffx_bin\amd_fidelityfx_framegeneration_dx12.dll')
    $sdkBefore = @($sdkMarks | Where-Object { Test-Path -LiteralPath (Join-Path $Port $_) })
    $NoOptionalSdks = $false
    Get-OptionalSdks
    $sdkNew = @($sdkMarks | Where-Object { (Test-Path -LiteralPath (Join-Path $Port $_)) -and $sdkBefore -notcontains $_ })
    $sdkHave = @($sdkMarks | Where-Object { Test-Path -LiteralPath (Join-Path $Port $_) })
    Info ("Optional SDKs present: {0} of 4{1}." -f $sdkHave.Count, $(if ($sdkNew.Count) { " ($($sdkNew.Count) new)" } else { '' }))
    if (-not (Test-Path -LiteralPath (Join-Path $Port 'recompiled_game\ppu_recomp.h'))) {
        Info "The port is not installed yet: run Setup.bat (or Setup.exe) - it builds with these SDKs."
    } elseif (-not $sdkNew.Count) {
        Info "Nothing new to build with."
    } else {
        Step "5/5  Building"
        Set-BuildJobs
        Repair-MovedBuildDirs $Port
        $script:BuildStart = Get-Date
        Invoke-SpBuild $Port "`"$Port\build.bat`" nopause" @(@('recompiled', 'build'), @('recompiled_game', 'build_game'))
        if ($LASTEXITCODE -ne 0) {
            $e = Get-FirstBuildError $Port $script:BuildStart
            if ($e) { Write-Host ""; Write-Host "   The first error:" -ForegroundColor Yellow; foreach ($l in $e -split "`n") { Write-Host "   $l" -ForegroundColor Yellow } }
            Fail "The build failed - see logs\build.log and logs\build_rpcs3.log (the first error is shown above)."
        }
        Info "Done: the new options are in the launcher's graphics settings."
    }
    Complete-SpStep
    Save-SpTimings
    try { Stop-Transcript | Out-Null } catch {}
    if (-not $Unattended) { Read-Host "Press Enter to close" }
    exit 0
}
Step "Checking this PC"
Refresh-Path
Test-ThisPC
if ($script:Chk.Errors) { Fail $script:Chk.FirstError }

# ---------------------------------------------------------------- 1. tools
Step "1/5  Build tools"
Refresh-Path
$need = @(Get-MissingToolKeys)
if ($need.Count) {
    Info "These free build tools are missing. The port is compiled on this PC from your own game (compiled game code"
    Info "cannot be shared), so it needs a compiler and its helpers:"
    Get-BtPlanText $need | ForEach-Object { Info "  $_" }
    Info "Nothing else is installed, and tools already on this PC are used as they are. Setup records what it installs in"
    Info "tools_cache\installed_by_setup.txt: Setup.bat -RemoveTools (or Setup.exe > Uninstall) removes exactly those again."
    # update.ps1 and other unattended runs never install system-wide on their own: keep the portable choice, or stop
    $mode = Get-BtToolMode -PortableTools:$PortableTools -InstallTools:$InstallTools -Unattended:$Unattended
    $script:ToolMode = $mode
    if ($mode -eq 'P' -and -not $PortableTools) { Info "The portable build tools were chosen before (tools_cache\): fetching the missing ones the same way." }
    if ($mode -eq 'STOP') {
        Fail "These build tools are missing: $((@($need | ForEach-Object { Get-BtLabel $_ })) -join ', '). This unattended run does not install anything without permission. In Setup.exe click Install/Resume (it explains them and asks first), or run Setup.bat (it asks too); on the command line, add -InstallTools (system-wide) or -PortableTools (inside this folder)."
    }
    while (-not $mode) {
        $a = Read-Host "Install them? [S] system-wide, with winget (recommended)  [P] portable: only inside this folder (experimental)  [N] no, stop"
        if ($a -match '^\s*s?\s*$') { $mode = 'S' } elseif ($a -match '^\s*p') { $mode = 'P' } elseif ($a -match '^\s*n') { $mode = 'N' }
    }
    if ($mode -eq 'N') { Fail "Setup needs the build tools listed above. Install them yourself (docs\INSTALL.md, `"Manual install`") or run Setup.bat again and answer S or P." }
    if ($mode -eq 'S') { $script:SystemInstallOk = $true }
    if ($mode -eq 'P') {
        $pk = @(Resolve-BtPortableKeys $need)
        Info "Portable build tools (EXPERIMENTAL): downloaded into $(Get-BtCache) - no installer runs, nothing is added to Windows."
        Get-BtPlanText $pk -Portable | ForEach-Object { Info "  $_" }
        if ($pk -contains 'vs' -and -not $AcceptMicrosoftLicense -and (Test-BtMsLicenseAccepted)) { Info "Microsoft's licence for the MSVC libraries was accepted before on this PC ($script:BtMsLicense)." }
        elseif ($pk -contains 'vs' -and -not $AcceptMicrosoftLicense) {
            Info "The MSVC libraries and the Windows SDK are Microsoft's. xwin downloads them from Microsoft's servers for you,"
            Info "under Microsoft's licence: $script:BtMsLicense"
            if ($Unattended) { Fail "-PortableTools needs -AcceptMicrosoftLicense when it runs unattended (the MSVC libraries are Microsoft's)." }
            if ((Read-Host "Do you accept Microsoft's licence? [y/N]") -notmatch '^\s*y') { Fail "Microsoft's licence was not accepted. Run Setup.bat again and answer S to install Visual Studio Build Tools instead." }
        }
        $script:BtLog = Join-Path $Port 'logs\portable_tools.log'
        try { Install-PortableTools $pk -AcceptMicrosoftLicense } catch { Fail "Could not set up the portable build tools: $_ (log: logs\portable_tools.log). Run Setup.bat again to retry, or answer S to install them system-wide." }
        Refresh-Path
        $need = @(Get-MissingToolKeys)
        if ($need.Count) { Fail "These portable build tools are still missing: $($need -join ', '). See logs\portable_tools.log." }
        Info "Portable build tools: OK (tools_cache\)"
    }
}
$py = Find-Python
if (-not $py) { $py = Install-PythonTool }
if (-not $py) { Fail (Get-BtPythonHelp $script:PyFind) }
Info "Python: $py ($($script:PyFind.Version))"
& $py -c "import Crypto" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Info "Installing pycryptodome (for decrypting the game files)..."
    # the portable Python (tools_cache\python) keeps its packages inside itself; a system Python gets a --user install
    $portablePy = $py.StartsWith((Get-BtCache) + '\', [StringComparison]::OrdinalIgnoreCase)
    $pipArgs = @('-m', 'pip', 'install') + $(if ($portablePy) { @() } else { @('--user') }) + @('--quiet', '--disable-pip-version-check', 'pycryptodome')
    & $py @pipArgs 2>&1 | Out-Host
    & $py -c "import Crypto" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not install the Python package pycryptodome." }
    if (-not $portablePy) { Add-BtManifest "pip|$py|pycryptodome" }
}
# portable mode (chosen now or before) gets the portable CMake, never a system-wide one
function Install-CMakeTool {
    if ($script:ToolMode -eq 'P' -or (-not $script:ToolMode -and (Test-BtPortableChosen))) {
        $script:BtLog = Join-Path $Port 'logs\portable_tools.log'
        try { Install-PortableTools @('cmake') } catch { Fail "Could not set up the portable CMake: $_ (log: logs\portable_tools.log). Run Setup.bat again to retry." }
        Refresh-Path
    } else { Winget-Install 'Kitware.CMake' 'CMake' }
}
if (-not (Have 'cmake')) { Install-CMakeTool }
elseif (-not (Test-CmakeOk)) {
    Info "The cmake found on PATH is an MSYS2/Cygwin build, which cannot build the port: getting CMake for Windows next to it."
    Install-CMakeTool
    if (-not (Test-CmakeOk)) { Fail "CMake for Windows is still not found ahead of the MSYS2/Cygwin cmake. Restart the PC and run Setup.bat again." }
}
if (-not (Have 'ninja')) { Winget-Install 'Ninja-build.Ninja' 'Ninja' }
if (-not (Test-ClangOk)) { Winget-Install 'LLVM.LLVM' "LLVM (clang) $($script:BtClangMin) or newer" }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs -and -not (Get-BtPortableEnv).Msvc) {
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' 'Visual Studio 2022 Build Tools (C++)' @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { Fail "Visual Studio Build Tools did not install (C++ workload)." }
}
foreach ($c in @('cmake', 'ninja', 'clang-cl')) { if (-not (Have $c)) { Fail "$c is still missing after installing it. Restart the PC and run Setup.bat again." } }
if (-not (Test-ClangOk)) { Fail "LLVM (clang-cl) $($script:BtClangMin) or newer is needed (the PGO profile pgo\r1.profdata is LLVM profile format 13), found clang-cl $(Get-BtClangMajor). Update LLVM (winget upgrade LLVM.LLVM, or https://github.com/llvm/llvm-project/releases) and run Setup.bat again." }
Info "Visual Studio C++ tools, LLVM, CMake, Ninja: OK"

# ---------------------------------------------------------------- 2. game copy
Step "2/5  Your copy of the game"
$gf = Join-Path $Port 'gamefiles'
$usr = Join-Path $gf 'PS3_GAME\USRDIR'
if ((Test-Path -LiteralPath "$gf.copying") -or -not (Test-Path (Join-Path $usr 'EBOOT.BIN')) -or -not (Test-Path (Join-Path $usr 'game.self')) -or @($GameProfile.Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $gf $_)) }).Count) {
    if ($Unattended -and -not $Game) { Fail "The game copy is missing or was not copied completely, and no -Game path was given. Pick your game in Setup.exe again." }
    $err = Install-GameFiles $GameProfile $Game $gf $Port -Mode $GameCopyMode
    if ($err) { Fail $err }
}
# the PS3's hard-drive cache and game-data folders the game expects to exist
New-Item -ItemType Directory -Force -Path (Join-Path $gf "cache\$TitleId"), (Join-Path $gf "game\$TitleId\USRDIR") | Out-Null
Info "Game files: $gf"

# ---------------------------------------------------------------- 3. decrypt
Step "3/5  Decrypting the game program files"
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'game') | Out-Null
$need = $false
foreach ($n in $Sha1.Keys) { $p = Join-Path $Port "game\$n"; if (-not (Test-Path $p) -or (Sha1Of $p) -ne $Sha1[$n]) { $need = $true } }
if ($need) {
    $kv = Join-Path $Port 'tools\key_vault.cpp'
    if (-not (Test-Path $kv)) {
        Info "Fetching the PS3 application keys from RPCS3's public source code..."
        try { Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/key_vault.cpp' -OutFile $kv }
        catch { Fail "Could not download key_vault.cpp from github.com (no internet?)." }
    }
    & $py tools\unself.py --keys $kv --out game "$usr\EBOOT.BIN" "$usr\game.self" | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Decrypting EBOOT.BIN / game.self failed." }
    foreach ($n in $Sha1.Keys) {
        if ((Sha1Of (Join-Path $Port "game\$n")) -ne $Sha1[$n]) {
            Fail "game\$n is not the expected version. The port supports the unpatched US disc (BCUS98107 v1.00); a game update or another release won't match."
        }
    }
}
Info "EBOOT.elf and game.elf: OK (BCUS98107 v1.00)"

# ---------------------------------------------------------------- 4. game code
Step "4/5  Generating the game code from your copy"
# Generated again when it is missing, when forced, or when the code generator (its scripts, hooks and the lifter)
# changed since it was generated: tools\build_doctor.ps1 keeps a hash of those files in recompiled\.generator_inputs.
$GenScript = 'rfom_regen.py'; $GenDirs = @('recompiled', 'recompiled_game', 'project\spu')
$genWhy = if ((-not (Test-Path (Join-Path $Port 'recompiled_game\ppu_recomp.h')) -or -not (Test-Path (Join-Path $Port 'project\spu\pm_wwsjob\spu_recomp.c')))) { 'new' } elseif ($env:RFOM_REGEN) { 'forced' } else { Test-GenStale $Port $GenScript }
if ($genWhy) {
    if ($genWhy -eq 'changed') { Info "This version changes the code generator: generating the game code again (10-25 minutes)." }
    elseif ($genWhy -eq 'missing') { Info "Generating the game code again once, so it matches this version's code generator (10-25 minutes)." }
    if ($genWhy -ne 'new') { Info "Files that come out the same keep their date, so the build only compiles what really changed." }
    $genSnap = Save-GenSnapshot $Port $GenDirs
    Remove-GenStamp $Port
    $code = Invoke-SpRegen $py 'tools\rfom_regen.py --eboot game\EBOOT.elf --game game\game.elf' @('eboot:ppu_analyze', 'eboot:ppu_lift', 'eboot:ppu_emit', 'eboot:ppu_hooks', 'eboot:ppu_copy',
        'game:ppu_analyze', 'game:ppu_lift', 'game:ppu_emit', 'game:ppu_post', 'game:ppu_hooks', 'game:ppu_copy', 'spu_lift', 'spu_post')
    if ($code -ne 0) { Fail "Generating the game code failed (see the messages above)." }
    if ($genSnap.Count) { $g = Restore-GenTimes $Port $GenDirs $genSnap; Info ("Generated files: {0} changed, {1} unchanged." -f $g.Changed, $g.Kept) }
    Save-GenStamp $Port $GenScript
} else {
    Set-SpEstimate 'gen' 1; Info "Already generated (set RFOM_REGEN=1 to force)."
    if ((Get-GenPlan $Port $GenScript).Legacy) { Save-GenStamp $Port $GenScript }   # the earlier version's record, for this version's generator
}

# ---------------------------------------------------------------- 5. build
Step "5/5  Building"
$ffd = Join-Path $Port 'tools\ffmpeg_bin'
if ($FFmpegZip -or (Test-FFmpegLgpl $ffd) -ne 'lgpl') {
    if ((Test-FFmpegLgpl $ffd) -eq 'other') { Info "Replacing the FFmpeg DLLs from an older setup with the LGPL build..." }
    $zip = Join-Path $env:TEMP "$($TitleId)_ffmpeg_lgpl.zip"
    if ($FFmpegZip) {
        if (-not (Test-Path -LiteralPath $FFmpegZip)) { Fail "-FFmpegZip: there is no file at $FFmpegZip" }
        $zip = (Resolve-Path -LiteralPath $FFmpegZip).Path
        Info "Using your FFmpeg zip: $zip"
    } else {
        Info "Fetching FFmpeg 7.1, LGPL build (~60 MB, for the game's videos)..."
        try { Invoke-WebRequest -UseBasicParsing -Uri $FFmpegUrl -OutFile $zip }
        catch { Fail "Could not download FFmpeg from github.com ($FFmpegUrl).
If the internet works and that link is gone, download an FFmpeg 7.1 'win64-lgpl-shared-7.1' zip from https://github.com/BtbN/FFmpeg-Builds/releases and run:  Setup.bat -FFmpegZip <path of the zip>" }
        if ((Get-PortFileHash $zip SHA256) -ne $FFmpegSha256) { Remove-Item $zip -Force; Fail "The FFmpeg download is incomplete or not the expected file (SHA-256 mismatch). Run Setup.bat again." }
    }
    $tmp = Join-Path $env:TEMP "$($TitleId)_ffmpeg"; if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-PortZip -LiteralPath $zip -DestinationPath $tmp
    $bin = Get-ChildItem $tmp -Recurse -Filter 'avcodec-61.dll' | Select-Object -First 1
    if (-not $bin) { Fail "This FFmpeg zip has no avcodec-61.dll: it must be an FFmpeg 7.1 shared build (avcodec-61, avutil-59, swresample-5)." }
    $lic = Get-ChildItem $tmp -Recurse -Filter 'LICENSE.txt' | Sort-Object { $_.FullName.Length } | Select-Object -First 1
    if (-not $lic -or (Get-Content -LiteralPath $lic.FullName -TotalCount 1) -cnotmatch 'GNU LESSER GENERAL PUBLIC LICENSE') { Fail "This FFmpeg zip is not an LGPL build (its LICENSE.txt is not the GNU LGPL). Use a 'lgpl-shared' build." }
    if (Test-Path $ffd) { Remove-Item (Join-Path $ffd '*') -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $ffd | Out-Null
    foreach ($n in 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll') {
        $f = Join-Path $bin.DirectoryName $n
        if (-not (Test-Path $f)) { Fail "This FFmpeg zip has no $n." }
        Copy-Item -LiteralPath $f -Destination $ffd
    }
    Copy-Item -LiteralPath $lic.FullName -Destination (Join-Path $ffd 'LICENSE.txt')
    $from = if ($FFmpegZip) { $zip } else { $FFmpegUrl }
    Set-Content -LiteralPath (Join-Path $ffd 'SOURCE.txt') -Encoding ASCII -Value @(
        'FFmpeg shared libraries, LGPL v3 (see LICENSE.txt), unmodified, from:', $from,
        'FFmpeg source code: https://github.com/FFmpeg/FFmpeg (release/7.1). Build scripts: https://github.com/BtbN/FFmpeg-Builds')
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    if (-not $FFmpegZip) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
}
Info "FFmpeg (LGPL build): OK"
Get-OptionalSdks
Set-BuildJobs
Repair-MovedBuildDirs $Port
$script:BuildStart = $script:BuildStart0 = Get-Date
$BuildPairs = @(@('recompiled', 'build'), @('recompiled_game', 'build_game'))   # the generated PPU code and its build folders (for the build progress)
Invoke-SpBuild $Port "`"$Port\build.bat`" nopause" $BuildPairs
for ($h = 1; $LASTEXITCODE -ne 0 -and $h -le 2 -and -not $script:UserJobs -and (Test-BuildOom); $h++) {
    Info "The compiler ran out of memory. Trying again with fewer files at a time (the build continues where it stopped)..."
    Set-BuildJobs -Halve $h
    $script:BuildStart = Get-Date
    Invoke-SpBuild $Port "`"$Port\build.bat`" nopause" $BuildPairs
}
# Not out of memory and not an error in the code ("build.ninja still dirty", a moved folder, a stale CMake cache):
# one more try with a fresh build configuration. The files already compiled are kept.
if ($LASTEXITCODE -ne 0 -and -not (Test-BuildOom) -and ((Test-BuildConfigProblem $Port $script:BuildStart) -or -not (Test-BuildCodeError $Port $script:BuildStart))) {
    $e = Get-FirstBuildError $Port $script:BuildStart
    if ($e) { Info "The build stopped at:"; foreach ($l in $e -split "`n") { Info $l } }
    Info "Trying once more with a fresh build configuration (the files already compiled are kept)..."
    Reset-BuildConfig $Port
    $script:BuildStart = Get-Date
    Invoke-SpBuild $Port "`"$Port\build.bat`" nopause" $BuildPairs
}
if ($LASTEXITCODE -ne 0) {
    $e = Get-FirstBuildError $Port $script:BuildStart
    if ($e) { Write-Host ""; Write-Host "   The first error:" -ForegroundColor Yellow; foreach ($l in $e -split "`n") { Write-Host "   $l" -ForegroundColor Yellow } }
    Fail "The build failed - see logs\build.log and logs\build_game.log (the first error is shown above)."
}
# No AVX2 confirmed on this CPU: also build the variant without AVX2 (build_cpu.bat baseline -> build_baseline\);
# run.bat starts it when the launcher finds no AVX2. Not fatal: the AVX2 build is complete either way.
# Not when SSE4.1 is known to be missing: the baseline variant needs SSE4.1, so it could not run either. When
# Windows can't tell (older Windows 10), it is built; the exe's own CPU check explains a CPU that is too old.
if (-not (Test-Avx2) -and (Test-Sse41Missing)) {
    Write-Host "   This CPU has neither AVX2 nor SSE4.1. SSE4.1 is the minimum of every build of the port, so the variant without AVX2 is not built either." -ForegroundColor Yellow
} elseif (-not (Test-Avx2)) {
    Info "Also building the variant for CPUs without AVX2 (build_cpu.bat baseline, into build_baseline\)..."
    & cmd /c "`"$Port\build_cpu.bat`" baseline nopause"
    if ($LASTEXITCODE -ne 0) { Info "The build without AVX2 failed (see logs\build_build_baseline*.log); on a CPU without AVX2 the game will not start." }
}
Info ("The build took {0:hh\:mm\:ss}." -f ((Get-Date) - $script:BuildStart0))
Remove-SetupLeftovers $Port $TitleId
Show-DiskUsage $Port
# build.bat can finish without an exe (the launcher build failing is not fatal there, or an antivirus
# quarantined the file): stop here instead of reporting success and failing to start Launcher.exe.
$missing = @('build\ResistanceFoM.exe', 'build_game\ResistanceFoMGame.exe', 'Launcher.exe' | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Port $_)) })
if ($missing) { Fail "The build finished but $($missing -join ', ') is missing - an antivirus may have quarantined it (check its quarantine/history, restore the file and add the port folder to the exclusions), or see logs\build.log." }

Complete-SpStep
Write-SpTimingSummary
Save-SpTimings
Write-Host ""
Write-Host "SETUP COMPLETE. Start the game with Launcher.exe." -ForegroundColor Green
if (@(Get-BtManifest | Where-Object { $_ -match '^(winget|direct|portable)\|' }).Count) {
    Info "The build tools Setup installed are only needed to build the port. Keep them if you want to update or rebuild it"
    Info "later; Setup.bat -RemoveTools (or Setup.exe > Uninstall) removes them any time."
    if (-not $Unattended) { [void](Invoke-ToolRemoval -Ask) }
}
try { Stop-Transcript | Out-Null } catch {}
if (-not $Unattended) {
    Start-Process (Join-Path $Port 'Launcher.exe')
    Read-Host "Press Enter to close"
}
