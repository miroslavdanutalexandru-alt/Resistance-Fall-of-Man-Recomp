# Runs "cmake --build <dir> -j <jobs>", echoes the output and writes it to <log> as UTF-8.
# Tee-Object in Windows PowerShell writes UTF-16, which Setup.exe could not follow for progress.
# The log is flushed per line so Setup.exe can show progress live.
param([Parameter(Mandatory)][string]$BuildDir, [Parameter(Mandatory)][string]$Log, [string]$Jobs = '')

$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $Log
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
# shared: Setup and anyone else may read the log while the build runs
$fs = New-Object System.IO.FileStream($Log, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
$w = New-Object System.IO.StreamWriter($fs, (New-Object System.Text.UTF8Encoding($false)))
$w.AutoFlush = $true
$failed = $false
$cmakeArgs = @('--build', $BuildDir)
if ($Jobs) { $cmakeArgs += @('-j', $Jobs) }
try {
    & cmake @cmakeArgs 2>&1 | ForEach-Object {
        $s = "$_"
        $w.WriteLine($s)
        if ($s -match '^FAILED: |^ninja: build stopped') { $failed = $true }
        $s
    }
    $code = $LASTEXITCODE
} finally {
    $w.Dispose()
}
if ($failed -and $code -eq 0) { $code = 1 }
exit $code
