# Build tools: what Setup installs and why, the record of what it installed, the portable mode and the removal.
# Dot-sourced by setup.ps1 and tools\installer\prereqs.ps1 (the same file in the three Resistance ports).
#
#  * Get-BtPlanText            the explanation Setup shows before it installs anything (tool, why, where from, size)
#  * tools_cache\installed_by_setup.txt
#                              one line per thing Setup itself installed or changed, so it can be undone:
#                                winget|<id>                 installed with winget
#                                direct|<id>                 installed from the pinned official download (prereqs.ps1)
#                                pip|<python.exe>|<package>  a Python package installed with pip
#                                gitconfig|safe.directory|<folder>   added by tools\fetch_rpcs3.ps1
#                                portable|<key>              a portable tool in tools_cache\
#                              Tools that were already on the PC are never listed (and never removed).
#  * Install-PortableTools     EXPERIMENTAL: puts the missing tools into tools_cache\ instead of installing them:
#                              Python (python.org's NuGet package), LLVM, CMake, Ninja, MinGit (zip / tar.xz from
#                              their GitHub releases, SHA checked) and, instead of Visual Studio, the MSVC C/C++
#                              libraries + Windows SDK downloaded from Microsoft with xwin
#                              (https://github.com/Jake-Shadle/xwin). Nothing outside the port folder changes.
#                              tools\toolchain_env.bat (build.bat) and Use-PortableTools pick them up.
#  * Remove-SetupTools         uninstalls / deletes what the manifest lists. -DryRun only prints the plan.

