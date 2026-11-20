# Tests for tools\fetch_rpcs3.ps1 without the internet: a fake "RPCS3" with two submodules and an extra library is
# made with the real git in build_test_setup\ (git-ignored), and failures are injected around Invoke-GitRaw.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\tests\test_fetch_rpcs3.ps1
$ErrorActionPreference = 'Stop'
$Port = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$Root = Join-Path $Port 'build_test_setup\fetch'
if (-not (Test-Path (Join-Path $Port 'tools\fetch_rpcs3.ps1'))) { Write-Host 'SKIP: this port does not fetch RPCS3'; exit 0 }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Write-Host 'SKIP: git not found'; exit 0 }
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Root | Out-Null

$script:Failed = 0
function Check($Ok, [string]$What) {
    $Ok = [bool]@($Ok | Where-Object { $_ }).Count   # arrays from -match count as true when anything matched
    if ($Ok) { Write-Host "  ok   $What" } else { Write-Host "  FAIL $What"; $script:Failed++ }
}

# ---- fake upstream repositories ---------------------------------------------------------------------------
$env:GIT_AUTHOR_NAME = $env:GIT_COMMITTER_NAME = 't'; $env:GIT_AUTHOR_EMAIL = $env:GIT_COMMITTER_EMAIL = 't@t'
function G {
    $ErrorActionPreference = 'Continue'
    $o = @(& git -c protocol.file.allow=always -c core.autocrlf=false -c init.defaultBranch=master @args 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE) { throw "git $args : $o" }
    $o
}
function New-Repo([string]$Name, [hashtable]$Files) {
    $d = Join-Path $Root "up\$Name"; G init -q $d | Out-Null
    foreach ($k in $Files.Keys) { $p = Join-Path $d $k; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $Files[$k] + "`n") }
    G -C $d add -A | Out-Null; G -C $d commit -q -m init | Out-Null
    $d
}
$s1 = New-Repo 's1' @{ 'one.h' = 'int one;' }
$s2 = New-Repo 's2' @{ 'two.h' = 'int two;' }
$ex = New-Repo 'ex' @{ 'ex.h' = 'int ex;' }; G -C $ex tag v1 | Out-Null
$up = New-Repo 'rpcs3' @{ 'rpcs3\Emu\core.cpp' = 'int core = 1;' }
G -C $up submodule add -q "file:///$($s1 -replace '\\','/')" 3rdparty/s1/s1 | Out-Null
G -C $up submodule add -q "file:///$($s2 -replace '\\','/')" 3rdparty/s2 | Out-Null
G -C $up commit -q -m subs | Out-Null
$upCommit = ((G -C $up rev-parse HEAD) -join '').Trim()
# the "port patch": changes a file and adds the marker file
$pc = Join-Path $Root 'patchwork'; G clone -q $up $pc | Out-Null
[IO.File]::WriteAllText((Join-Path $pc 'rpcs3\Emu\core.cpp'), "int core = 2;`n")
New-Item -ItemType Directory -Force (Join-Path $pc 'rpcs3\Emu\RSX\VK') | Out-Null
[IO.File]::WriteAllText((Join-Path $pc 'rpcs3\Emu\RSX\VK\Marker.cpp'), "int marker;`n")
G -C $pc add -A | Out-Null
$patch = Join-Path $Root 'test-rpcs3.patch'
[IO.File]::WriteAllText($patch, ((G -C $pc diff --cached --no-color) -join "`n") + "`n")
# what GitHub's source archives would contain, as zips with one top folder
Add-Type -AssemblyName System.IO.Compression.FileSystem
function New-Archive([string]$Dir, [string]$Top) {
    $stage = Join-Path $Root "zipstage\$Top"; G clone -q $Dir $stage | Out-Null; Remove-Item (Join-Path $stage '.git') -Recurse -Force
    $zip = Join-Path $Root "$Top.zip"; [IO.Compression.ZipFile]::CreateFromDirectory((Split-Path $stage), $zip, 'Optimal', $false)
    Remove-Item (Split-Path $stage) -Recurse -Force; $zip
}
$Zips = @{ 'rpcs3' = (New-Archive $up 'rpcs3-x'); 's1' = (New-Archive $s1 's1-x'); 's2' = (New-Archive $s2 's2-x'); 'ex' = (New-Archive $ex 'ex-x') }

