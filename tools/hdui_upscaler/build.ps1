# Rebuilds tools\hdui_upscaler\bin\ (the upscalers HDUIPack.exe runs) from src\ and the pinned ncnn release.
#   powershell -ExecutionPolicy Bypass -File tools\hdui_upscaler\build.ps1 [-Work <folder for the ncnn checkout and builds>]
# Needs git, CMake, Ninja, Visual Studio 2022 (C++ build tools) and, for the Vulkan build, the Vulkan SDK (glslangValidator).
# After a rebuild, put the printed SHA-256 values into project\hd_ui_pack.cpp (upscaler_cpu, upscaler_gpu, upscaler_omp) in all three ports.
param([string]$Work = (Join-Path $PSScriptRoot '_build'))
$ErrorActionPreference = 'Stop'
$NcnnUrl = 'https://github.com/Tencent/ncnn.git'
$NcnnTag = '20261020'
$NcnnCommit = '7e36d25cb153092ba87a60530455c7376fedf452'

$Src = Join-Path $PSScriptRoot 'src'
$Bin = Join-Path $PSScriptRoot 'bin'
$Ncnn = Join-Path $Work 'ncnn'
New-Item -ItemType Directory -Force -Path $Work, $Bin | Out-Null

if (-not (Test-Path (Join-Path $Ncnn 'CMakeLists.txt'))) {
    git clone --depth 1 --branch $NcnnTag $NcnnUrl $Ncnn
    if ($LASTEXITCODE) { throw 'git clone of ncnn failed' }
}
$head = (git -C $Ncnn rev-parse HEAD).Trim()
if ($head -ne $NcnnCommit) { throw "ncnn in $Ncnn is at $head, not $NcnnCommit ($NcnnTag)" }
git -C $Ncnn submodule update --init --depth 1 glslang
if ($LASTEXITCODE) { throw 'ncnn glslang submodule failed' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$ncnnCm = $Ncnn.Replace('\', '/')
$ver = "$NcnnTag-$($NcnnCommit.Substring(0, 7))"

foreach ($v in @(@{ On = 'ON'; Dir = 'build_vk'; Exe = 'realesrgan-ncnn-vulkan.exe' }, @{ On = 'OFF'; Dir = 'build_cpu'; Exe = 'realesrgan-ncnn-cpu.exe' })) {
    $b = Join-Path $Work $v.Dir
    $cmd = "call `"$vcvars`" >nul && cmake -G Ninja -S `"$Src`" -B `"$b`" -DCMAKE_BUILD_TYPE=Release -DHDUI_VULKAN=$($v.On) " +
           "-DNCNN_SRC=`"$ncnnCm`" -DHDUI_NCNN_VERSION=$ver && cmake --build `"$b`""
    & $env:ComSpec /c $cmd
    if ($LASTEXITCODE) { throw "build of $($v.Exe) failed" }
    Copy-Item -Force (Join-Path $b $v.Exe) $Bin
}

# the OpenMP runtime the upscalers link (Microsoft Visual C++ redistributable file)
$omp = Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Recurse -Filter vcomp140.dll |
    Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC\d+\.OpenMP\\' -and $_.FullName -notmatch '\\(debug_nonredist|onecore)\\' } |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $omp) { throw 'vcomp140.dll not found in the Visual C++ redistributable folder' }
Copy-Item -Force $omp.FullName $Bin

Get-ChildItem $Bin -File | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name }