if (-not $script:BtPort) { $script:BtPort = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
function Get-BtCache { return (Join-Path $script:BtPort 'tools_cache') }
function Get-BtManifestPath { return (Join-Path (Get-BtCache) 'installed_by_setup.txt') }
$script:BtLog = ''
# Oldest clang-cl major the port builds with: the committed PGO profile (pgo\*.profdata) is LLVM's indexed profile
# format 13, which LLVM 22 introduced (older clang rejects it). prereqs.ps1 -Check and build.bat use the same minimum.
$script:BtClangMin = 22

# key -> what it is, why the port needs it, where it comes from, rough size (MB) installed / portable
$script:BtInfo = [ordered]@{
    python = @{ Id = 'Python.Python.3.12'; Label = 'Python 3.12'; MB = 150; PortableMB = 100
        Why = 'runs the port''s own scripts: it decrypts your EBOOT.BIN and generates (recompiles) the game code. The Python package pycryptodome (decryption) is added with pip. The Windows installer also adds IDLE, Python''s small editor - that is part of Python, not something extra.'
        From = 'python.org (winget package Python.Python.3.12)' }
    cmake = @{ Id = 'Kitware.CMake'; Label = 'CMake'; MB = 120; PortableMB = 120
        Why = 'reads the port''s build description and writes the build files.'
        From = 'Kitware / cmake.org (winget package Kitware.CMake)' }
    ninja = @{ Id = 'Ninja-build.Ninja'; Label = 'Ninja'; MB = 1; PortableMB = 1
        Why = 'runs the thousands of compile jobs in parallel.'
        From = 'github.com/ninja-build (winget package Ninja-build.Ninja)' }
    clang = @{ Id = 'LLVM.LLVM'; Label = 'LLVM (clang-cl)'; MB = 3000; PortableMB = 2500
        Why = 'the C/C++ compiler that turns the generated game code into a Windows program.'
        From = 'llvm.org / github.com/llvm (winget package LLVM.LLVM)' }
    git = @{ Id = 'Git.Git'; Label = 'Git'; MB = 350; PortableMB = 60
        Why = 'downloads the RPCS3 source code the port''s renderer is built from (tools\fetch_rpcs3.ps1).'
        From = 'git-scm.com / Git for Windows (winget package Git.Git)' }
    vs = @{ Id = 'Microsoft.VisualStudio.2022.BuildTools'; Label = 'Visual Studio 2022 Build Tools (C++)'; MB = 7000; PortableMB = 1500
        Why = 'Microsoft''s C/C++ runtime headers and libraries, the linker and the Windows SDK. Every Windows program is built against them; clang-cl cannot build one without them. Only the command-line tools are installed, not the Visual Studio editor.'
        From = 'Microsoft (winget package Microsoft.VisualStudio.2022.BuildTools)' }
}
$script:BtIdToKey = @{}
foreach ($k in $script:BtInfo.Keys) { $script:BtIdToKey[$script:BtInfo[$k].Id] = $k }
$script:BtIdToKey['VSModify'] = 'vs'

# portable downloads (all pinned and hash checked)
$script:BtMsLicense = 'https://go.microsoft.com/fwlink/?LinkId=2086102'
$script:BtPortable = [ordered]@{
    python = @{ Url = 'https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg'; Alg = 'SHA512B64'
        Hash = 'u9pNz2iKlCEbYtUJaKkbOPMF0LjR7NkCafdKhvigpPzrt8oWKgdTpHaR6z3wyWQAm9PYGUxv0Zr66NX9AeHMDw=='; Dir = 'python'; Exe = 'python\python.exe'
        From = 'python.org''s official NuGet package (api.nuget.org)' }
    clang = @{ Url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.xz'; Alg = 'SHA256'
        Hash = '62fdada7f6fa2b73856486dfbbdba3d2c1882991621615bcb55c60207611c6d4'; Dir = 'llvm'; Exe = 'llvm\bin\clang-cl.exe'
        From = 'LLVM''s GitHub release (~900 MB download)' }
    cmake = @{ Url = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'; Alg = 'SHA256'
        Hash = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'; Dir = 'cmake'; Exe = 'cmake\bin\cmake.exe'
        From = 'Kitware''s GitHub release' }
    ninja = @{ Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'; Alg = 'SHA256'
        Hash = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'; Dir = 'ninja'; Exe = 'ninja\ninja.exe'
        From = 'Ninja''s GitHub release' }
    git = @{ Url = 'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip'; Alg = 'SHA256'
        Hash = 'e3bf78351fe777fba47ddee5950082f8a98bb0f02604fdc7ba3ee2cb68ffa877'; Dir = 'git'; Exe = 'git\cmd\git.exe'
        From = 'Git for Windows'' GitHub release (MinGit)' }
    vs = @{ Url = 'https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-pc-windows-msvc.tar.gz'; Alg = 'SHA256'
        Hash = '96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26'; Dir = 'msvc'; Exe = ''
        From = 'xwin 0.10.0 (GitHub), which downloads the MSVC libraries and the Windows SDK from Microsoft''s own servers (~1 GB)' }
}

function Write-BtMsg([string]$t) {
    Write-Host "   $t"
    if ($script:BtLog) { try { Add-Content -LiteralPath $script:BtLog -Value ('{0:HH:mm:ss} {1}' -f (Get-Date), $t) -Encoding UTF8 } catch {} }
}

# ---------------------------------------------------------------- explanation
function Get-BtPlanText([string[]]$Keys, [switch]$Portable) {
    $o = New-Object System.Collections.ArrayList
    $mb = 0
    foreach ($k in $Keys) {
        $i = $script:BtInfo[$k]; if (-not $i) { continue }
        $size = if ($Portable) { $i.PortableMB } else { $i.MB }; $mb += $size
        $from = if ($Portable) { $script:BtPortable[$k].From } else { $i.From }
        $label = Get-BtLabel $k -Portable:$Portable
        [void]$o.Add(('* {0}  (~{1})' -f $label, (Format-BtMB $size)))
        $why = if ($Portable -and $k -eq 'vs') { 'Microsoft''s C/C++ runtime headers and libraries and the Windows SDK, which every Windows program is built against (clang-cl cannot build one without them). LLVM''s own linker, resource compiler and manifest tool stand in for Microsoft''s.' } else { $i.Why }
        [void]$o.Add("    why:  $why")
        [void]$o.Add("    from: $from")
    }
    [void]$o.Add(('Total: about {0} of disk space{1}.' -f (Format-BtMB $mb), $(if ($Portable) { ' inside this folder (tools_cache\)' } else { ' on drive C:' })))
    return [string[]]$o
}
function Get-BtLabel([string]$Key, [switch]$Portable) {
    if ($Portable -and $Key -eq 'vs') { return 'MSVC C/C++ libraries + Windows SDK (instead of Visual Studio)' }
    return $script:BtInfo[$Key].Label
}
function Format-BtMB([double]$mb) { if ($mb -ge 1000) { return ('{0:0.#} GB' -f ($mb / 1000)) } else { return ('{0} MB' -f [int][Math]::Max(1, $mb)) } }

# ---------------------------------------------------------------- manifest
function Get-BtManifest {
    $m = Get-BtManifestPath
    if (-not (Test-Path -LiteralPath $m)) { return @() }
    $seen = @{}; $out = @()
    foreach ($l in (Get-Content -LiteralPath $m -Encoding UTF8)) {
        $t = "$l".Trim().TrimStart([char]0xFEFF)
        if (-not $t -or $t.StartsWith('#') -or $seen.ContainsKey($t.ToLowerInvariant())) { continue }
        $seen[$t.ToLowerInvariant()] = 1; $out += $t
    }
    return $out
}
function Add-BtManifest([string]$Line) {
    if (@(Get-BtManifest) -contains $Line) { return }
    $m = Get-BtManifestPath
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $m) | Out-Null
        if (-not (Test-Path -LiteralPath $m)) { [IO.File]::WriteAllText($m, "# What Setup installed or changed on this PC (Setup.bat -RemoveTools / Setup.exe --uninstall undo it). Do not edit.`r`n") }
        [IO.File]::AppendAllText($m, "$Line`r`n")
    } catch { Write-BtMsg "(could not write $m`: $_)" }
}
function Set-BtManifest([string[]]$Lines) {
    $m = Get-BtManifestPath
    if (-not $Lines -or -not $Lines.Count) { Remove-Item -LiteralPath $m -Force -ErrorAction SilentlyContinue; return }
    $keep = if (Test-BtMsLicenseAccepted -MarkerOnly) { "$script:BtMsAccepted`r`n" } else { '' }
    [IO.File]::WriteAllText($m, "# What Setup installed or changed on this PC (Setup.bat -RemoveTools / Setup.exe --uninstall undo it). Do not edit.`r`n" + $keep + (($Lines -join "`r`n") + "`r`n"))
}
# Microsoft's licence for the portable MSVC libraries, once accepted, is remembered in the manifest as a '#' line (nothing to
# undo; Setup.exe --prepare and unattended runs may then fetch them again). Forgotten when everything is removed.
$script:BtMsAccepted = '#accepted|microsoft-msvc-license'
function Test-BtMsLicenseAccepted([switch]$MarkerOnly) {
    $m = Get-BtManifestPath
    if ((Test-Path -LiteralPath $m) -and @(Get-Content -LiteralPath $m -Encoding UTF8 | Where-Object { "$_".Trim().TrimStart([char]0xFEFF) -eq $script:BtMsAccepted }).Count) { return $true }
    return ((-not $MarkerOnly) -and (@(Get-BtManifest) -contains 'portable|vs'))
}
function Save-BtMsLicenseAccepted {
    if (Test-BtMsLicenseAccepted -MarkerOnly) { return }
    $m = Get-BtManifestPath
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $m) | Out-Null
        if (-not (Test-Path -LiteralPath $m)) { [IO.File]::WriteAllText($m, "# What Setup installed or changed on this PC (Setup.bat -RemoveTools / Setup.exe --uninstall undo it). Do not edit.`r`n") }
        [IO.File]::AppendAllText($m, "$script:BtMsAccepted`r`n")
    } catch { Write-BtMsg "(could not write $m`: $_)" }
}
# winget exit codes that mean "installed now" (0, reboot needed); "already installed" / "no newer version" are not recorded
function Test-BtFreshInstall([int]$Code) { return ($Code -eq 0 -or $Code -eq 3010 -or $Code -eq -1978334967) }

