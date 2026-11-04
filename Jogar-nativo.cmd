@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" -Renderer native
if errorlevel 1 pause
