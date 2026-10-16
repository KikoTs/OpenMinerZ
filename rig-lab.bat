@echo off
setlocal
pushd "%~dp0"
if not exist "out\build\native-release\Release\openminerz_mod_sdk.exe" (
    echo Build the SDK first: cmake --build --preset native-release --target openminerz_mod_sdk
    pause
    popd
    exit /b 1
)
py -3 tools\ModSdk\rig_lab.py --prepare --open %*
set "RIG_LAB_EXIT=%ERRORLEVEL%"
popd
if not "%RIG_LAB_EXIT%"=="0" pause
exit /b %RIG_LAB_EXIT%
