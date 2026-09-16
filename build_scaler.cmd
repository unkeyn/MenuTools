@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_scaler.ps1"
exit /b %ERRORLEVEL%
