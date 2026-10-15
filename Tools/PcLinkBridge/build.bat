@echo off
rem Builds bin\PcLinkServer.exe (64-bit) and bin\x86\PcLinkServer.exe (32-bit, for 32-bit Windows)
rem with the Visual Studio C++ tools. "build test" also builds and runs the simulation (test\sim.cpp).
setlocal
cd /d "%~dp0"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
	echo The Visual Studio C++ tools were not found.
	exit /b 1
)
call :build x64 bin || exit /b 1
call :build x86 bin\x86 || exit /b 1
if /i "%~1"=="test" (
	bin\sim.exe || exit /b 1
)
exit /b 0

:build
setlocal
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 1
if not exist %2 mkdir %2
set "FLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8"
cl %FLAGS% server\main.cpp /Fo:%2\ /Fe:%2\PcLinkServer.exe advapi32.lib setupapi.lib cfgmgr32.lib winmm.lib || exit /b 1
if "%1"=="x64" (
	cl %FLAGS% test\sim.cpp /Fo:%2\ /Fe:%2\sim.exe || exit /b 1
)
exit /b 0
