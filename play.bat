@echo off
setlocal
set "OPENMINERZ_GAME=%~dp0out\build\native-release\Release\openminerz.exe"
if not exist "%OPENMINERZ_GAME%" (
    echo OpenMinerZ Release build is missing.
    echo Build it with: cmake --build --preset native-release
    pause
    exit /b 1
)
pushd "%~dp0out\build\native-release\Release"
"%OPENMINERZ_GAME%"
set "OPENMINERZ_EXIT=%ERRORLEVEL%"
popd
exit /b %OPENMINERZ_EXIT%