# ---------------------------------------------------------------- portable layout
# The xwin "splat" layout: crt\include, crt\lib\<arch>, sdk\include\{ucrt,um,shared,winrt,cppwinrt}, sdk\lib\{um,ucrt}\<arch>
# (<arch> is x86_64, or x64 with --preserve-ms-arch-notation). Returns the arch folder name, or '' when incomplete.
function Get-BtMsvcArch([string]$Msvc) {
    if (-not (Test-Path -LiteralPath (Join-Path $Msvc 'crt\include\vcruntime.h'))) { return '' }
    if (-not (Test-Path -LiteralPath (Join-Path $Msvc 'sdk\include\um\windows.h'))) { return '' }
    if (-not (Test-Path -LiteralPath (Join-Path $Msvc 'sdk\include\ucrt\stdio.h'))) { return '' }
    foreach ($a in 'x86_64', 'x64') {
        if ((Test-Path -LiteralPath (Join-Path $Msvc "crt\lib\$a")) -and (Test-Path -LiteralPath (Join-Path $Msvc "sdk\lib\um\$a\kernel32.lib")) -and
            (Test-Path -LiteralPath (Join-Path $Msvc "sdk\lib\ucrt\$a"))) { return $a }
    }
    return ''
}
# What the portable tools add to the environment (the same rules as tools\toolchain_env.bat).
function Get-BtPortableEnv([string]$Cache = (Get-BtCache)) {
    $path = @()
    $msvc = Join-Path $Cache 'msvc'
    $arch = Get-BtMsvcArch $msvc
    if ($arch -and (Test-Path -LiteralPath (Join-Path $Cache 'shims\link.exe'))) { $path += Join-Path $Cache 'shims' }
    foreach ($d in 'llvm\bin', 'cmake\bin', 'ninja', 'python', 'python\Scripts', 'git\cmd') {
        $p = Join-Path $Cache $d
        if (Test-Path -LiteralPath $p) { $path += $p }
    }
    $inc = @(); $lib = @()
    if ($arch) {
        foreach ($d in 'crt\include', 'sdk\include\ucrt', 'sdk\include\um', 'sdk\include\shared', 'sdk\include\winrt') { $inc += Join-Path $msvc $d }
        if (Test-Path -LiteralPath (Join-Path $msvc 'sdk\include\cppwinrt')) { $inc += Join-Path $msvc 'sdk\include\cppwinrt' }
        $lib = @((Join-Path $msvc "crt\lib\$arch"), (Join-Path $msvc "sdk\lib\um\$arch"), (Join-Path $msvc "sdk\lib\ucrt\$arch"))
    }
    return @{ Path = $path; Include = $inc; Lib = $lib; Msvc = [bool]$arch; Arch = $arch }
}
# Puts the portable tools first on this process's PATH (and INCLUDE / LIB for the portable MSVC). Returns $true if any.
function Use-PortableTools([string]$Cache = (Get-BtCache)) {
    if ("$env:PORT_TOOLCHAIN" -eq 'system') { return $false }
    $e = Get-BtPortableEnv $Cache
    if (-not $e.Path.Count) { return $false }
    $env:Path = (($e.Path + @($env:Path -split ';' | Where-Object { $_ -and $e.Path -notcontains $_ })) -join ';')
    if ($e.Msvc) { $env:INCLUDE = $e.Include -join ';'; $env:LIB = $e.Lib -join ';' }
    return $true
}
function Test-BtPortableTool([string]$Key, [string]$Cache = (Get-BtCache)) {
    if ($Key -eq 'vs') { return [bool](Get-BtMsvcArch (Join-Path $Cache 'msvc')) }
    $p = $script:BtPortable[$Key]; if (-not $p) { return $false }
    return (Test-Path -LiteralPath (Join-Path $Cache $p.Exe))
}

