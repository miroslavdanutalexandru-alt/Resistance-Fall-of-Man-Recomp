# Tests for tools\installer\buildtools.ps1 and tools\toolchain_env.bat without the internet and without changing the PC:
# fake tools_cache layouts in build_test_setup\ (git-ignored), downloads / unpacking / uninstallers replaced by mocks.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\tests\test_buildtools.ps1
$ErrorActionPreference = 'Stop'
$Port = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$Root = Join-Path $Port 'build_test_setup\buildtools'
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Root | Out-Null

$script:Failed = 0
function Check($Ok, [string]$What) {
    $Ok = [bool]@($Ok | Where-Object { $_ }).Count
    if ($Ok) { Write-Host "  ok   $What" } else { Write-Host "  FAIL $What"; $script:Failed++ }
}
function Touch([string]$p, [string]$text = 'x') { New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $text) }

. (Join-Path $Port 'tools\installer\buildtools.ps1')
$script:BtPort = Join-Path $Root 'port'                 # every tools_cache path below lives in the test folder
New-Item -ItemType Directory -Force -Path $script:BtPort | Out-Null
$Cache = Get-BtCache
Check ($Cache -eq (Join-Path $Root 'port\tools_cache')) 'tools_cache follows the port folder'

Write-Host '-- explanation'
$plan = Get-BtPlanText @('python', 'vs')
Check (@($plan -match 'IDLE').Count) 'Python entry explains IDLE'
Check (@($plan -match 'from: Microsoft').Count) 'VS entry says where it comes from'
Check (@($plan -match 'on drive C:').Count) 'system plan: size on C:'
$pplan = Get-BtPlanText @('python', 'vs') -Portable
Check (@($pplan -match 'xwin').Count -and @($pplan -match 'tools_cache').Count) 'portable plan: xwin and tools_cache'

Write-Host '-- manifest'
Add-BtManifest 'winget|Kitware.CMake'
Add-BtManifest 'winget|Kitware.CMake'
Add-BtManifest 'pip|C:\Py\python.exe|pycryptodome'
$m = @(Get-BtManifest)
Check ($m.Count -eq 2 -and $m[0] -eq 'winget|Kitware.CMake') 'add + dedupe'
# a line appended by fetch_rpcs3.ps1 (Add-Content -Encoding UTF8 writes a BOM on a new file)
Remove-Item (Get-BtManifestPath)
Add-Content -LiteralPath (Get-BtManifestPath) -Value 'gitconfig|safe.directory|F:/Porty/x' -Encoding UTF8
Add-BtManifest 'gitconfig|safe.directory|F:/Porty/x'
$m = @(Get-BtManifest)
Check ($m.Count -eq 1 -and $m[0] -eq 'gitconfig|safe.directory|F:/Porty/x') 'BOM line from fetch_rpcs3.ps1 read and not duplicated'
Set-BtManifest @()
Check (-not (Test-Path (Get-BtManifestPath))) 'empty manifest removes the file'
Check ((Test-BtFreshInstall 0) -and (Test-BtFreshInstall 3010) -and -not (Test-BtFreshInstall -1978335135) -and -not (Test-BtFreshInstall -1978335189)) 'already-installed winget codes are not recorded'

Write-Host '-- portable layout'
function New-FakeMsvc([string]$c, [string]$arch) {
    foreach ($f in 'msvc\crt\include\vcruntime.h', 'msvc\sdk\include\um\windows.h', 'msvc\sdk\include\ucrt\stdio.h', 'msvc\sdk\include\shared\x.h', "msvc\sdk\lib\um\$arch\kernel32.lib", "msvc\crt\lib\$arch\msvcrt.lib", "msvc\sdk\lib\ucrt\$arch\ucrt.lib") { Touch (Join-Path $c $f) }
}
$c1 = Join-Path $Root 'c1 (x86)'; New-FakeMsvc $c1 'x86_64'; Touch "$c1\shims\link.exe"; Touch "$c1\llvm\bin\clang-cl.exe"; Touch "$c1\ninja\ninja.exe"
$c2 = Join-Path $Root 'c2'; New-FakeMsvc $c2 'x64'; Touch "$c2\shims\link.exe"; Touch "$c2\msvc\sdk\include\cppwinrt\w.h"
$c3 = Join-Path $Root 'c3'; New-FakeMsvc $c3 'x86_64'; Remove-Item "$c3\msvc\sdk\lib\um\x86_64\kernel32.lib"; Touch "$c3\cmake\bin\cmake.exe"
Check ((Get-BtMsvcArch "$c1\msvc") -eq 'x86_64') 'xwin layout x86_64'
Check ((Get-BtMsvcArch "$c2\msvc") -eq 'x64') 'xwin layout x64 (--preserve-ms-arch-notation)'
Check ((Get-BtMsvcArch "$c3\msvc") -eq '') 'incomplete layout rejected'
$e3 = Get-BtPortableEnv $c3
Check (-not $e3.Msvc -and $e3.Path.Count -eq 1 -and $e3.Path[0] -like '*cmake\bin') 'incomplete MSVC: only the tool folders'