# ---- load the script under test ---------------------------------------------------------------------------
. (Join-Path $Port 'tools\fetch_rpcs3.ps1') -Library -Dest (Join-Path $Root 'dest') -LogFile (Join-Path $Root 'fetch.log') `
    -RetryDelay 0 -Tries 2 -Patch $patch -Manifest (Join-Path $Root 'manifest.txt') -StageRoot (Join-Path $Root 'stage')
$ErrorActionPreference = 'Stop'
$script:Commit = $upCommit
$script:RepoUrl = "file:///$($up -replace '\\','/')"
$script:RepoArchive = 'zip://rpcs3'
$script:MarkerRel = 'rpcs3\Emu\RSX\VK\Marker.cpp'
$script:Subs = @(
    @{ Path = '3rdparty/s1/s1'; Name = '3rdparty/s1/s1'; Repo = 'zip/s1'; Sha = ((G -C $s1 rev-parse HEAD) -join '').Trim() },
    @{ Path = '3rdparty/s2';    Name = '3rdparty/s2';    Repo = 'zip/s2'; Sha = ((G -C $s2 rev-parse HEAD) -join '').Trim() })
$script:Extras = @(@{ Path = '3rdparty/ex'; Key = 'ex'; Repo = 'zip/ex'; Tag = 'v1'; Url = "file:///$($ex -replace '\\','/')" })
$script:GitCfg += @('-c', 'protocol.file.allow=always', '-c', 'uploadpack.allowAnySHA1InWant=true')
${function:Real-GitRaw} = ${function:Invoke-GitRaw}
$script:Rules = @()      # @{ Match = regex on the joined args; Text = what "git" says; Times = how often (0 = always) }
$script:GitCalls = New-Object System.Collections.Generic.List[string]
function Invoke-GitRaw([string[]]$GitArgs) {
    $line = $GitArgs -join ' '; $script:GitCalls.Add($line)
    if ($line -match '^config --global') { return [pscustomobject]@{ Code = 0; Lines = @() } }   # never the real global config
    foreach ($r in $script:Rules) {
        if ($line -match $r.Match -and ($r.Times -eq 0 -or $r.Used -lt $r.Times)) {
            $r.Used++
            return [pscustomobject]@{ Code = 128; Lines = @($r.Text -split "`n") }
        }
    }
    Real-GitRaw $GitArgs
}
$script:Downloads = New-Object System.Collections.Generic.List[string]
$script:FailDownloads = $false
function Get-UrlFile([string]$Url, [string]$OutFile) {
    $script:Downloads.Add($Url)
    if ($script:FailDownloads) { throw 'The remote name could not be resolved' }
    $key = 'rpcs3'; if ($Url -match 'zip/(s1|s2|ex)') { $key = $Matches[1] }
    Copy-Item -LiteralPath $Zips[$key] -Destination $OutFile -Force
}
function Reset-Run { $script:Rules = @(); $script:GitCalls.Clear(); $script:Downloads.Clear(); $script:FailDownloads = $false; $script:GitText.Clear(); $script:SafeDirsAdded = @{} }
function Clear-All {
    foreach ($d in 'dest', 'stage') { $p = Join-Path $Root $d; if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
    $dl = Join-Path $Port 'tools_cache\rpcs3_download'; if (Test-Path $dl) { Remove-Item $dl -Recurse -Force }
    $tc = Join-Path $Port 'tools_cache'; if ((Test-Path $tc) -and -not (Get-ChildItem -LiteralPath $tc -Force)) { Remove-Item -LiteralPath $tc -Force }
}
$script:FakeDrive = $null    # $null: every drive is local NTFS
function Get-DriveFacts { param($Path)
    if ($script:FakeDrive -and $Path -notlike "$Root\stage*") { return $script:FakeDrive }
    @{ Root = 'C:\'; Type = 'Fixed'; Format = 'NTFS' }
}
$Dest = Join-Path $Root 'dest'
function Test-Complete([string]$D) {
    (Test-Path (Join-Path $D 'rpcs3\Emu\RSX\VK\Marker.cpp')) -and ((Get-Content (Join-Path $D 'rpcs3\Emu\core.cpp')) -eq 'int core = 2;') -and
    (Test-Path (Join-Path $D '3rdparty\s1\s1\one.h')) -and (Test-Path (Join-Path $D '3rdparty\s2\two.h')) -and (Test-Path (Join-Path $D '3rdparty\ex\ex.h'))
}
function Run-Fetch {
    try { Invoke-FetchRpcs3 *>&1 | Out-Null; return $true }
    catch { $script:LastError = "$($_.Exception.Message)"; Write-Host "     (fetch stopped: $script:LastError)"; return $false }
}
function Count-Calls([string]$Pattern) { @($script:GitCalls | Where-Object { $_ -match $Pattern }).Count }

Write-Host 'fetch_rpcs3: normal run'
Reset-Run; Clear-All
Check (Run-Fetch) 'finishes'
Check (Test-Complete $Dest) 'patched source with submodules and extras'
Check ((Count-Calls 'submodule update .*--jobs 5') -eq 1) 'submodules fetched together first'
Check (-not $script:Downloads.Count) 'no archive downloads needed'
Reset-Run
Check (Run-Fetch) 'second run finishes'
Check (-not $script:GitCalls.Count) 'second run: nothing downloaded'

Write-Host 'fetch_rpcs3: R2 #13 - shallow submodule fetches fail (server refuses unadvertised objects)'
Reset-Run; Clear-All
$script:Rules = @(@{ Match = 'submodule update .*--depth 1'; Text = 'error: Server does not allow request for unadvertised object 0123'; Times = 0; Used = 0 })
Check (Run-Fetch) 'finishes'
Check (Test-Complete $Dest) 'complete'
Check ((Count-Calls 'protocol\.version=2 .*submodule update -q --init -- 3rdparty/s2$') -eq 1) 'full (non-shallow) per-submodule fetch used'
Check ((Get-Content (Join-Path $Root 'fetch.log') -Raw) -match 'unadvertised object') "git's own message is in the log"

Write-Host 'fetch_rpcs3: flaky network - the first try fails, the retry works'
Reset-Run; Clear-All
$script:Rules = @(@{ Match = 'fetch -q --depth 1'; Text = 'fatal: unable to access: Recv failure: Connection was reset'; Times = 1; Used = 0 })
Check (Run-Fetch) 'finishes'
Check ((Count-Calls 'fetch -q --depth 1') -eq 2) 'shallow fetch tried twice'

Write-Host 'fetch_rpcs3: git cannot download anything - GitHub source archives'
Reset-Run; Clear-All
$script:Rules = @(@{ Match = ' fetch -q|submodule update|^clone'; Text = 'fatal: unable to access: SSL certificate problem'; Times = 0; Used = 0 })
Check (Run-Fetch) 'finishes from archives'
Check (Test-Complete $Dest) 'complete'
Check ($script:Downloads.Count -eq 4) "4 archives downloaded ($($script:Downloads.Count))"
Check ((Get-FetchMode $Dest) -eq 'archive') 'archive mode recorded'

Write-Host 'fetch_rpcs3: resume after a stopped run'
Reset-Run; Clear-All
$script:Rules = @(@{ Match = 'submodule update'; Text = 'error: RPC failed; curl 92 HTTP/2 stream 5 was not closed cleanly'; Times = 0; Used = 0 }); $script:FailDownloads = $true
Check (-not (Run-Fetch)) 'first run fails'
Check (@(Get-FetchFailureHint (($script:GitText -join "`n") + "`n" + $script:LastError)) -match 'dropped') 'hint: connection dropped'
New-Item -ItemType File -Force (Join-Path $Dest '3rdparty\s1\s1\half-written.tmp') | Out-Null
Reset-Run
Check (Run-Fetch) 'second run finishes'
Check ((Count-Calls 'fetch -q --depth 1 --no-tags origin') -eq 0) 'RPCS3 itself not downloaded again'
Check (-not (Test-Path (Join-Path $Dest '3rdparty\s1\s1\half-written.tmp'))) 'leftover of the stopped run cleaned'
Check (Test-Complete $Dest) 'complete'

Write-Host 'fetch_rpcs3: damaged tree - the patch does not apply'
Reset-Run
Remove-Item (Join-Path $Dest '.port_fetch\patch.ok'), (Join-Path $Dest 'rpcs3\Emu\RSX\VK\Marker.cpp') -Force -ErrorAction SilentlyContinue
Set-Content (Join-Path $Dest 'rpcs3\Emu\core.cpp') 'garbage' -Encoding ASCII
Check (Run-Fetch) 'finishes'
Check (Test-Complete $Dest) 'reset and patched'
Reset-Run
Remove-Item (Join-Path $Dest '.port_fetch\patch.ok') -Force
Check (Run-Fetch) 'patch already applied: recognised'
Check (Test-Complete $Dest) 'still complete'

Write-Host 'fetch_rpcs3: dubious ownership'
Reset-Run; Clear-All
$script:Rules = @(@{ Match = 'remote add'; Times = 1; Used = 0
    Text = "fatal: detected dubious ownership in repository at 'X:/dest'`nTo add an exception for this directory, call:`n`n`tgit config --global --add safe.directory X:/dest" })
Check (Run-Fetch) 'finishes'
Check ((Count-Calls '^config --global --add safe\.directory X:/dest$') -eq 1) 'safe.directory added for that folder only'
Check (@(Get-Content (Join-Path $Root 'manifest.txt')) -contains 'gitconfig|safe.directory|X:/dest') 'recorded for -RemoveTools'

Write-Host 'fetch_rpcs3: exFAT drive - done in a local folder, then copied'
Reset-Run; Clear-All
$script:FakeDrive = @{ Root = 'F:\'; Type = 'Removable'; Format = 'exFAT' }
Check (Run-Fetch) 'finishes'
Check (Test-Complete $Dest) 'complete in the port folder'
Check (-not (Test-Path (Join-Path $Dest '.git'))) 'copied without .git'
Check (-not @(Get-ChildItem (Join-Path $Root 'stage') -ErrorAction SilentlyContinue).Count) 'local work folder removed'
Check ((Get-Content (Join-Path $Root 'fetch.log') -Raw) -match 'exFAT drive') 'message names the drive type'
$script:FakeDrive = $null

Write-Host 'fetch_rpcs3: drive and hint checks'
Check ((Get-DriveProblem @{ Root = 'F:\'; Type = 'Removable'; Format = 'exFAT' }) -match 'exFAT') 'exFAT detected'
Check ((Get-DriveProblem @{ Root = 'F:\'; Type = 'Fixed'; Format = 'FAT32' }) -match 'FAT32') 'FAT32 detected'
Check ((Get-DriveProblem @{ Root = '\\nas\x\'; Type = 'Network'; Format = '' }) -match 'network') 'network detected'
Check ($null -eq (Get-DriveProblem @{ Root = 'C:\'; Type = 'Fixed'; Format = 'NTFS' })) 'NTFS is fine'
Check (@(Get-FetchFailureHint 'fatal: unable to access ... Could not resolve host: github.com') -match 'look up github') 'hint: DNS'
Check (@(Get-FetchFailureHint 'fatal: detected dubious ownership') -match 'NTFS') 'hint: ownership'
Check (@(Get-FetchFailureHint 'error: Server does not allow request for unadvertised object') -match 'refused') 'hint: unadvertised'
Check (@(Get-FetchFailureHint 'error: unable to create file x: Filename too long') -match 'short folder') 'hint: long path'
Check (@(Get-FetchFailureHint 'schannel: next InitializeSecurityContext failed') -match 'antivirus') 'hint: TLS'

Clear-All
if ($script:Failed) { Write-Host "test_fetch_rpcs3: $script:Failed FAILED (work files kept in $Root)"; exit 1 }
Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue; $bts = Split-Path $Root; if ((Test-Path $bts) -and -not (Get-ChildItem -LiteralPath $bts -Force)) { Remove-Item -LiteralPath $bts -Force }
Write-Host 'test_fetch_rpcs3: all passed'; exit 0