# The portable tools to fetch for the missing ones. The portable MSVC libraries are the newest (their C++ headers need
# clang 19 or newer) and the PGO profile needs clang 22 ($BtClangMin): an older installed LLVM gets a portable one next to it.
function Resolve-BtPortableKeys([string[]]$Missing) {
    $k = @($Missing | Where-Object { $script:BtPortable.Contains($_) } | Select-Object -Unique)
    if ($k -contains 'vs' -and $k -notcontains 'clang') {
        $v = Get-BtClangMajor
        if ($v -lt $script:BtClangMin) { $k += 'clang' }
    }
    # install order: Python first (it can unpack .tar.xz), LLVM before the MSVC libraries (the link.exe stand-ins)
    return @(@('python', 'git', 'ninja', 'cmake', 'clang', 'vs') | Where-Object { $k -contains $_ })
}
# An MSYS2/Cygwin cmake reads Windows paths as relative and cannot build the port (tools\cmake_native_probe.cmake;
# build.bat checks it too). prereqs.ps1 -Check reports such a cmake as missing, so Setup.exe asks before installing CMake.
function Test-BtCmakeNative([string]$Exe) {
    $probe = Join-Path $script:BtPort 'tools\cmake_native_probe.cmake'
    if (-not (Test-Path -LiteralPath $probe)) { return $true }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    Push-Location -LiteralPath $script:BtPort
    try { & $Exe -P tools/cmake_native_probe.cmake *> $null; return ($LASTEXITCODE -eq 0) } catch { return $false } finally { Pop-Location; $ErrorActionPreference = $old }
}
function Get-BtClangMajor {
    $c = Get-Command clang-cl -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $exe = if ($c) { $c.Source } elseif (Test-Path -LiteralPath 'C:\Program Files\LLVM\bin\clang-cl.exe') { 'C:\Program Files\LLVM\bin\clang-cl.exe' } else { return 0 }
    $v = "$(& $exe --version 2>$null | Select-Object -First 1)"
    if ($v -match 'version (\d+)\.') { return [int]$Matches[1] }
    return 0
}
# ---------------------------------------------------------------- downloads (mockable in the tests)
function Get-BtUrlFile([string]$Url, [string]$OutFile) {
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path -LiteralPath $curl) {
        & $curl -L --fail --silent --show-error --retry 3 --retry-delay 5 -o $OutFile $Url
        if ($LASTEXITCODE -ne 0) { throw "download failed (curl exit code $LASTEXITCODE): $Url" }
        return
    }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile
}
function Invoke-BtNative([string]$File, [string[]]$Arguments) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $File @Arguments 2>&1 | ForEach-Object { Write-BtMsg "$_" }; return $LASTEXITCODE } finally { $ErrorActionPreference = $old }
}
function Test-BtHash([string]$File, [string]$Alg, [string]$Hash) {
    $a = if ($Alg -like 'SHA512*') { [Security.Cryptography.SHA512]::Create() } else { [Security.Cryptography.SHA256]::Create() }
    $s = [IO.File]::OpenRead($File)
    try { $b = $a.ComputeHash($s) } finally { $s.Dispose(); $a.Dispose() }
    if ($Alg -eq 'SHA512B64') { return ([Convert]::ToBase64String($b) -ceq $Hash) }
    return ((([BitConverter]::ToString($b) -replace '-', '')) -eq $Hash.ToUpperInvariant())
}
function Expand-BtZip([string]$Zip, [string]$Dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dest)
}
# .tar.gz / .tar.xz: Windows' tar.exe (bsdtar), else Python's tarfile
function Expand-BtTar([string]$Tar, [string]$Dest, [string]$Python = '') {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path -LiteralPath $tar) {
        if ((Invoke-BtNative $tar @('-xf', $Tar, '-C', $Dest)) -eq 0) { return }
        Write-BtMsg 'tar.exe could not unpack it; trying Python'
    }
    if (-not $Python) { throw "cannot unpack $Tar (no tar.exe and no Python)" }
    $code = 'import sys,tarfile; t=tarfile.open(sys.argv[1]); kw={"filter":"data"} if hasattr(tarfile,"data_filter") else {}; t.extractall(sys.argv[2],**kw)'
    if ((Invoke-BtNative $Python @('-c', $code, $Tar, $Dest)) -ne 0) { throw "could not unpack $Tar" }
}
# moves the single top folder of an unpacked archive (or the folder itself) to $Dest
function Move-BtTop([string]$From, [string]$Dest, [string]$Sub = '') {
    $src = $From
    $items = @(Get-ChildItem -LiteralPath $From -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $src = $items[0].FullName }
    if ($Sub) { $src = Join-Path $src $Sub }
    if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
    Move-Item -LiteralPath $src -Destination $Dest
}
function New-BtLink([string]$Target, [string]$Link) {
    Remove-Item -LiteralPath $Link -Force -ErrorAction SilentlyContinue
    try { New-Item -ItemType HardLink -Path $Link -Value $Target -ErrorAction Stop | Out-Null } catch { Copy-Item -LiteralPath $Target -Destination $Link -Force }
}