# toolchain_env.bat must agree with Get-BtPortableEnv
$bat = Join-Path $Port 'tools\toolchain_env.bat'
foreach ($c in $c1, $c2) {
    $e = Get-BtPortableEnv $c
    $out = @(& cmd /c "set PORT_TOOLCHAIN=&& set PORT_TOOLS_CACHE=$c&& call `"$bat`" >nul && echo RC=OK&& set INCLUDE&& set LIB&& set PATH" 2>&1)
    $get = { param($n) $l = @($out | Where-Object { $_ -like "$n=*" }) | Select-Object -First 1; if ($l) { $l.Substring($n.Length + 1) } else { '' } }
    $bp = (& $get 'PATH') -split ';'
    Check (@($out -match '^RC=OK').Count) "toolchain_env.bat ok for $(Split-Path $c -Leaf)"
    Check ((& $get 'INCLUDE') -eq ($e.Include -join ';')) "INCLUDE matches ($(Split-Path $c -Leaf))"
    Check ((& $get 'LIB') -eq ($e.Lib -join ';')) "LIB matches ($(Split-Path $c -Leaf))"
    Check ((($bp | Select-Object -First $e.Path.Count) -join ';') -eq ($e.Path -join ';')) "PATH starts with the portable folders ($(Split-Path $c -Leaf))"
}
$out = @(& cmd /c "set PORT_TOOLS_CACHE=$c1&& set PORT_TOOLCHAIN=system&& call `"$bat`" >nul 2>nul & set PATH" 2>&1)
Check (-not @($out -match [regex]::Escape("$c1\shims")).Count) 'PORT_TOOLCHAIN=system ignores tools_cache'
Remove-Item "$c1\shims\link.exe"
$out = @(& cmd /c "set PORT_TOOLCHAIN=&& set PORT_TOOLS_CACHE=$c1&& call `"$bat`" && echo RC=OK" 2>&1)
Check (@($out -match 'incomplete').Count -and -not @($out -match '^RC=OK').Count) 'missing link shim stops the build with a message'
Touch "$c1\shims\link.exe"

$savedPath = $env:Path; $savedInc = $env:INCLUDE; $savedLib = $env:LIB
try {
    Check (Use-PortableTools $c1) 'Use-PortableTools: used'
    Check ($env:Path -like "$c1\shims;*" -and $env:INCLUDE -like "$c1\msvc\crt\include;*") 'Use-PortableTools: PATH and INCLUDE'
    $env:Path = $savedPath; $env:PORT_TOOLCHAIN = 'system'
    Check (-not (Use-PortableTools $c1) -and $env:Path -eq $savedPath) 'Use-PortableTools: PORT_TOOLCHAIN=system'
} finally { $env:Path = $savedPath; $env:INCLUDE = $savedInc; $env:LIB = $savedLib; $env:PORT_TOOLCHAIN = $null }

Write-Host '-- unpacking (the real Expand-BtTar; the portable install below mocks it)'
$tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
if (Test-Path -LiteralPath $tarExe) {
    $tsrc = Join-Path $Root 'tar_src'; Touch "$tsrc\top\hello.txt" 'hola'
    $tgz = Join-Path $Root 'tar_test.tar.gz'
    & $tarExe -czf $tgz -C $tsrc top
    $tout = Join-Path $Root 'tar_out'
    try { Expand-BtTar $tgz $tout } catch { Write-Host "    $_" }
    Check ((Test-Path "$tout\top\hello.txt") -and (Get-Content "$tout\top\hello.txt") -eq 'hola') 'Expand-BtTar unpacks the archive with tar.exe (not tar.exe itself: $tar / $Tar are one variable)'
} else { Write-Host '  skip Expand-BtTar: no tar.exe' }

Write-Host '-- Python probe'
# cmd.exe stands in for python: it prints a version and a sys.executable that does not exist
$cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
$pr = Invoke-BtPyProbe $cmdExe @('/c', 'echo 3.12.1&echo C:\no\such\python.exe&rem')
Check ($pr -and $pr.Version -eq [version]'3.12.1' -and $pr.Exe -eq $cmdExe) 'probe: a sys.executable that does not exist falls back to the probed exe ($exe / $Exe are one variable)'

Write-Host '-- which clang-cl'
$cc = Join-Path $Cache 'llvm\bin\clang-cl.exe'; Touch $cc
Check ((Find-BtClangCl) -eq $cc) 'tools_cache\llvm first, as tools\toolchain_env.bat puts it on PATH'
Remove-Item -LiteralPath (Join-Path $Cache 'llvm') -Recurse -Force
$pfc = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
if (Test-Path -LiteralPath $pfc) { Check ((Find-BtClangCl) -eq $pfc) 'then Program Files\LLVM, ahead of any other clang-cl on PATH' }
else { Write-Host '  skip Program Files\LLVM order: no LLVM there' }

Write-Host '-- portable key choice'
function Get-BtClangMajor { return 18 }
Check ((@(Resolve-BtPortableKeys @('vs')) -join ',') -eq 'clang,vs') 'portable MSVC with clang 18 installed: portable LLVM too'
function Get-BtClangMajor { return 21 }
Check ((@(Resolve-BtPortableKeys @('vs')) -join ',') -eq 'clang,vs') 'portable MSVC with clang 21 installed (cannot read the PGO profile): portable LLVM too'
function Get-BtClangMajor { return 22 }
Check ((@(Resolve-BtPortableKeys @('vs', 'ninja', 'bogus')) -join ',') -eq 'ninja,vs') 'clang 22 installed: kept; unknown keys dropped'
Write-Host '-- hashes'
$hf = Join-Path $Root 'hash.bin'; [IO.File]::WriteAllText($hf, 'abc')
Check (Test-BtHash $hf 'SHA256' 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'SHA-256 hex'
Check (Test-BtHash $hf 'SHA512B64' '3a81oZNherrMQXNJriBBMRLm+k6JqX6iCp7u5ktV05ohkpkqJ0/BqDa6PCOj/uu9RU1EI2Q86A4qmslPpUyknw==') 'SHA-512 base64 (NuGet)'
Check (-not (Test-BtHash $hf 'SHA256' ('0' * 64))) 'hash mismatch detected'

Write-Host '-- unpacking a real .tar.gz'
$tsrc = Join-Path $Root 'tar src'; Touch (Join-Path $tsrc 'xwin-1\xwin.exe') 'w'
$tgz = Join-Path $Root 'x w.tar.gz'
& (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $tgz -C $tsrc 'xwin-1'
$tx = Join-Path $Root 'unpack tar'
Expand-BtTar $tgz $tx
Check ((Get-Content -Raw (Join-Path $tx 'xwin-1\xwin.exe')) -eq 'w') 'tar.exe unpacks the archive (not tar.exe itself)'
$py = (Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch '\\WindowsApps\\' } | Select-Object -First 1).Source
if ($py) {
    $px = Join-Path $Root 'unpack py'; Expand-BtTar $tgz $px $py -TarExe ''
    Check ((Get-Content -Raw (Join-Path $px 'xwin-1\xwin.exe')) -eq 'w') 'no tar.exe: Python tarfile unpacks it'
} else { Write-Host '  skip Python tarfile fallback (no python on PATH)' }

Write-Host '-- portable install (mocked downloads)'
# local stand-ins for the pinned archives, with their real hashes
$src = Join-Path $Root 'src'
function New-Zip([string]$name, [hashtable]$files) {
    $d = Join-Path $src "z_$name"; foreach ($k in $files.Keys) { Touch (Join-Path $d $k) $files[$k] }
    $z = Join-Path $src $name; Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::CreateFromDirectory($d, $z); $z
}
function HashOf([string]$f, [switch]$B64) {
    if ($B64) { $a = [Security.Cryptography.SHA512]::Create(); return [Convert]::ToBase64String($a.ComputeHash([IO.File]::ReadAllBytes($f))) }
    $a = [Security.Cryptography.SHA256]::Create(); ([BitConverter]::ToString($a.ComputeHash([IO.File]::ReadAllBytes($f))) -replace '-', '').ToLower()
}
$zips = @{
    ninja = New-Zip 'ninja-win.zip' @{ 'ninja.exe' = 'n' }
    cmake = New-Zip 'cmake.zip' @{ 'cmake-4.4.4-windows-x86_64\bin\cmake.exe' = 'c' }
    python = New-Zip 'python.nupkg' @{ 'tools\python.exe' = 'p'; 'python.nuspec' = 's' }
    git = New-Zip 'mingit.zip' @{ 'cmd\git.exe' = 'g' }
    clang = New-Zip 'llvm.tar.xz' @{ 'dummy' = 'tar' }
    vs = New-Zip 'xwin.tar.gz' @{ 'dummy' = 'tar' }
}
foreach ($k in $zips.Keys) {
    $script:BtPortable[$k].Url = 'https://example.invalid/' + (Split-Path $zips[$k] -Leaf)
    if ($k -eq 'python') { $script:BtPortable[$k].Hash = HashOf $zips[$k] -B64 } else { $script:BtPortable[$k].Hash = HashOf $zips[$k] }
}
$script:Downloads = @()
function Get-BtUrlFile([string]$Url, [string]$OutFile) { $script:Downloads += $Url; Copy-Item -LiteralPath (Join-Path $src (Split-Path $Url -Leaf)) -Destination $OutFile -Force }
function Expand-BtTar([string]$Tar, [string]$Dest, [string]$Python = '') {
    if ($Tar -like '*llvm*') {
        foreach ($f in 'bin\clang-cl.exe', 'bin\lld-link.exe', 'bin\llvm-rc.exe', 'bin\llvm-mt.exe', 'bin\llvm-lib.exe', 'lib\clang\23\include\x86intrin.h', 'lib\LLVMCore.lib', 'include\llvm\x.h') { Touch (Join-Path $Dest "clang+llvm-23.1.5-x86_64-pc-windows-msvc\$f") }
    } else { Touch (Join-Path $Dest 'xwin-0.10.0-x86_64-pc-windows-msvc\xwin.exe') }
}
$script:Native = @()
function Invoke-BtNative([string]$File, [string[]]$Arguments) {
    $script:Native += ,(@($File) + $Arguments)
    if ($File -like '*xwin.exe') { $o = $Arguments[[array]::IndexOf($Arguments, '--output') + 1]; New-FakeMsvc (Split-Path $o) 'x86_64'; Touch (Join-Path $Arguments[[array]::IndexOf($Arguments, '--cache-dir') + 1] 'big.cab') }
    return 0
}
$all = @('python', 'git', 'ninja', 'cmake', 'clang', 'vs')
$threw = $false; try { Install-PortableTools $all } catch { $threw = $_ -match 'licence' }
Check ($threw -and -not $script:Downloads.Count) 'MSVC libraries need the Microsoft licence accepted first'
Install-PortableTools $all -AcceptMicrosoftLicense -DryRun
Check (-not (Test-Path (Join-Path $Cache 'llvm')) -and -not $script:Downloads.Count) 'dry run downloads and unpacks nothing'
Install-PortableTools $all -AcceptMicrosoftLicense
foreach ($k in $all) { Check (Test-BtPortableTool $k) "portable $k installed" }
Check ($script:Downloads.Count -eq 6) 'six downloads'
Check ((Test-Path "$Cache\llvm\lib\clang\23\include\x86intrin.h") -and -not (Test-Path "$Cache\llvm\lib\LLVMCore.lib") -and -not (Test-Path "$Cache\llvm\include")) 'LLVM: keeps bin + lib\clang only'
Check ((Test-Path "$Cache\shims\link.exe") -and (Test-Path "$Cache\shims\rc.exe") -and (Test-Path "$Cache\shims\mt.exe") -and (Test-Path "$Cache\shims\lib.exe")) 'link/rc/mt/lib shims'
Check (-not (Test-Path "$Cache\xwin_cache") -and -not (Test-Path "$Cache\downloads") -and -not @(Get-ChildItem $Cache -Filter '_unpack_*').Count) 'download / unpack / xwin cache folders cleaned'
$xw = @($script:Native | Where-Object { $_[0] -like '*xwin.exe' })
Check ($xw.Count -eq 1 -and ($xw[0] -contains '--accept-license') -and ($xw[0] -contains '--disable-symlinks')) 'xwin called with --accept-license splat --disable-symlinks'
Check (@($script:Native | Where-Object { $_[0] -like '*python\python.exe' -and $_ -contains 'ensurepip' }).Count) 'portable Python gets pip (ensurepip)'
$m = @(Get-BtManifest)
Check ((@($m | Where-Object { $_ -like 'portable|*' }).Count -eq 6)) 'manifest lists the six portable tools'
$script:Downloads = @()
Install-PortableTools $all -AcceptMicrosoftLicense
Check (-not $script:Downloads.Count -and @(Get-BtManifest).Count -eq 6) 'second run: nothing downloaded, manifest unchanged'
$e = Get-BtPortableEnv
Check ($e.Msvc -and $e.Path[0] -eq "$Cache\shims") 'installed cache is picked up'

# a damaged download
Remove-Item "$Cache\ninja" -Recurse -Force
$script:BtPortable.ninja.Hash = '0' * 64
$threw = $false; try { Install-PortableTools @('ninja') } catch { $threw = $_ -match 'hash mismatch' }
Check ($threw -and -not (Test-Path "$Cache\downloads\ninja-win.zip")) 'hash mismatch: stops and deletes the file'

Write-Host '-- a failed portable install leaves no partial folder (re-review #2)'
function NoLeftovers { return (-not @(Get-ChildItem $Cache -Filter '_stage_*').Count -and -not @(Get-ChildItem $Cache -Filter '_unpack_*').Count) }
$savedGit = @{} + $script:BtPortable.git
Remove-Item "$Cache\git" -Recurse -Force
Set-BtManifest @(Get-BtManifest | Where-Object { $_ -ne 'portable|git' })
$bad = New-Zip 'mingit-bad.zip' @{ 'cmd\readme.txt' = 'r' }   # unpacks fine, but has no git.exe
$script:BtPortable.git.Url = 'https://example.invalid/mingit-bad.zip'; $script:BtPortable.git.Hash = HashOf $bad
$threw = $false; try { Install-PortableTools @('git') } catch { $threw = $_ -match 'incomplete' }
Check ($threw -and -not (Test-Path "$Cache\git") -and (NoLeftovers)) 'incomplete unzip (git): no tools_cache\git left behind'
Check (@(Get-BtManifest) -notcontains 'portable|git') 'incomplete tool: not in the manifest'
$script:BtPortable.git = $savedGit
$origNative = ${function:Invoke-BtNative}
function Invoke-BtNative([string]$File, [string[]]$Arguments) {
    if ($File -like '*xwin.exe') { Touch (Join-Path $Arguments[[array]::IndexOf($Arguments, '--output') + 1] 'crt\include\vcruntime.h'); return 1 }
    return 0
}
Remove-Item "$Cache\msvc" -Recurse -Force
$threw = $false; try { Install-PortableTools @('vs') -AcceptMicrosoftLicense } catch { $threw = $_ -match 'xwin failed' }
Check ($threw -and -not (Test-Path "$Cache\msvc") -and (NoLeftovers)) 'xwin failing halfway: no partial tools_cache\msvc'
Set-Item -Path function:Invoke-BtNative -Value $origNative

Write-Host '-- removal'
Set-BtManifest @('winget|LLVM.LLVM', 'winget|Microsoft.VisualStudio.2022.BuildTools', 'direct|Ninja-build.Ninja', 'pip|C:\Py\python.exe|pycryptodome', 'gitconfig|safe.directory|F:/Porty/x', 'portable|clang', 'portable|vs', 'portable|python')
$script:Uninst = @()
function Get-BtWinget { return [pscustomobject]@{ Source = 'C:\fake\winget.exe' } }
function Invoke-BtUninstallCmd([string]$File, [string]$ArgLine) { $script:Uninst += "$File $ArgLine"; if ($ArgLine -match 'Microsoft\.VisualStudio') { return 1 }; return 0 }
$script:Native = @()
$plan = @(Remove-SetupTools -DryRun)
Check ($plan.Count -eq 8 -and -not $script:Uninst.Count -and -not $script:Native.Count -and (Test-Path "$Cache\llvm")) 'dry run: lists everything, changes nothing'
Check (Test-BtNeedsAdmin $plan) 'winget / direct entries need administrator rights'
Check (-not (Test-BtNeedsAdmin @('portable|clang', 'pip|x|y'))) 'portable / pip entries do not'
Set-BtManifest @('winget|LLVM.LLVM', 'winget|Microsoft.VisualStudio.2022.BuildTools', 'pip|C:\Windows\System32\cmd.exe|pycryptodome', 'gitconfig|safe.directory|F:/Porty/x', 'portable|clang', 'portable|vs', 'portable|python')
$left = @(Remove-SetupTools)
Check (@($script:Uninst -match 'uninstall --id LLVM\.LLVM -e').Count) 'winget uninstall LLVM'
Check (@($script:Native | Where-Object { $_ -contains '--unset-all' -and $_ -contains 'F:/Porty/x' -and $_ -contains '--fixed-value' }).Count) 'safe.directory entry removed with git config --fixed-value --unset-all'
Check (@($script:Native | Where-Object { $_ -contains 'uninstall' -and $_ -contains 'pycryptodome' }).Count) 'pip uninstall pycryptodome'
Check (-not (Test-Path "$Cache\llvm") -and -not (Test-Path "$Cache\msvc") -and -not (Test-Path "$Cache\shims") -and -not (Test-Path "$Cache\python")) 'portable folders deleted'
Check ($left.Count -eq 1 -and $left[0] -eq 'winget|Microsoft.VisualStudio.2022.BuildTools' -and @(Get-BtManifest) -contains $left[0]) 'a failed uninstall stays in the manifest'
$script:Uninst = @()
function Invoke-BtUninstallCmd([string]$File, [string]$ArgLine) { $script:Uninst += "$File $ArgLine"; return -1978335212 }
$left = @(Remove-SetupTools)
Check (-not $left.Count -and -not (Test-Path (Get-BtManifestPath))) '"not installed" counts as removed; manifest gone'
$left = @(Remove-SetupTools)
Check (-not $left.Count) 'nothing recorded: nothing to do'

Write-Host '-- safe.directory without git on PATH (review #2)'
$savedPath = $env:Path
$env:Path = "$env:SystemRoot\System32"
$script:Native = @()
Remove-Item "$Cache\git" -Recurse -Force -ErrorAction SilentlyContinue
Set-BtManifest @('gitconfig|safe.directory|F:/Porty/y')
$left = @(Remove-SetupTools)
Check ($left.Count -eq 1 -and @(Get-BtManifest) -contains 'gitconfig|safe.directory|F:/Porty/y' -and -not $script:Native.Count) 'no git anywhere: the entry stays listed (not dropped while the setting stays)'
Touch "$Cache\git\cmd\git.exe"
$left = @(Remove-SetupTools)
Check (-not $left.Count -and @($script:Native | Where-Object { $_[0] -eq "$Cache\git\cmd\git.exe" -and $_ -contains 'F:/Porty/y' }).Count) 'the portable MinGit in tools_cache undoes it'
function Invoke-BtNative([string]$File, [string[]]$Arguments) { $script:Native += ,(@($File) + $Arguments); return 1 }
Set-BtManifest @('gitconfig|safe.directory|F:/Porty/z')
$left = @(Remove-SetupTools)
Check ($left.Count -eq 1) 'git config failing: the entry stays listed'
Set-BtManifest @()
$env:Path = $savedPath

Write-Host '-- tools already on the PC are never recorded (review #1)'
$script:WgList = @{}; $script:Arp = @{}
function Invoke-BtWingetList([string]$Winget, [string]$Id) { if ($script:WgList.ContainsKey($Id)) { return 0 }; return -1978335212 }
function Get-BtArpEntry([string]$Pattern) { if ($script:Arp.ContainsKey($Pattern)) { return [pscustomobject]@{ DisplayName = 'x'; UninstallString = 'x' } }; return $null }
Check (-not (Test-BtPresentBefore 'Kitware.CMake')) 'absent everywhere: a fresh install (recorded)'
Check (Test-BtPresentBefore 'Kitware.CMake' 'old') 'an older CMake (<4.2) that winget upgrades: not recorded'
Check (Test-BtPresentBefore 'LLVM.LLVM' 'old') 'an older LLVM (<18): not recorded'
Check (Test-BtPresentBefore 'Python.Python.3.12' 'old') 'an older Python: not recorded'
$script:WgList['LLVM.LLVM'] = 1
Check (Test-BtPresentBefore 'LLVM.LLVM') 'winget lists the package: not recorded'
$script:WgList = @{}
$script:Arp['^CMake$'] = 1
Check (Test-BtPresentBefore 'Kitware.CMake') 'Apps & features has it (installed without winget): not recorded'
$script:Arp['^Visual Studio Build Tools 2022'] = 1
Check (Test-BtPresentBefore 'Microsoft.VisualStudio.2022.BuildTools') 'VS Build Tools without the C++ workload (winget would modify it): not recorded'
$script:Arp = @{}
function Get-BtWinget { return $null }
Check (Test-BtPresentBefore 'Git.Git') 'no winget: unknown, so treated as present (not recorded)'
function Get-BtWinget { return [pscustomobject]@{ Source = 'C:\fake\winget.exe' } }
function Invoke-BtWingetList([string]$Winget, [string]$Id) { return -1978335217 }
Check (Test-BtPresentBefore 'Git.Git') 'winget list fails with another code (-1978335217): treated as present (re-review #1)'
function Invoke-BtWingetList([string]$Winget, [string]$Id) { throw 'winget crashed' }
Check (Test-BtPresentBefore 'Git.Git') 'winget list throws: treated as present'
function Invoke-BtWingetList([string]$Winget, [string]$Id) { return -1978335212 }
Check (-not (Test-BtPresentBefore 'Git.Git')) 'only "no installed package found" (0x8A150014) means absent'
$script:Arp['^Git$'] = 1
Check (Test-BtPresentBefore 'Git.Git') 'Apps & features entry: present even when winget says absent'
$script:Arp = @{}

Write-Host '-- unattended runs never install system-wide on their own (review #3)'
Remove-Item $Cache -Recurse -Force -ErrorAction SilentlyContinue
Check ((Get-BtToolMode) -eq '') 'interactive: ask'
Check ((Get-BtToolMode -Unattended) -eq 'STOP') 'unattended (update.ps1): stop and list the tools'
Check ((Get-BtToolMode -Unattended -InstallTools) -eq 'S') 'unattended -InstallTools (Setup.exe after its question): system-wide'
Check ((Get-BtToolMode -Unattended -PortableTools) -eq 'P') 'unattended -PortableTools: portable'
Add-BtManifest 'portable|cmake'
Check ((Get-BtToolMode -Unattended) -eq 'P') 'portable chosen before (manifest): portable again'
Set-BtManifest @()
Touch "$Cache\ninja\ninja.exe"
Check ((Get-BtToolMode -Unattended) -eq 'P') 'portable chosen before (complete tool in tools_cache): portable again'
Remove-Item $Cache -Recurse -Force
Touch "$Cache\git\README.txt"; Touch "$Cache\msvc\crt\include\vcruntime.h"; Touch "$Cache\ninja\ninja.zip.part"
Check ((Get-BtToolMode -Unattended) -eq 'STOP') 'leftovers of a failed portable attempt do not switch to portable mode (re-review #2)'
Remove-Item $Cache -Recurse -Force
Check ((Get-BtToolMode -Unattended) -eq 'STOP') 'tools_cache lost: stop, never system-wide'

Write-Host '-- an MSYS2/Cygwin cmake is not accepted as CMake (re-review #3)'
$fk = Join-Path $Root 'fakecmake'
Touch "$fk\msys\cmake.bat" '@exit /b 1'; Touch "$fk\native\cmake.bat" '@exit /b 0'
Check (Test-BtCmakeNative "$fk\native\cmake.bat") 'no probe file in the port: accepted (nothing to check)'
Touch "$script:BtPort\tools\cmake_native_probe.cmake" 'x'
Check (-not (Test-BtCmakeNative "$fk\msys\cmake.bat")) 'probe fails (MSYS2/Cygwin cmake): not native'
Check (Test-BtCmakeNative "$fk\native\cmake.bat") 'probe passes: native'
Check (-not (Test-BtCmakeNative "$fk\missing\cmake.exe")) 'cmake that cannot run: not native'

Write-Host '-- Microsoft''s licence is remembered in the manifest (re-review 3 #2)'
Remove-Item $Cache -Recurse -Force -ErrorAction SilentlyContinue
Check (-not (Test-BtMsLicenseAccepted)) 'fresh folder: not accepted'
Save-BtMsLicenseAccepted; Save-BtMsLicenseAccepted
Check ((Test-BtMsLicenseAccepted) -and -not @(Get-BtManifest).Count) 'accepted: remembered, but nothing to remove'
Check (-not (Test-BtPortableChosen)) 'the licence line alone does not mean portable mode'
Add-BtManifest 'portable|ninja'; Set-BtManifest @('portable|ninja', 'portable|cmake')
Check ((Test-BtMsLicenseAccepted) -and @(Get-Content (Get-BtManifestPath) | Where-Object { $_ -eq $script:BtMsAccepted }).Count -eq 1) 'kept (once) when the manifest is rewritten'
Set-BtManifest @()
Check (-not (Test-BtMsLicenseAccepted)) 'forgotten when everything is removed'
Add-BtManifest 'portable|vs'
Check (Test-BtMsLicenseAccepted) 'portable MSVC libraries in the manifest: accepted before'
Set-BtManifest @()
Install-PortableTools @('vs') -AcceptMicrosoftLicense -DryRun *> $null
Check (-not (Test-BtMsLicenseAccepted)) 'dry run records nothing'
$threw = $false; try { Install-PortableTools @('vs') } catch { $threw = $true }
Check ($threw -and -not (Test-BtMsLicenseAccepted)) 'no licence: refused, nothing recorded'

Write-Host '-- setup.ps1: an MSYS2 cmake is missing, and portable mode fetches the portable CMake (re-review 3 #1)'
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Port 'setup.ps1'), [ref]$null, [ref]$null)
foreach ($n in 'Have', 'Test-NativeCmake', 'Test-CmakeOk', 'Test-ClangOk', 'Get-MissingToolKeys', 'Install-CMakeTool') {
    $fd = @($ast.FindAll({ param($x) $x -is [Management.Automation.Language.FunctionDefinitionAst] -and $x.Name -eq $n }, $true))
    Check ($fd.Count -eq 1) "setup.ps1 defines $n"
    . ([scriptblock]::Create($fd[0].Extent.Text))
}
function Find-Python { return 'py' }; function Find-VS { return 'C:\VS' }; $NeedGit = $false
function Fail([string]$m) { throw "FAIL: $m" }; function Refresh-Path { }
$script:Calls = @()
function Install-PortableTools([string[]]$Keys) { $script:Calls += "portable:$($Keys -join ',')" }
function Winget-Install([string]$id, [string]$label) { $script:Calls += "winget:$id" }
$savedPath = $env:Path; $savedPF = $env:ProgramFiles; $savedLog = $script:BtLog
$pf = Join-Path $Root 'pf'; New-Item -ItemType Directory -Force -Path $pf | Out-Null
try {
    $env:ProgramFiles = $pf
    foreach ($t in 'ninja', 'clang-cl') { Touch "$fk\other\$t.bat" '@exit /b 0' }
    $env:Path = "$fk\msys;$fk\other;$env:SystemRoot\System32"
    Check (@(Get-MissingToolKeys) -contains 'cmake') 'MSYS2 cmake on PATH, no C:\Program Files\CMake: cmake is missing'
    Touch "$pf\CMake\bin\cmake.exe"
    Check (@(Get-MissingToolKeys) -notcontains 'cmake') 'MSYS2 cmake + C:\Program Files\CMake (build.bat uses it first): ok'
    Remove-Item "$pf\CMake" -Recurse -Force
    $env:Path = "$fk\native;$fk\other;$env:SystemRoot\System32"
    Check (@(Get-MissingToolKeys) -notcontains 'cmake') 'native cmake on PATH: ok'
    $script:ToolMode = 'P'; Install-CMakeTool
    Check (($script:Calls -join ' ') -eq 'portable:cmake') 'portable mode: the step-5 fallback gets the portable CMake, no winget'
    $script:Calls = @(); $script:ToolMode = ''; Install-CMakeTool
    Check (($script:Calls -join ' ') -eq 'winget:Kitware.CMake') 'system-wide mode: winget (it asks / checks permission itself)'
    $script:Calls = @(); Add-BtManifest 'portable|ninja'; Install-CMakeTool
    Check (($script:Calls -join ' ') -eq 'portable:cmake') 'portable chosen before (manifest), no mode decided this run: portable CMake'
    $script:Calls = @(); $script:ToolMode = 'S'; Install-CMakeTool
    Check (($script:Calls -join ' ') -eq 'winget:Kitware.CMake') 'portable chosen before, but S answered this run: system-wide (round 4)'
    $script:ToolMode = ''
    Set-BtManifest @()
} finally { $env:Path = $savedPath; $env:ProgramFiles = $savedPF; $script:BtLog = $savedLog }

Write-Host '-- Python 3.10+ finder (player report "python 3.10+ missing")'
$pyd = Join-Path $Root 'py'
Touch "$pyd\Microsoft\WindowsApps\python.bat" "@echo Python was not found; run without arguments to install from the Microsoft Store 1>&2`r`n@exit /b 9009`r`n"
Touch "$pyd\Py39\python.bat" "@echo 3.9.13`r`n@echo %~f0`r`n"
Touch "$pyd\Py312\python.bat" "@echo 3.12.10`r`n@echo %~f0`r`n"
$stubC = @{ Exe = "$pyd\Microsoft\WindowsApps\python.bat"; Pre = @(); From = 'PATH' }
$oldC = @{ Exe = "$pyd\Py39\python.bat"; Pre = @(); From = 'PATH' }
$newC = @{ Exe = "$pyd\Py312\python.bat"; Pre = @(); From = 'registry' }
Check ($null -eq (Invoke-BtPyProbe $stubC.Exe)) 'probe: the Store alias stub (exit 9009) is not Python'
$pr = Invoke-BtPyProbe $newC.Exe
Check ($pr -and $pr.Version -eq [version]'3.12.10' -and $pr.Exe -eq $newC.Exe) 'probe: version and sys.executable'
$f = Find-BtPython -Candidates @($stubC, $oldC, $newC)
Check ($f.Exe -eq $newC.Exe -and $f.Version -eq '3.12.10' -and $f.From -eq 'registry') 'stub and 3.9 on PATH skipped, the registry 3.12 found'
$f = Find-BtPython -Candidates @($stubC, $oldC)
Check (-not $f.Exe -and $f.Old -eq '3.9.13' -and $f.Stub) 'none usable: the old version and the stub are reported'
Check ((Get-BtPythonHelp $f) -match 'App execution aliases' -and (Get-BtPythonHelp $f) -match '3\.9\.13') 'help names the alias setting and the old version'
$f = Find-BtPython -Candidates @($oldC, $oldC) -Probe { param($e, $p) $script:Probes++; return @{ Version = [version]'3.9.1'; Exe = $e } }
Check (-not $f.Exe -and -not $f.Stub) 'no stub flag without a WindowsApps candidate'
$script:Probes = 0; [void](Find-BtPython -Candidates @($oldC, $oldC, $newC) -Probe { param($e, $p) $script:Probes++; return $null })
Check ($script:Probes -eq 2) 'each candidate probed once'
$rk = "HKCU:\SOFTWARE\PortBtTest_$PID"
try {
    New-Item -Path "$rk\PythonCore\3.9\InstallPath" -Force | Out-Null
    Set-Item -Path "$rk\PythonCore\3.9\InstallPath" -Value "$pyd\Py39"
    Touch "$pyd\Py39\python.exe"; Touch "$pyd\Py312\python.exe"; Touch "$pyd\Py311\python.exe"
    New-Item -Path "$rk\PythonCore\3.12\InstallPath" -Force | Out-Null
    New-ItemProperty -Path "$rk\PythonCore\3.12\InstallPath" -Name ExecutablePath -Value "$pyd\Py312\python.exe" | Out-Null
    New-Item -Path "$rk\PythonCore\3.11-64\InstallPath" -Force | Out-Null
    Set-Item -Path "$rk\PythonCore\3.11-64\InstallPath" -Value "$pyd\Py311"
    New-Item -Path "$rk\PythonCore\3.13\InstallPath" -Force | Out-Null
    New-ItemProperty -Path "$rk\PythonCore\3.13\InstallPath" -Name ExecutablePath -Value "$pyd\gone\python.exe" | Out-Null
    $re = @(Get-BtPyRegistryExes @($rk))
    Check (($re -join ';') -eq "$pyd\Py312\python.exe;$pyd\Py311\python.exe;$pyd\Py39\python.exe") 'PEP 514 registry: newest first, ExecutablePath or the folder, missing files skipped'
} finally { Remove-Item -LiteralPath $rk -Recurse -Force -ErrorAction SilentlyContinue }
# setup.ps1: winget installs Python but it still cannot be found -> the portable Python; portable mode -> no winget
foreach ($n in 'Find-Python', 'Install-PythonTool') {
    $fd = @($ast.FindAll({ param($x) $x -is [Management.Automation.Language.FunctionDefinitionAst] -and $x.Name -eq $n }, $true))
    Check ($fd.Count -eq 1) "setup.ps1 defines $n"
    . ([scriptblock]::Create($fd[0].Extent.Text))
}
function Info([string]$m) { }
$savedLog = $script:BtLog; $savedPath = $env:Path
try {
    function Find-BtPython { if ($script:Calls -contains 'portable:python') { return @{ Exe = "$pyd\Py312\python.exe"; Version = '3.12.10'; Stub = $false; Old = '' } }; return @{ Exe = ''; Stub = $true; Old = '' } }
    $script:Calls = @(); $script:ToolMode = 'S'
    $got = Install-PythonTool
    Check (($script:Calls -join ' ') -eq 'winget:Python.Python.3.12 portable:python' -and $got -eq "$pyd\Py312\python.exe") 'installed but not found: falls back to the portable Python'
    Check (($env:Path -split ';') -contains "$pyd\Py312") 'a Python found outside PATH is put on PATH for the build'
    $script:Calls = @(); $script:ToolMode = 'P'
    [void](Install-PythonTool)
    Check (($script:Calls -join ' ') -eq 'portable:python') 'portable mode: the portable Python, no winget'
    $script:Calls = @(); $script:ToolMode = 'S'
    function Find-BtPython { return @{ Exe = ''; Stub = $false; Old = '3.9.1' } }
    Check (-not (Install-PythonTool) -and ($script:Calls -join ' ') -eq 'winget:Python.Python.3.12 portable:python') 'nothing works: $null (setup.ps1 then stops with Get-BtPythonHelp)'
} finally { $script:BtLog = $savedLog; $env:Path = $savedPath; $script:ToolMode = '' }

if ($script:Failed) { Write-Host "test_buildtools: $script:Failed FAILED (work files kept in $Root)"; exit 1 }
Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue; $bts = Split-Path $Root; if ((Test-Path $bts) -and -not (Get-ChildItem -LiteralPath $bts -Force)) { Remove-Item -LiteralPath $bts -Force }
Write-Host 'test_buildtools: all passed'; exit 0
