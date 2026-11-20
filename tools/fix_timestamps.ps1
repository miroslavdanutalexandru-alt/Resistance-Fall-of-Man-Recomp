# Files dated in the future break the build: Ninja sees CMakeLists.txt newer than build.ninja and
# regenerates forever ("still dirty after 100 tries"). Zip and installer times are stored as local
# time without a time zone, so a release packed a few hours "ahead" of the player's time zone
# arrives with future dates (also a PC clock that was ahead and then corrected). This sets such
# files to the current time, the build folders' own files (build.ninja, CMakeCache.txt) included.
# build.bat runs it before every build, so Setup, Setup.exe --prepare (RecompLauncher) and a plain
# build.bat all get it.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$skip = @('gamefiles', 'game', 'gamedata', 'shadercache', '.git')
$now = Get-Date
$fixed = 0
function Walk([string]$dir, [bool]$top) {
    foreach ($f in [IO.Directory]::EnumerateFiles($dir)) {
        try {
            # any time after now: even a few seconds ahead is enough to make Ninja loop
            if ([IO.File]::GetLastWriteTime($f) -gt $now) {
                try { [IO.File]::SetLastWriteTime($f, $now) }
                catch {
                    # read-only files (some unzip tools keep the attribute) cannot get a new time
                    $a = [IO.File]::GetAttributes($f)
                    [IO.File]::SetAttributes($f, $a -band -bnot [IO.FileAttributes]::ReadOnly)
                    [IO.File]::SetLastWriteTime($f, $now)
                }
                $script:fixed++
            }
        } catch { }
    }
    foreach ($d in [IO.Directory]::EnumerateDirectories($dir)) {
        if ($top -and $skip -contains (Split-Path -Leaf $d)) { continue }
        if (([IO.File]::GetAttributes($d) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
        Walk $d $false
    }
}
Walk $Root $true
if ($fixed) { Write-Host "Fixed $fixed file(s) dated in the future (time zone of the download, or the PC clock); the build would loop otherwise." }