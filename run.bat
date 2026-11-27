@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
rem settings and controls are also kept in Saved Games\Resistance Fall of Man\config, so they survive a reinstall or update:
rem missing files are restored from there before the game starts and saved back after it closes. portable.txt turns this off.
set "CFG_USER=%USERPROFILE%\Saved Games\Resistance Fall of Man\config"
set "CFG_FILES=settings.ini settings.bat controls.txt mouselook.cfg"
if not exist portable.txt if exist "%CFG_USER%" robocopy "%CFG_USER%" . %CFG_FILES% /XO /XN /XC /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
rem RFOM_BUILD_DIR: run another build of the programs (a Launcher.exe in that folder sets it); on a CPU without AVX2
rem (the launcher sets PORT_CPU_V3=0) the build_baseline ones when they have been built (build_cpu.bat baseline)
if not defined RFOM_BUILD_DIR (
  set "RFOM_BUILD_DIR=build"
  if "%PORT_CPU_V3%"=="0" if exist build_baseline\ResistanceFoM.exe set "RFOM_BUILD_DIR=build_baseline"
)
rem missing files: the message goes to the console and logs\run.log; no pause when the launcher started this file hidden
rem (it sets PORT_NOPAUSE=1 and checks these files itself first)
if not exist "%RFOM_BUILD_DIR%\ResistanceFoM.exe" (set "RUNERR=The game is not built yet - run Setup.bat first." & goto missing)
if not exist game\EBOOT.elf (set "RUNERR=The game program game\EBOOT.elf is missing - run Setup.bat again to decrypt it from your copy." & goto missing)
rem ---- settings the port needs (Launcher.exe's choices come after, from settings.bat) ----
set "SCREEN_AS_TEX=0"
set "CHILD_RSX_ACCUM_FRAME=1"
set "CHILD_RFOM_FPS=60"
set "CHILD_SPURS_PM_YIELD=0"
rem shader debug dump (logs\fp_*.hlsl / vs_*.hlsl, for bug reports only): set CHILD_FP_SAVE=1 / CHILD_VS_SAVE=1
set "CHILD_FREEZE_RELAUNCH=900"
set "CHILD_SPU_JOB_INTERP=0"
set "CHILD_RSX_MIDFLUSH=1"
rem developer diagnostics are off in normal play. For a bug report only (not needed otherwise):
rem   set "PS3_DEBUG=%~dp0logs\dbg.txt" + set "CHILD_PS3_FTRACE=99999:6:%~dp0logs\ftrace.txt" = F7 frame trace / F11 profile keys,
rem   set "PORT_DEBUG_KEYS=1" = the renderer's debug keys. These keys overlap the photo mode / benchmark / extras keys.
rem your copy of the game: gamefiles\ (Setup.bat put it there), or the folder set in the launcher (Saves and tools,
rem Game files folder: settings.bat PORT_GAMEFILES). A PS3_VFS_ROOT set before this line (in the environment, or by
rem hand right here, like set "PS3_VFS_ROOT=E:\Games\gamefiles") wins over both. Updates replace run.bat, so prefer
rem the launcher setting.
rem Launcher.exe writes settings.bat (display mode, frame limit, vsync, filtering, keys)
if exist settings.bat call settings.bat
if not defined PS3_VFS_ROOT if defined PORT_GAMEFILES set "PS3_VFS_ROOT=%PORT_GAMEFILES%"
if not defined PS3_VFS_ROOT set "PS3_VFS_ROOT=%~dp0gamefiles"
if not exist "%PS3_VFS_ROOT%\PS3_GAME\USRDIR\" (set "RUNERR=Your game files were not found - run Setup.bat, or check the game files folder in the launcher, Saves and tools page." & goto missing)
rem portable.txt: saves stay in this folder (gamedata) too
if exist portable.txt set "RFOM_SAVE_DIR=portable"
rem shader cache backup (outside the game folder, so Setup/updates can't wipe it): restore missing files before, save new ones after
set "SHC_BACKUP=%LOCALAPPDATA%\ResistancePort\shadercache"
if exist "%SHC_BACKUP%" robocopy "%SHC_BACKUP%" shadercache /E /XO /XN /XC /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
echo Running... (log: %~dp0logs\run.log). Close the game window to stop.
rem output goes to logs\run.log only (Launcher.exe starts this file hidden, so there is no console window)
"%RFOM_BUILD_DIR%\ResistanceFoM.exe" "%~dp0game\EBOOT.elf" > logs\run.log 2>&1
rem the game can hand over to a new process (level loads, freeze restart): wait until none is left
:waitgame
for %%E in (ResistanceFoM.exe ResistanceFoMGame.exe) do tasklist /FI "IMAGENAME eq %%E" 2>nul | find /i "%%E" >nul && (ping -n 3 127.0.0.1 >nul & goto waitgame)
if exist shadercache robocopy shadercache "%SHC_BACKUP%" /E /XO /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
if not exist portable.txt robocopy . "%CFG_USER%" %CFG_FILES% /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
exit /b 0
:missing
echo %RUNERR%
>logs\run.log echo %RUNERR%
if not defined PORT_NOPAUSE pause
exit /b 1
