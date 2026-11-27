@echo off
rem Second game copy on the same PC, as another player.
rem Same as trace_run.bat but its own player name, P2P port and log file.
setlocal
set "PS3_NP_ONLINE_ID=Player2"
set "PS3_NET_P2P_PORT=3659"
set "NET_TRACE_LOG=logs\net_trace2.log"
call "%~dp0trace_run.bat"
