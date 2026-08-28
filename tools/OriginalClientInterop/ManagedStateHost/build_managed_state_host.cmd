@echo off
setlocal

set "OPENMINERZ_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%OPENMINERZ_VSWHERE%" exit /b 2
for /f "usebackq tokens=*" %%i in (`"%OPENMINERZ_VSWHERE%" -latest -products * -property installationPath`) do set "OPENMINERZ_VSROOT=%%i"
if not defined OPENMINERZ_VSROOT exit /b 3

set "OPENMINERZ_INTEROP_ROOT=%~dp0..\..\.."
set "OPENMINERZ_INTEROP_OUT=%OPENMINERZ_INTEROP_ROOT%\out\interop-managed"
"%OPENMINERZ_VSROOT%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0OpenMinerZ.OriginalStateHost.csproj" /restore /m /v:minimal /p:Configuration=Release /p:Platform=x86 /p:OutputPath="%OPENMINERZ_INTEROP_OUT%\Host"
if %errorlevel% neq 0 exit /b 4
"%OPENMINERZ_VSROOT%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0Fixture\FixtureCastleMinerZ.csproj" /restore /m /v:minimal /p:Configuration=Release /p:Platform=x86 /p:OutputPath="%OPENMINERZ_INTEROP_OUT%\Fixture"
exit /b %errorlevel%
