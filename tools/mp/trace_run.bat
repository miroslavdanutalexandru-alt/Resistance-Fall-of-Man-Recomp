@echo off
rem ---------------------------------------------------------------------------
rem Multiplayer: run the game online with packet logging, windowed.
rem The log goes to logs\net_trace.log so logs\run.log is left alone.
rem Online play is turned on for this run only; the other online defaults
rem (hosts, player name) come from main.cpp.
rem
rem Close the game window to stop. Nothing is copied back to Saved Games.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0..\.."
set "ROOT=%CD%"
if not exist logs mkdir logs

if not defined RFOM_BUILD_DIR set "RFOM_BUILD_DIR=build"
if not exist "%RFOM_BUILD_DIR%\ResistanceFoM.exe" (echo Not built. & pause & exit /b 1)

rem ---- settings the port needs (copied from run.bat) ----
set "SCREEN_AS_TEX=0"
set "CHILD_RSX_ACCUM_FRAME=1"
set "CHILD_RFOM_FPS=60"
set "CHILD_SPURS_PM_YIELD=0"
set "CHILD_FP_SAVE=1"
set "CHILD_VS_SAVE=1"
set "CHILD_SPU_JOB_INTERP=0"
set "CHILD_RSX_MIDFLUSH=1"
set "PS3_DEBUG=%ROOT%\logs\dbg.txt"
set "PS3_VFS_ROOT=%ROOT%\gamefiles"
if exist settings.bat call settings.bat

rem ---- windowed, so the trace run does not take over the screen ----
set "RSX_DISPLAY=windowed"
set "RSX_WINDOW=1280x720"
set "RSX_FOCUS_PAUSE=0"

rem ---- online on for this run (it is off by default), with tracing ----
set "PS3_NET_ONLINE=1"
set "PS3_NET_TRACE=1"
set "RFOM_MP_TRACE=1"
rem A server on this PC: its TLS root (the server's data\svo_root.der copied to
rem logs\mp\svo_root.der) replaces the game's at load time instead of r1_root.der.
if not defined PS3_MEMPATCH if exist "%ROOT%\logs\mp\svo_root.der" set "PS3_MEMPATCH=10038310:%ROOT%\logs\mp\svo_root.der"
rem Overridable so a second copy can run as another player (run_player2.bat).
if not defined PS3_NP_ONLINE_ID set "PS3_NP_ONLINE_ID=TestPlayer"
if not defined PS3_NET_P2P_PORT set "PS3_NET_P2P_PORT=3658"
if not defined NET_TRACE_LOG set "NET_TRACE_LOG=logs\net_trace.log"

echo Tracing network to %NET_TRACE_LOG% ...
"%RFOM_BUILD_DIR%\ResistanceFoM.exe" "%ROOT%\game\EBOOT.elf" > "%NET_TRACE_LOG%" 2>&1

:waitgame
for %%E in (ResistanceFoM.exe ResistanceFoMGame.exe) do tasklist /FI "IMAGENAME eq %%E" 2>nul | find /i "%%E" >nul && (ping -n 3 127.0.0.1 >nul & goto waitgame)
echo Done. Trace: %ROOT%\%NET_TRACE_LOG%
