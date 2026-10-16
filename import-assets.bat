@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Import-GameAssets.ps1" %*
set "IMPORT_EXIT=%ERRORLEVEL%"
if not "%IMPORT_EXIT%"=="0" pause
exit /b %IMPORT_EXIT%
