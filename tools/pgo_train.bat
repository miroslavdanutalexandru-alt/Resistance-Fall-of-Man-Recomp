@echo off
rem PGO step 2 of 4: play the instrumented build (build_cpu.bat pgo-gen, or scripts\pgo_build_instrumented.ps1)
rem to record a profile. Play normally for 10-20 minutes and cover what matters for speed: the heaviest fights,
rem a busy outdoor level, a level load (the hand-off to the level program), a checkpoint reload, cutscenes and
rem the menus. The instrumented build runs slower than normal (roughly 10-30%); that does not matter for the
rem profile. Counters are written every PORT_PGO_DUMP_S seconds (default 30) and when a program exits, to
rem pgo\raw\r1-<pid>.profraw (the front end and the level program each write their own file; all are merged).
rem Several sessions are fine: run this again and everything in pgo\raw is merged.
rem Then: powershell -ExecutionPolicy Bypass -File scripts\pgo_merge_and_build.ps1 (merge -> pgo\r1.profdata +
rem build_cpu.bat pgo-use). From then on the normal build.bat uses pgo\r1.profdata as well.
setlocal
cd /d "%~dp0.."
if not exist build_pgo_gen\ResistanceFoMGame.exe (echo build_pgo_gen\ResistanceFoMGame.exe not found - run build_cpu.bat pgo-gen first. & pause & exit /b 1)
if not exist pgo\raw mkdir pgo\raw
set "LLVM_PROFILE_FILE=%CD%\pgo\raw\r1-%%p.profraw"
set "RFOM_BUILD_DIR=build_pgo_gen"
if not defined PORT_PGO_DUMP_S set "PORT_PGO_DUMP_S=30"
echo Recording a PGO profile with build_pgo_gen\ -^> pgo\raw\
call run.bat
echo.
dir /b pgo\raw\*.profraw 2>nul || echo No .profraw written - check logs\run.log.
echo Next: powershell -ExecutionPolicy Bypass -File scripts\pgo_merge_and_build.ps1
pause