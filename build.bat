@echo off
setlocal
set "NOPAUSE="
for %%A in (%*) do if /i "%%~A"=="nopause" set "NOPAUSE=1"
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist recompiled_game\ppu_recomp.h (echo The game code has not been generated yet - run Setup.bat first. & goto fail)
rem A game window left open locks the .exe and the link fails -- close any that are still running.
rem Close only a copy of ResistanceFoMGame.exe started from THIS folder (never one from another checkout).
powershell -NoProfile -Command "$d=[IO.Path]::GetFullPath('%~dp0'); $k=0; Get-Process ResistanceFoMGame -EA 0 | ?{ $_.Path -and $_.Path.StartsWith($d,'OrdinalIgnoreCase') } | ForEach-Object { Stop-Process -Id $_.Id -Force; $k=1 }; if($k){ 'Closed a ResistanceFoMGame.exe that was still running.' }"
rem Close only a copy of ResistanceFoM.exe started from THIS folder (never one from another checkout).
powershell -NoProfile -Command "$d=[IO.Path]::GetFullPath('%~dp0'); $k=0; Get-Process ResistanceFoM -EA 0 | ?{ $_.Path -and $_.Path.StartsWith($d,'OrdinalIgnoreCase') } | ForEach-Object { Stop-Process -Id $_.Id -Force; $k=1 }; if($k){ 'Closed a ResistanceFoM.exe that was still running.' }"
echo ============================================================
echo  Resistance: Fall of Man - native PC build
echo  Builds two programs: the front end (EBOOT) and the game (game.self).
echo  The first build takes a long time. Leave it running.
echo ============================================================
rem Visual Studio (vcvars64) or the portable build tools in tools_cache\ - see tools\toolchain_env.bat
call "%~dp0tools\toolchain_env.bat"
if errorlevel 1 goto fail
rem An MSYS2/Cygwin cmake found first on PATH reads D:/... as a relative path (configure then fails with paths like
rem /d/<folder>/D:/...): fall back to the CMake that comes with Visual Studio, else stop with a clear message.
if not exist tools\cmake_native_probe.cmake goto cmake_ok
cmake -P tools/cmake_native_probe.cmake >nul 2>&1 && goto cmake_ok
set "VSCMK=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMK%\CMake\bin\cmake.exe" set "PATH=%VSCMK%\CMake\bin;%VSCMK%\Ninja;%PATH%"
cmake -P tools/cmake_native_probe.cmake >nul 2>&1 || goto cmake_bad
echo Using the CMake that comes with Visual Studio - the cmake found first on PATH is not a Windows build.
goto cmake_ok
:cmake_bad
echo No Windows CMake was found. The cmake on PATH, if any, is an MSYS2/Cygwin build, which cannot build this port:
where cmake 2>nul
echo Install CMake for Windows - winget install Kitware.CMake, or https://cmake.org/download/ - or run Setup.bat, then try again.
goto fail
:cmake_ok
rem the header path goes on the cmake lines in quotes of its own: quotes nested in set "..." would leave %~dp0
rem unquoted, and a folder name with "&" or ")" would then break the line
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON"
set "FFINC=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem files dated in the future (zip/installer time zones) make Ninja regenerate forever
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fix_timestamps.ps1"
if not defined RFOM_JOBS set "RFOM_JOBS=4"
rem PGO (profile-guided optimisation): when pgo\r1.profdata exists (recorded with tools\pgo_train.bat, see README),
rem the runtime (incl. the D3D12 renderer), the hand-written and the lifted SPU code of both programs are optimised
rem with it. A profile older than the sources only loses part of the gain (changed functions are built without it).
rem Opt out: set PORT_PGO=0 or "build.bat nopgo".
set "PGOMODE=off"
set "PGOPROF="
if exist "%~dp0pgo\r1.profdata" set "PGOMODE=use"
if "%PORT_PGO%"=="0" set "PGOMODE=off"
if /i "%PORT_PGO%"=="off" set "PGOMODE=off"
for %%A in (%*) do if /i "%%~A"=="nopgo" set "PGOMODE=off"
rem pgo\r1.profdata is indexed profile format 13, which only clang 22 or newer can read (older clang stops with
rem "unsupported instrumentation profile format version" on every file).
set "CLANGMAJ=0"
for /f "tokens=3" %%V in ('clang-cl --version 2^>nul ^| findstr /b /c:"clang version"') do for /f "delims=." %%M in ("%%V") do set "CLANGMAJ=%%M"
set "CLANGFOUND=clang-cl %CLANGMAJ%"
if "%CLANGMAJ%"=="0" set "CLANGFOUND=no clang-cl"
if "%PGOMODE%"=="use" if %CLANGMAJ% LSS 22 (
  echo ERROR: this build needs LLVM / clang-cl 22 or newer, found %CLANGFOUND%.
  echo The PGO profile pgo\r1.profdata is in LLVM's profile format 13, which older clang cannot read.
  echo Run Setup.exe or Setup.bat again to update LLVM, or install LLVM 22+ from https://github.com/llvm/llvm-project/releases
  echo - or build without the profile: set PORT_PGO=0 or run "build.bat nopgo".
  goto fail
)
if "%PGOMODE%"=="use" set "PGOPROF=%~dp0pgo\r1.profdata"
if "%PGOMODE%"=="use" echo PGO: optimising with pgo\r1.profdata - set PORT_PGO=0 or run "build.bat nopgo" to build without it

