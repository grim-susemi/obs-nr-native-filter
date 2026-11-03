@echo off
setlocal
title OBS Native NR
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" -Uninstall %*
exit /b %ERRORLEVEL%