# ---------------------------------------------------------------- portable install
# Installs the given keys (python, clang, cmake, ninja, git, vs) into tools_cache\. Throws on failure.
function Install-PortableTools([string[]]$Keys, [switch]$AcceptMicrosoftLicense, [switch]$DryRun, [string]$Cache = (Get-BtCache)) {
    if ($Keys -contains 'vs' -and -not $AcceptMicrosoftLicense) {
        throw "The portable MSVC libraries are Microsoft's: accept Microsoft's licence ($script:BtMsLicense) first (Setup asks, or -AcceptMicrosoftLicense)."
    }
    if ($Keys -contains 'vs' -and -not $DryRun) { Save-BtMsLicenseAccepted }
    $dl = Join-Path $Cache 'downloads'
    $order = @('python', 'git', 'ninja', 'cmake', 'clang', 'vs') | Where-Object { $Keys -contains $_ }
    foreach ($k in $order) {
        $p = $script:BtPortable[$k]
        if (Test-BtPortableTool $k $Cache) { Write-BtMsg "$(Get-BtLabel $k -Portable): already in tools_cache\"; Add-BtManifest "portable|$k"; continue }
        $file = Join-Path $dl (Split-Path $p.Url -Leaf)
        Write-BtMsg "$(Get-BtLabel $k -Portable): downloading $($p.Url)"
        if ($DryRun) { Write-BtMsg "  (dry run) would check $($p.Alg) and unpack into tools_cache\$($p.Dir)"; continue }
        New-Item -ItemType Directory -Force -Path $dl | Out-Null
        if (-not ((Test-Path -LiteralPath $file) -and (Test-BtHash $file $p.Alg $p.Hash))) {
            Get-BtUrlFile $p.Url $file
            if (-not (Test-BtHash $file $p.Alg $p.Hash)) { Remove-Item -LiteralPath $file -Force; throw "the download of $($p.Url) is damaged or was changed (hash mismatch)" }
        }
        # unpacked into a staging cache (tools_cache\_stage_<key>\<dir>) and moved into place only when complete,
        # so a failed attempt leaves no partial tools_cache\<dir> behind
        $x = Join-Path $Cache "_unpack_$k"
        $sc = Join-Path $Cache "_stage_$k"
        foreach ($d in $x, $sc) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
        New-Item -ItemType Directory -Force -Path $sc | Out-Null
        $sdest = Join-Path $sc $p.Dir
        $dest = Join-Path $Cache $p.Dir
        try {
            switch ($k) {
                'python' { Expand-BtZip $file $x; Move-BtTop $x $sdest 'tools' }
                'git' { Expand-BtZip $file $sdest }
                'ninja' { Expand-BtZip $file $sdest }
                'cmake' { Expand-BtZip $file $x; Move-BtTop $x $sdest }
                'clang' {
                    Expand-BtTar $file $x (Get-BtPython $Cache)
                    Move-BtTop $x $sdest
                    # the build needs bin\ and lib\clang\ (the compiler's own headers); drop the rest (static libraries, ~2 GB)
                    Get-ChildItem -LiteralPath (Join-Path $sdest 'lib') -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'clang' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
                    foreach ($d in 'include', 'libexec', 'share') { Remove-Item -LiteralPath (Join-Path $sdest $d) -Recurse -Force -ErrorAction SilentlyContinue }
                }
                'vs' {
                    Expand-BtTar $file $x (Get-BtPython $Cache)
                    $xw = @(Get-ChildItem -LiteralPath $x -Recurse -Filter 'xwin.exe' | Select-Object -First 1)
                    if (-not $xw) { throw 'xwin.exe not found in its download' }
                    $xwc = Join-Path $Cache 'xwin_cache'
                    Write-BtMsg "  xwin: downloading the MSVC libraries and the Windows SDK from Microsoft (licence accepted: $script:BtMsLicense)"
                    $c = Invoke-BtNative $xw[0].FullName @('--accept-license', '--arch', 'x86_64', '--cache-dir', $xwc, 'splat', '--output', $sdest, '--disable-symlinks')
                    Remove-Item -LiteralPath $xwc -Recurse -Force -ErrorAction SilentlyContinue
                    if ($c -ne 0) { throw "xwin failed (exit code $c)" }
                }
            }
            if (-not (Test-BtPortableTool $k $sc)) { throw "$(Get-BtLabel $k -Portable) is incomplete after unpacking" }
            if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }   # an older, incomplete copy
            Move-Item -LiteralPath $sdest -Destination $dest
            if ($k -eq 'python' -and (Invoke-BtNative (Join-Path $Cache $p.Exe) @('-m', 'ensurepip', '--default-pip')) -ne 0) {
                Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
                throw 'portable Python: ensurepip failed'
            }
        } finally {
            foreach ($d in $x, $sc) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue } }
        }
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        if (-not (Test-BtPortableTool $k $Cache)) { throw "$(Get-BtLabel $k -Portable) is incomplete after unpacking" }
        Add-BtManifest "portable|$k"
        Write-BtMsg "  done: tools_cache\$($p.Dir)"
    }
    if ($Keys -contains 'vs' -and -not $DryRun) { New-BtShims $Cache }
    if (-not $DryRun -and (Test-Path -LiteralPath $dl) -and -not @(Get-ChildItem -LiteralPath $dl -Force).Count) { Remove-Item -LiteralPath $dl -Force }
}
function Get-BtPython([string]$Cache = (Get-BtCache)) {
    $p = Join-Path $Cache 'python\python.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return (Find-BtPython $Cache).Exe
}

