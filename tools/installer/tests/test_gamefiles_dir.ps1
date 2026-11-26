# Tests for setup.ps1's Resolve-GameFilesDir: which game files folder Setup (and so the launcher's updates, which run
# Setup -Unattended) uses -- -GameFiles, else PS3_VFS_ROOT, else the launcher's settings.ini gamefiles=, else
# gamefiles\ in the port folder. Uses a fake port in build_test_setup\ (git-ignored).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\tests\test_gamefiles_dir.ps1
$ErrorActionPreference = 'Stop'
$Real = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$Root = Join-Path $Real 'build_test_setup\gamefilesdir'
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Root | Out-Null

# setup.ps1 runs its steps when dot-sourced, so take just the function from it
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Real 'setup.ps1'), [ref]$null, [ref]$null)
$fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Resolve-GameFilesDir' }, $true)
if (-not $fn) { throw 'Resolve-GameFilesDir not found in setup.ps1' }
. ([scriptblock]::Create($fn.Extent.Text))

$script:Failed = 0
function Check($Ok, [string]$What) {
    if ([bool]$Ok) { Write-Host "  ok   $What" } else { Write-Host "  FAIL $What"; $script:Failed++ }
}
$Port = Join-Path $Root 'port'
New-Item -ItemType Directory -Force -Path $Port | Out-Null
$ini = Join-Path $Port 'settings.ini'
$savedEnv = $env:PS3_VFS_ROOT
$env:PS3_VFS_ROOT = $null
$GameFiles = ''

try {
    $r = Resolve-GameFilesDir
    Check ($r.Dir -eq (Join-Path $Port 'gamefiles') -and -not $r.Custom) 'no settings.ini: gamefiles\ in the port folder'

    [IO.File]::WriteAllText($ini, "fullscreen=1`r`ngamefiles=`r`nvsync=1`r`n")
    $r = Resolve-GameFilesDir
    Check (-not $r.Custom) 'an empty gamefiles= is the default'

    [IO.File]::WriteAllText($ini, "fullscreen=1`r`ngamefiles=E:\Old\gamefiles`r`ngamefiles=E:/Games/R1 & co/gamefiles/`r`n")
    $r = Resolve-GameFilesDir
    Check ($r.Dir -eq 'E:\Games\R1 & co\gamefiles' -and $r.Custom -and $r.Source -like '*settings.ini*') 'the launcher setting (last line, slashes fixed)'

    [IO.File]::WriteAllText($ini, "gamefiles=$Port\gamefiles\`r`n")
    $r = Resolve-GameFilesDir
    Check (-not $r.Custom) 'the setting naming the default folder is not custom'

    [IO.File]::WriteAllText($ini, "gamefiles=E:\Games\gamefiles`r`n")
    $env:PS3_VFS_ROOT = 'F:\manual\gamefiles'
    $r = Resolve-GameFilesDir
    Check ($r.Dir -eq 'F:\manual\gamefiles' -and $r.Source -like '*PS3_VFS_ROOT*') 'PS3_VFS_ROOT wins over the setting'

    $GameFiles = 'G:\cmdline\gamefiles'
    $r = Resolve-GameFilesDir
    Check ($r.Dir -eq 'G:\cmdline\gamefiles' -and $r.Source -eq '-GameFiles') '-GameFiles wins over both'
    $GameFiles = ''

    $env:PS3_VFS_ROOT = 'E:\'
    $r = Resolve-GameFilesDir
    Check ($r.Dir -eq 'E:\') 'a drive root keeps its backslash'
}
finally {
    $env:PS3_VFS_ROOT = $savedEnv
    Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:Failed) { Write-Host "$($script:Failed) FAILED"; exit 1 }
Write-Host 'all passed'
