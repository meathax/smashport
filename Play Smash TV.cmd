@echo off
setlocal
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Play
if errorlevel 1 pause