# ---- Python 3.10+ for setup.ps1, prereqs.ps1 -Check / -Install and Get-BtPython (player report "python 3.10+ missing, can't
# install"): "py or python on PATH" missed a python.org install without "Add python.exe to PATH", a PATH not refreshed after
# winget, and took the Microsoft Store's python.exe alias stub (WindowsApps) for Python. Now: the portable Python, py -3,
# python / python3 on PATH, the PEP 514 registry entries and the usual install folders, each run once to check its version.
function Test-BtWindowsAppsPath([string]$Path) { return ($Path -match '\\Microsoft\\WindowsApps\\') }
# Runs <exe> [<pre>] -c <probe> with a time limit (an alias stub or the Python install manager may wait for input).
# Returns @{ Version = [version]; Exe = sys.executable } or $null.
function Invoke-BtPyProbe([string]$Exe, [string[]]$Pre = @(), [int]$TimeoutMs = 20000) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (@($Pre) + @('-c', '"import sys;print(''.''.join(map(str,sys.version_info[:3])));print(sys.executable)"')) -join ' '
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardInput = $true
    try { $p = [Diagnostics.Process]::Start($psi) } catch { return $null }
    try {
        $p.StandardInput.Close()
        $out = $p.StandardOutput.ReadToEndAsync(); [void]$p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutMs)) { try { $p.Kill() } catch { }; return $null }
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) { return $null }
        $l = @($out.Result -split "`r?`n" | Where-Object { $_.Trim() })
        if ($l.Count -lt 1 -or $l[0].Trim() -notmatch '^\d+\.\d+\.\d+$') { return $null }
        $exe = if ($l.Count -ge 2) { $l[1].Trim() } else { '' }
        if (-not $exe -or -not (Test-Path -LiteralPath $exe)) { $exe = $Exe }
        return @{ Version = [version]$l[0].Trim(); Exe = $exe }
    } catch { return $null } finally { $p.Dispose() }
}
function ConvertTo-BtPyTagVersion([string]$Tag) { try { return [version](($Tag -replace '[^\d.].*$', '') + '.0') } catch { return [version]'0.0' } }
# PEP 514: <root>\<company>\<tag>\InstallPath (ExecutablePath, or the folder as the default value); newest tag first
function Get-BtPyRegistryExes([string[]]$Roots = @('HKCU:\SOFTWARE\Python', 'HKLM:\SOFTWARE\Python', 'HKLM:\SOFTWARE\WOW6432Node\Python')) {
    $r = @()
    foreach ($root in $Roots) {
        foreach ($co in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            foreach ($tag in @(Get-ChildItem -LiteralPath $co.PSPath -ErrorAction SilentlyContinue | Sort-Object { ConvertTo-BtPyTagVersion $_.PSChildName } -Descending)) {
                $ip = Get-Item -LiteralPath (Join-Path $tag.PSPath 'InstallPath') -ErrorAction SilentlyContinue
                if (-not $ip) { continue }
                $exe = $ip.GetValue('ExecutablePath')
                if (-not $exe) { $d = $ip.GetValue(''); if ($d) { $exe = Join-Path $d 'python.exe' } }
                if ($exe -and (Test-Path -LiteralPath $exe)) { $r += "$exe" }
            }
        }
    }
    return $r
}
# The candidates in the order they are tried: @{ Exe; Pre = launcher arguments; From }
function Get-BtPythonCandidates([string]$Cache = (Get-BtCache)) {
    $c = New-Object Collections.ArrayList
    if ("$env:PORT_TOOLCHAIN" -ne 'system') {
        $p = Join-Path $Cache 'python\python.exe'
        if (Test-Path -LiteralPath $p) { [void]$c.Add(@{ Exe = $p; Pre = @(); From = 'portable (tools_cache)' }) }
    }
    $launchers = @(Get-Command 'py' -CommandType Application -All -ErrorAction SilentlyContinue | ForEach-Object Source) +
                 @("$env:SystemRoot\py.exe", "$env:LOCALAPPDATA\Programs\Python\Launcher\py.exe") | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    foreach ($l in $launchers) { [void]$c.Add(@{ Exe = $l; Pre = @('-3'); From = 'py launcher' }) }
    foreach ($n in 'python', 'python3') {
        foreach ($g in @(Get-Command $n -CommandType Application -All -ErrorAction SilentlyContinue)) { [void]$c.Add(@{ Exe = $g.Source; Pre = @(); From = 'PATH' }) }
    }
    foreach ($e in @(Get-BtPyRegistryExes)) { [void]$c.Add(@{ Exe = $e; Pre = @(); From = 'registry' }) }
    foreach ($pat in @("$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe", "$env:LOCALAPPDATA\Python\pythoncore-3*\python.exe",
                       "$env:ProgramFiles\Python3*\python.exe", "$env:SystemDrive\Python3*\python.exe")) {
        foreach ($f in @(Get-ChildItem -Path $pat -ErrorAction SilentlyContinue | Sort-Object { $n = $_.Directory.Name -replace '^\D+', ''; ConvertTo-BtPyTagVersion $(if ($n -match '^(\d)(\d+)$') { "$($Matches[1]).$($Matches[2])" } else { $n }) } -Descending)) {
            [void]$c.Add(@{ Exe = $f.FullName; Pre = @(); From = 'install folder' })
        }
    }
    return $c.ToArray()
}
# The first Python 3.10+: @{ Exe (full path, '' if none); Version; From; Old (newest older version found); Stub (only the Store alias answered) }
function Find-BtPython([string]$Cache = (Get-BtCache), [object[]]$Candidates = $null, [scriptblock]$Probe = $null) {
    if ($null -eq $Candidates) { $Candidates = @(Get-BtPythonCandidates $Cache) }
    $old = $null; $stub = $false; $seen = @{}
    foreach ($c in $Candidates) {
        $k = ("$($c.Exe)|$(@($c.Pre) -join ' ')").ToLowerInvariant()
        if ($seen.ContainsKey($k)) { continue }; $seen[$k] = 1
        $r = if ($Probe) { & $Probe $c.Exe @($c.Pre) } else { Invoke-BtPyProbe $c.Exe @($c.Pre) }
        if (-not $r) { if (Test-BtWindowsAppsPath $c.Exe) { $stub = $true }; continue }
        if ($r.Version -ge [version]'3.10') { return @{ Exe = "$($r.Exe)"; Version = "$($r.Version)"; From = $c.From; Old = ''; Stub = $false } }
        if (-not $old -or $r.Version -gt $old) { $old = $r.Version }
    }
    return @{ Exe = ''; Version = ''; From = ''; Old = $(if ($old) { "$old" } else { '' }); Stub = $stub }
}
# What to tell the player when Find-BtPython found nothing usable
function Get-BtPythonHelp($Found) {
    $t = 'Python 3.10 or newer was not found on this PC.'
    if ($Found -and $Found.Old) { $t += " Python $($Found.Old) is there, but it is too old." }
    if ($Found -and $Found.Stub) { $t += ' The "python" command is only the Microsoft Store shortcut, not Python: turn it off in Windows Settings > Apps > Advanced app settings > App execution aliases (python.exe and python3.exe).' }
    $t += ' Install Python 3.12 from https://www.python.org/downloads/windows/ (tick "Add python.exe to PATH"), or run Setup again and choose the portable build tools (Setup then downloads its own Python into tools_cache\).'
    return $t
}
# Without Visual Studio there is no link.exe / rc.exe / mt.exe: the LLVM ones stand in (lld-link acts as link.exe
# when it is called link.exe; clang-cl's /link and build.bat's rc call them by these names).
function New-BtShims([string]$Cache = (Get-BtCache)) {
    $bin = Join-Path $Cache 'llvm\bin'
    if (-not (Test-Path -LiteralPath (Join-Path $bin 'lld-link.exe'))) {
        $c = Get-Command 'lld-link' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $c) { $c = Get-Item -LiteralPath 'C:\Program Files\LLVM\bin\lld-link.exe' -ErrorAction SilentlyContinue }
        if (-not $c) { throw 'lld-link.exe (LLVM) not found for the portable linker' }
        $bin = Split-Path -Parent $(if ($c.Source) { $c.Source } else { $c.FullName })
    }
    $sh = Join-Path $Cache 'shims'
    New-Item -ItemType Directory -Force -Path $sh | Out-Null
    foreach ($m in @(@('lld-link.exe', 'link.exe'), @('llvm-rc.exe', 'rc.exe'), @('llvm-mt.exe', 'mt.exe'), @('llvm-lib.exe', 'lib.exe'))) {
        $t = Join-Path $bin $m[0]
        if (Test-Path -LiteralPath $t) { New-BtLink $t (Join-Path $sh $m[1]) }
    }
}

