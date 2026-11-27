@echo off
rem CPU-specific builds of both programs (front end + game), each in its own folder (build\ and build_game\ are not touched).
rem   build_cpu.bat            -> build_cpu\ResistanceFoM.exe + ResistanceFoMGame.exe       AVX2 everywhere + ThinLTO
rem   build_cpu.bat baseline   -> build_baseline\ResistanceFoM.exe + ResistanceFoMGame.exe  no AVX (older CPUs; run.bat picks it there)
rem   build_cpu.bat pgo-gen    -> build_pgo_gen\...   instrumented (runtime, hand-written, SPU), for tools\pgo_train.bat
rem   build_cpu.bat pgo-use    -> build_pgo\...       AVX2 + ThinLTO + PGO from pgo\r1.profdata
rem Add "nopause" as the last argument when calling from a script. PGO steps: scripts\pgo_build_instrumented.ps1,
rem tools\pgo_train.bat, scripts\pgo_merge_and_build.ps1 (build.bat itself uses pgo\r1.profdata when it exists).
rem Both programs end up in the one folder (the game finds its sibling there); the front end is built in <folder>\,
rem the game in <folder>_game\ and copied over. PORT_JOBS (default 4) compile jobs, at below-normal priority.
rem PORT_LTO=0 skips ThinLTO.
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist recompiled_game\ppu_recomp.h (echo The game code has not been generated yet - run Setup.bat first. & goto fail)
set "MODE=%~1"
if "%MODE%"=="" set "MODE=avx2"
if /i "%MODE%"=="nopause" (set "MODE=avx2" & set "NOPAUSE=1")
if /i "%~2"=="nopause" set "NOPAUSE=1"
if not defined PORT_JOBS set "PORT_JOBS=4"
set "LTO=ON"
if "%PORT_LTO%"=="0" set "LTO=OFF"
if /i "%MODE%"=="avx2"     (set "BDIR=build_cpu"      & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=off")
if /i "%MODE%"=="baseline" (set "BDIR=build_baseline" & set "OPTS=-DPORT_ARCH=baseline -DPORT_LTO=%LTO% -DPORT_PGO=off")
if /i "%MODE%"=="pgo-gen"  (set "BDIR=build_pgo_gen"  & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=OFF -DPORT_PGO=gen")
if /i "%MODE%"=="pgo-use" (
  if not exist pgo\r1.profdata (echo pgo\r1.profdata not found - run tools\pgo_train.bat and scripts\pgo_merge_and_build.ps1 first. & goto fail)
  set "BDIR=build_pgo" & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=use"
)
set "PGOPROF="
if /i "%MODE%"=="pgo-use" set "PGOPROF=%~dp0pgo\r1.profdata"
if not defined BDIR (echo Unknown mode "%MODE%": use avx2, baseline, pgo-gen or pgo-use. & goto fail)

rem Visual Studio (vcvars64) or the portable build tools in tools_cache\ - see tools\toolchain_env.bat
call "%~dp0tools\toolchain_env.bat"
if errorlevel 1 goto fail
rem the header path goes on the cmake lines in quotes of its own: quotes nested in set "..." would leave %~dp0
rem unquoted, and a folder name with "&" or ")" would then break the line
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON"
set "FFINC=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
set "COMMON=-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRFOM_SPU_AVX2=ON %OPTS% %FFOPT%"

echo ============================================================
echo  Resistance: Fall of Man - CPU build "%MODE%" into %BDIR%\
echo ============================================================
cmake -S project -B %BDIR% -DPORT_NAME=ResistanceFoM -DRECOMP_DIR="%~dp0recompiled" -DPS3RECOMP_DIR="%~dp0ps3recomp" %COMMON% "-DPORT_PGO_PROFILE=%PGOPROF%" "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure_%BDIR%.log 2>&1
if errorlevel 1 (echo Configure FAILED - see logs\configure_%BDIR%.log & goto fail)
cmake -S project -B %BDIR%_game -DPORT_NAME=ResistanceFoMGame -DRECOMP_DIR="%~dp0recompiled_game" -DPS3RECOMP_DIR="%~dp0ps3recomp" %COMMON% "-DPORT_PGO_PROFILE=%PGOPROF%" "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure_%BDIR%_game.log 2>&1
if errorlevel 1 (echo Configure FAILED - see logs\configure_%BDIR%_game.log & goto fail)
findstr /c:"CPU target" /c:"LTO:" /c:"PGO:" logs\configure_%BDIR%_game.log
for %%B in (%BDIR% %BDIR%_game) do (
  echo Building %%B ^(below-normal priority, %PORT_JOBS% jobs; log logs\build_%%B.log^)...
  "%PS%" -NoProfile -Command "(Get-Process -Id $PID).PriorityClass = 'BelowNormal'; cmake --build %%B -j %PORT_JOBS% 2>&1 | Tee-Object -FilePath logs\build_%%B.log | Select-String -CaseSensitive -Pattern '^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking' | ForEach-Object { $_.Line }; $c = $LASTEXITCODE; if (Select-String -Path logs\build_%%B.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
  if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build_%%B.log & goto fail)
)
copy /Y %BDIR%_game\ResistanceFoMGame.exe %BDIR%\ >nul
if exist %BDIR%_game\ResistanceFoMGame.pdb copy /Y %BDIR%_game\ResistanceFoMGame.pdb %BDIR%\ >nul
rem the DLLs and RFoMSwitch.exe build.bat put next to the normal builds (FFmpeg, FSR, DLSS, XeSS, Streamline)
for %%D in (build build_game) do for %%F in (%%D\*.dll %%D\RFoMSwitch.exe) do copy /Y "%%F" %BDIR%\ >nul 2>&1
echo.
echo BUILD SUCCEEDED: %BDIR%\ResistanceFoM.exe and %BDIR%\ResistanceFoMGame.exe
if /i "%MODE%"=="avx2" echo Run it with: set RFOM_BUILD_DIR=build_cpu ^& run.bat
if /i "%MODE%"=="pgo-gen" echo Next: tools\pgo_train.bat (plays the instrumented build), then scripts\pgo_merge_and_build.ps1
if /i "%MODE%"=="pgo-use" echo Run it with: set RFOM_BUILD_DIR=build_pgo ^& run.bat
if not defined NOPAUSE pause
exit /b 0
:fail
if not defined NOPAUSE pause
exit /b 1
