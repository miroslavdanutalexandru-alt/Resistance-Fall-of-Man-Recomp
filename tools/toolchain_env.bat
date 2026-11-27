@echo off
rem Sets up the C/C++ build tools for build.bat and build_cpu.bat (they "call" it: no setlocal, the variables stay).
rem  * The portable build tools in tools_cache\ (Setup's portable mode, tools\installer\buildtools.ps1) come first
rem    on PATH. With the portable MSVC libraries + Windows SDK (tools_cache\msvc, made by xwin) INCLUDE and LIB
rem    point there and Visual Studio is not needed; tools_cache\shims holds link.exe / rc.exe / mt.exe / lib.exe
rem    (copies of LLVM's lld-link / llvm-rc / llvm-mt / llvm-lib).
rem  * Otherwise Visual Studio's vcvars64.bat (found with vswhere), as before.
rem  * PORT_TOOLCHAIN=system ignores tools_cache\. PORT_TOOLS_CACHE=<folder> uses another folder (tests).
rem Sets VSDIR (empty with the portable MSVC libraries). Exit code 1 = no usable C++ libraries / linker found.
set "VSDIR="
set "PORT_TC_PATH="
set "PORT_TC_MSVC="
if not defined PORT_TOOLS_CACHE set "PORT_TOOLS_CACHE=%~dp0..\tools_cache"
if /i "%PORT_TOOLCHAIN%"=="system" goto vs
set "TC=%PORT_TOOLS_CACHE%"
set "TCA="
if not exist "%TC%\msvc\crt\include\vcruntime.h" goto tc_paths
if not exist "%TC%\msvc\sdk\include\um\windows.h" goto tc_paths
if not exist "%TC%\msvc\sdk\include\ucrt\stdio.h" goto tc_paths
for %%A in (x86_64 x64) do if not defined TCA if exist "%TC%\msvc\crt\lib\%%A\" if exist "%TC%\msvc\sdk\lib\um\%%A\kernel32.lib" if exist "%TC%\msvc\sdk\lib\ucrt\%%A\" set "TCA=%%A"
:tc_paths
if defined TCA if exist "%TC%\shims\link.exe" set "PORT_TC_PATH=%TC%\shims;"
if exist "%TC%\llvm\bin\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\llvm\bin;"
if exist "%TC%\cmake\bin\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\cmake\bin;"
if exist "%TC%\ninja\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\ninja;"
if exist "%TC%\python\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\python;"
if exist "%TC%\python\Scripts\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\python\Scripts;"
if exist "%TC%\git\cmd\" set "PORT_TC_PATH=%PORT_TC_PATH%%TC%\git\cmd;"
if not defined TCA goto vs
if not exist "%TC%\shims\link.exe" (echo The portable build tools in tools_cache are incomplete [shims\link.exe is missing] - run Setup.bat again. & exit /b 1)
set "INCLUDE=%TC%\msvc\crt\include;%TC%\msvc\sdk\include\ucrt;%TC%\msvc\sdk\include\um;%TC%\msvc\sdk\include\shared;%TC%\msvc\sdk\include\winrt"
if exist "%TC%\msvc\sdk\include\cppwinrt\" set "INCLUDE=%INCLUDE%;%TC%\msvc\sdk\include\cppwinrt"
set "LIB=%TC%\msvc\crt\lib\%TCA%;%TC%\msvc\sdk\lib\um\%TCA%;%TC%\msvc\sdk\lib\ucrt\%TCA%"
set "PORT_TC_MSVC=1"
echo Using the portable build tools in tools_cache (set PORT_TOOLCHAIN=system to use the installed ones).
goto paths
:vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%I"
if not defined VSDIR (echo Visual Studio C++ build tools not found - run Setup.bat. & exit /b 1)
rem vcvars looks for vswhere on PATH (else it prints "'vswhere.exe' is not recognized")
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo Could not set up the Visual Studio build tools. & exit /b 1)
:paths
set "PATH=%PORT_TC_PATH%C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
rem vcvars64 picks the newest Windows SDK installed. The D3D12 renderer needs 10.0.20348 or newer: older d3d12.h
rem declare some methods with the wrong C calling convention (and lack D3D12_SDK_VERSION, which is what is checked).
set "PORT_D3D12_H="
for %%H in (d3d12.h) do set "PORT_D3D12_H=%%~$INCLUDE:H"
if not defined PORT_D3D12_H (echo The Windows SDK headers were not found [no d3d12.h on INCLUDE] - add a Windows SDK in the Visual Studio Installer or run Setup.bat again. & exit /b 1)
findstr /m /c:"D3D12_SDK_VERSION" "%PORT_D3D12_H%" >nul 2>&1
if errorlevel 1 goto sdk_old
if defined WindowsSDKVersion echo Using Windows SDK %WindowsSDKVersion:\=%
exit /b 0
:sdk_old
echo ERROR: the Windows SDK found is too old to build this port: %PORT_D3D12_H%
echo Windows SDK 10.0.20348 or newer is needed; 10.0.26100 [Windows 11 SDK] is recommended. Add it in the Visual Studio
echo Installer [Modify ^> Individual components ^> Windows 11 SDK] or run Setup.bat again, then build again.
exit /b 1
