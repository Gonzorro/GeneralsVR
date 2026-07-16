@echo off
title GeneralsVR
rem Double-click me! Runs the GeneralsVR launcher next to this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0GeneralsVR.ps1" %*
if errorlevel 1 pause
