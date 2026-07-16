@echo off
title GeneralsVR Setup
rem Downloads the current GeneralsVR installer script and runs it.
rem The script finds your Steam Zero Hour install, fetches the newest release,
rem sets up the registry + a desktop shortcut, and offers to start the game.

set "PS1=%TEMP%\GeneralsVR.ps1"
curl.exe -fsSL "https://raw.githubusercontent.com/Gonzorro/GeneralsVR/feature/openxr-vr/installer/GeneralsVR.ps1" -o "%PS1%"
if errorlevel 1 (
    echo.
    echo Could not download the installer script - are you online?
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Install
pause