# ---------------------------------------------------------------- removal
function Get-BtWinget { return (Get-Command winget -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1) }
function Invoke-BtUninstallCmd([string]$File, [string]$ArgLine) {
    $p = Start-Process -FilePath $File -ArgumentList $ArgLine -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode
}
function Get-BtArpEntry([string]$Pattern) {
    foreach ($k in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*') {
        $e = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -match $Pattern -and ($_.QuietUninstallString -or $_.UninstallString) } | Select-Object -First 1
        if ($e) { return $e }
    }
    return $null
}
$script:BtArp = @{ 'Python.Python.3.12' = '^Python 3\.12\.\d+ \(64-bit\)$'; 'Kitware.CMake' = '^CMake$'; 'LLVM.LLVM' = '^LLVM$'; 'Git.Git' = '^Git$'
    'Microsoft.VisualStudio.2022.BuildTools' = '^Visual Studio Build Tools 2022' }
# "winget list --id <id> -e" exit code: 0 = installed (separate so the tests can mock it)
function Invoke-BtWingetList([string]$Winget, [string]$Id) {
    return (Invoke-BtUninstallCmd $Winget "list --id $Id -e --accept-source-agreements --disable-interactivity")
}
# Was this package on the PC already, before Setup installs it? Then it is the user's: winget may upgrade it in place
# (an older CMake / LLVM), but it is never recorded for removal. Decided BEFORE the install. $Status: the tool check's
# status for it ('old' = an older copy is there, so it is the user's too).
function Test-BtPresentBefore([string]$Id, [string]$Status = 'missing') {
    if ($Status -and $Status -ne 'missing') { return $true }
    if ($script:BtArp[$Id] -and (Get-BtArpEntry $script:BtArp[$Id])) { return $true }
    if ($Id -eq 'Ninja-build.Ninja' -and (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Ninja\ninja.exe'))) { return $true }
    # only winget's "no installed package found" (0x8A150014) means absent; no winget, an error or another code = unknown,
    # and unknown is treated as present (not recorded: Setup never removes what might be the user's)
    $w = Get-BtWinget
    if (-not $w) { return $true }
    try { $c = Invoke-BtWingetList $w.Source $Id } catch { return $true }
    return ($c -ne -1978335212)
}
# git for undoing a git setting: the one on PATH, else the portable MinGit in tools_cache\
function Get-BtGit([string]$Cache = (Get-BtCache)) {
    $g = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($g) { return $g.Source }
    $p = Join-Path $Cache 'git\cmd\git.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return $null
}
# The user chose the portable build tools before (Setup.bat / update.ps1 unattended runs keep that choice).
function Test-BtPortableChosen([string]$Cache = (Get-BtCache)) {
    if (@(Get-BtManifest | Where-Object { $_ -like 'portable|*' }).Count) { return $true }
    # only complete tools count: a failed attempt's leftovers must not switch later unattended runs to portable mode
    foreach ($k in $script:BtPortable.Keys) { if (Test-BtPortableTool $k $Cache) { return $true } }
    return $false
}
# Step 1's answer without asking: 'P' / 'S' from -PortableTools / -InstallTools. Unattended without them (update.ps1 runs
# setup.ps1 -Unattended after every update): 'P' when the portable tools were chosen before, else 'STOP' - an unattended
# run never installs anything system-wide on its own. '' = ask.
function Get-BtToolMode([switch]$PortableTools, [switch]$InstallTools, [switch]$Unattended, [string]$Cache = (Get-BtCache)) {
    if ($PortableTools) { return 'P' }
    if ($InstallTools) { return 'S' }
    if ($Unattended) { if (Test-BtPortableChosen $Cache) { return 'P' } else { return 'STOP' } }
    return ''
}
# Which manifest lines need administrator rights to undo (machine-wide installs).
function Test-BtNeedsAdmin([string[]]$Lines) { return [bool]@($Lines | Where-Object { $_ -match '^(winget|direct)\|' }).Count }
# Undoes the manifest. Returns the lines that could not be undone (they stay in the manifest).
function Remove-SetupTools([switch]$DryRun, [string]$Cache = (Get-BtCache)) {
    $lines = @(Get-BtManifest)
    $left = @()
    if (-not $lines.Count) { Write-BtMsg 'Setup has not installed any build tools on this PC (nothing listed in tools_cache\installed_by_setup.txt).' }
    $winget = Get-BtWinget
    # VS first is not needed; Python packages before Python itself
    $ordered = @($lines | Where-Object { $_ -like 'pip|*' }) + @($lines | Where-Object { $_ -like 'gitconfig|*' }) + @($lines | Where-Object { $_ -notlike 'pip|*' -and $_ -notlike 'gitconfig|*' })
    foreach ($l in $ordered) {
        $f = $l -split '\|'
        $ok = $true
        try {
            switch ($f[0]) {
                'pip' {
                    Write-BtMsg "Python package $($f[2]): $($f[1]) -m pip uninstall -y $($f[2])"
                    if (-not $DryRun -and (Test-Path -LiteralPath $f[1])) { [void](Invoke-BtNative $f[1] @('-m', 'pip', 'uninstall', '-y', '--disable-pip-version-check', $f[2])) }
                }
                'gitconfig' {
                    Write-BtMsg "Git setting: git config --global --fixed-value --unset-all $($f[1]) `"$($f[2])`""
                    $g = Get-BtGit $Cache
                    # without git the setting cannot be undone: keep it listed (not dropped while the setting stays)
                    if (-not $g) { Write-BtMsg "  git was not found (not on PATH, not in tools_cache\git): kept in the list"; $ok = $false }
                    elseif (-not $DryRun) {
                        $c = Invoke-BtNative $g @('config', '--global', '--fixed-value', '--unset-all', $f[1], $f[2])
                        # 5 = the setting is not there (already removed)
                        $ok = ($c -eq 0 -or $c -eq 5)
                        if (-not $ok) { Write-BtMsg "  git exit code $c" }
                    }
                }
                'portable' {
                    $d = Join-Path $Cache $script:BtPortable[$f[1]].Dir
                    Write-BtMsg "Portable $(Get-BtLabel $f[1] -Portable): delete $d"
                    if ($f[1] -eq 'vs') { Write-BtMsg "  and $(Join-Path $Cache 'shims')" }
                    if (-not $DryRun) {
                        Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
                        if ($f[1] -eq 'vs') { Remove-Item -LiteralPath (Join-Path $Cache 'shims') -Recurse -Force -ErrorAction SilentlyContinue }
                        if (Test-Path -LiteralPath $d) { $ok = $false }
                    }
                }
                { $_ -eq 'winget' -or $_ -eq 'direct' } {
                    $id = $f[1]; $label = if ($script:BtIdToKey[$id]) { $script:BtInfo[$script:BtIdToKey[$id]].Label } else { $id }
                    if ($id -eq 'Ninja-build.Ninja' -and $f[0] -eq 'direct') {
                        $nd = Join-Path $env:ProgramFiles 'Ninja'
                        Write-BtMsg "$label`: delete $nd and remove it from the system PATH"
                        if (-not $DryRun) {
                            Remove-Item -LiteralPath $nd -Recurse -Force -ErrorAction SilentlyContinue
                            $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
                            $n = (@($m -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ne $nd }) -join ';')
                            if ($n -ne $m) { [Environment]::SetEnvironmentVariable('Path', $n, 'Machine') }
                        }
                    } elseif ($winget) {
                        Write-BtMsg "$label`: winget uninstall --id $id -e --silent"
                        if (-not $DryRun) {
                            $c = Invoke-BtUninstallCmd $winget.Source "uninstall --id $id -e --silent --disable-interactivity --accept-source-agreements"
                            # 0, reboot needed, or "no installed package found" (already removed)
                            $ok = ($c -eq 0 -or $c -eq 3010 -or $c -eq -1978334967 -or $c -eq -1978335212)
                            if (-not $ok) { Write-BtMsg "  winget exit code $c" }
                        }
                    } elseif ($id -eq 'Microsoft.VisualStudio.2022.BuildTools') {
                        $vi = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
                        $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
                        $vp = if (Test-Path -LiteralPath $vw) { & $vw -products Microsoft.VisualStudio.Product.BuildTools -property installationPath | Select-Object -First 1 }
                        Write-BtMsg "$label`: `"$vi`" uninstall --installPath `"$vp`" --quiet --norestart"
                        if (-not $DryRun) {
                            if (-not $vp -or -not (Test-Path -LiteralPath $vi)) { $ok = $false }
                            else { $c = Invoke-BtUninstallCmd $vi "uninstall --installPath `"$vp`" --quiet --norestart"; $ok = ($c -eq 0 -or $c -eq 3010) }
                        }
                    } else {
                        $e = if ($script:BtArp[$id]) { Get-BtArpEntry $script:BtArp[$id] }
                        if (-not $e) { Write-BtMsg "$label`: not found in Apps & features (already removed?)" }
                        else {
                            $u = if ($e.QuietUninstallString) { $e.QuietUninstallString } else { $e.UninstallString }
                            if ($u -match '(?i)msiexec' -and $u -match '(\{[0-9A-F-]+\})') { $file = "$env:SystemRoot\System32\msiexec.exe"; $a = "/x $($Matches[1]) /qn /norestart" }
                            elseif ($u -match '^"([^"]+)"\s*(.*)$') { $file = $Matches[1]; $a = $Matches[2] } else { $file = $u; $a = '' }
                            if (-not $e.QuietUninstallString -and $file -match '(?i)unins\d+\.exe$') { $a = "$a /VERYSILENT /NORESTART".Trim() }
                            Write-BtMsg "$label`: $file $a"
                            if (-not $DryRun) { $c = Invoke-BtUninstallCmd $file $a; $ok = ($c -eq 0 -or $c -eq 3010) }
                        }
                    }
                }
                default { Write-BtMsg "(unknown entry kept: $l)"; $ok = $false }
            }
        } catch { Write-BtMsg "  FAILED: $_"; $ok = $false }
        if (-not $ok) { $left += $l }
    }
    if ($DryRun) { return $lines }
    $keepCache = $left.Count -gt 0
    if (-not $keepCache) {
        foreach ($d in 'shims', 'downloads', 'xwin_cache') { Remove-Item -LiteralPath (Join-Path $Cache $d) -Recurse -Force -ErrorAction SilentlyContinue }
    }
    Set-BtManifest $left
    if (-not $keepCache -and (Test-Path -LiteralPath $Cache) -and -not @(Get-ChildItem -LiteralPath $Cache -Force).Count) { Remove-Item -LiteralPath $Cache -Force -ErrorAction SilentlyContinue }
    return $left
}