(
  echo Configuring front end...
  cmake -S project -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=ResistanceFoM -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DRFOM_SPU_AVX2=ON -DPORT_PGO=%PGOMODE% "-DPORT_PGO_PROFILE=%PGOPROF%" %FFOPT% "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure.log & type logs\configure.log & goto fail)
)
echo [1/2] Building front end (EBOOT)...
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_tee.ps1" -BuildDir build -Jobs "%RFOM_JOBS%" -Log logs\build.log
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build.log & goto fail)

(
  echo Configuring game...
  cmake -S project -B build_game -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=ResistanceFoMGame -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled_game" -DRFOM_SPU_AVX2=ON -DPORT_PGO=%PGOMODE% "-DPORT_PGO_PROFILE=%PGOPROF%" %FFOPT% "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure_game.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure_game.log & type logs\configure_game.log & goto fail)
)
echo [2/2] Building game (game.self)...
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_tee.ps1" -BuildDir build_game -Jobs "%RFOM_JOBS%" -Log logs\build_game.log
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build_game.log & goto fail)

echo Building the launcher...
set "LRES="
rc /nologo /i project /fo build\launcher.res project\launcher.rc > logs\build_launcher_rc.log 2>&1 && set "LRES=build\launcher.res"
clang-cl /nologo /O2 /EHsc /W0 project\launcher.cpp /Fo:build\launcher.obj /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LRES% user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs\build_launcher.log 2>&1
if errorlevel 1 (echo Launcher build failed - see logs\build_launcher.log) else (echo Launcher.exe built)
rem RFoMSwitch.exe keeps a black cover over the game window while a level load switches programs
clang-cl /nologo /O2 /EHsc /W0 project\rfom_switch.cpp /Fo:build\rfom_switch.obj /Fe:build\RFoMSwitch.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib > logs\build_switch.log 2>&1
if errorlevel 1 (echo RFoMSwitch build failed - see logs\build_switch.log) else (copy /Y build\RFoMSwitch.exe build_game\ >nul)
rem HDUIPack.exe: the launcher's Create HD UI pack button (project\hd_ui_pack.cpp + hd_ui_game_r1.h; reads the
rem game's archive, upscales with tools\hdui_upscaler on the GPU (CPU if that fails), writes mods\hd_ui). Failure only disables that button.
if not exist build\hdui mkdir build\hdui
clang-cl /nologo /O2 /EHsc /W0 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I ps3recomp\include /I ps3recomp\libs\video /I ps3recomp\third_party\miniz /I ps3recomp\third_party\stb /I project project\hd_ui_pack.cpp ps3recomp\libs\video\rsx_texture_layout.c ps3recomp\libs\video\rsx_native_hot.c ps3recomp\third_party\miniz\miniz.c /Fo:build\hdui\ /Fe:HDUIPack.exe /link urlmon.lib bcrypt.lib > logs\build_hdui.log 2>&1
if errorlevel 1 (echo HDUIPack build failed - see logs\build_hdui.log) else (echo HDUIPack.exe built)

