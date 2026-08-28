@echo off
setlocal

set "OPENMINERZ_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%OPENMINERZ_VSWHERE%" exit /b 2
for /f "usebackq tokens=*" %%i in (`"%OPENMINERZ_VSWHERE%" -latest -products * -property installationPath`) do set "OPENMINERZ_VSROOT=%%i"
if not defined OPENMINERZ_VSROOT exit /b 3

call "%OPENMINERZ_VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if %errorlevel% neq 0 exit /b 4

set "OPENMINERZ_BRIDGE_SOURCE=%~dp0."
set "OPENMINERZ_BRIDGE_BUILD=%~dp0..\..\out\build\steam-bridge32"
cmake -S "%OPENMINERZ_BRIDGE_SOURCE%" -B "%OPENMINERZ_BRIDGE_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 exit /b 5
cmake --build "%OPENMINERZ_BRIDGE_BUILD%" --parallel
if %errorlevel% neq 0 exit /b 6

"%~dp0..\..\out\steam-bridge32\OpenMinerZ.SteamBridge32.exe" --self-test
exit /b %errorlevel%