rem shaderscan: the launcher's optional offline shader-cache seeding. It lives in its own repository
rem (https://github.com/miroslavdanutalexandru-alt/shaderscan, GPL-2.0); tools\get_shaderscan.ps1 downloads the release
rem pinned in tools\shaderscan.version into tools\shaderscan.exe and checks its SHA-256 (set SHADERSCAN_EXE=<exe> to
rem use your own build). Offline or failed: the launcher works without it.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File tools\get_shaderscan.ps1 || ver >nul
rem FFmpeg (video decode) is loaded at run time: Setup.bat puts the DLLs in tools\ffmpeg_bin
for %%D in (build build_game) do for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %%D\ >nul
rem AMD FidelityFX (FSR 3.1, the anti-aliasing option): AMD's signed DLLs, MIT licence (tools\ffx_bin\LICENSE.txt)
for %%D in (build build_game) do for %%F in (amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll) do if exist "tools\ffx_bin\%%F" copy /Y "tools\ffx_bin\%%F" %%D\ >nul
rem AMD FSR 3 frame generation (the "FSR 3 2x" Frame Generation choice, game program only): optional, setup.ps1 downloads it
if exist "tools\ffx_bin\amd_fidelityfx_framegeneration_dx12.dll" copy /Y "tools\ffx_bin\amd_fidelityfx_framegeneration_dx12.dll" build_game\ >nul
rem NVIDIA DLSS (the "NVIDIA DLSS" option): rfom_dlss.dll is the only binary linking the NGX SDK, which setup.ps1
rem downloads into tools\dlss (NVIDIA RTX SDK licence in tools\dlss\LICENSE.txt); nvngx_dlss.dll is NVIDIA's redistributable.
if exist "tools\dlss\lib\nvsdk_ngx_s.lib" (
  clang-cl /nologo /O2 /EHsc /MT /LD /W0 /Itools\dlss\include project\rfom_dlss.cpp /Fo:build\rfom_dlss.obj /Fe:build\rfom_dlss.dll /link tools\dlss\lib\nvsdk_ngx_s.lib d3d12.lib advapi32.lib user32.lib > logs\build_dlss.log 2>&1
  if errorlevel 1 (echo DLSS bridge build failed - see logs\build_dlss.log) else (
    echo rfom_dlss.dll built
    copy /Y build\rfom_dlss.dll build_game\ >nul
    for %%D in (build build_game) do copy /Y "tools\dlss\nvngx_dlss.dll" %%D\ >nul
  )
)
rem NVIDIA DLSS Frame Generation (the "Frame Generation" option): rfom_dlssg.dll talks to NVIDIA Streamline (MIT),
rem which setup.ps1 downloads into tools\streamline; the sl.*.dll plugins and nvngx_dlssg.dll are NVIDIA's signed
rem redistributables (tools\streamline\license.txt, nvngx_dlss.license.txt). Only the game program uses them.
if exist "tools\streamline\include\sl.h" if exist "tools\streamline\bin\x64\sl.interposer.dll" (
  clang-cl /nologo /O2 /EHsc /MT /LD /W0 /Itools\streamline\include project\rfom_dlssg.cpp /Fo:build\rfom_dlssg.obj /Fe:build\rfom_dlssg.dll /link d3d12.lib dxgi.lib wintrust.lib crypt32.lib advapi32.lib user32.lib > logs\build_dlssg.log 2>&1
  if errorlevel 1 (echo Frame generation bridge build failed - see logs\build_dlssg.log) else (
    echo rfom_dlssg.dll built
    copy /Y build\rfom_dlssg.dll build_game\ >nul
    for %%F in (sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll) do copy /Y "tools\streamline\bin\x64\%%F" build_game\ >nul
  )
)
rem Intel XeSS (the "Intel XeSS" option): Intel's redistributable libxess.dll, unmodified, under the Intel Simplified
rem Software License (tools\xess\LICENSE.txt, third-party-programs.txt). Missing = the option falls back to FSR 3.1.
for %%D in (build build_game) do if exist "tools\xess\bin\libxess.dll" copy /Y "tools\xess\bin\libxess.dll" %%D\ >nul
echo.
echo BUILD SUCCEEDED: build\ResistanceFoM.exe and build_game\ResistanceFoMGame.exe
echo Start the game with Launcher.exe
if not defined NOPAUSE pause
exit /b 0
:fail
if not defined NOPAUSE pause
exit /b 1
