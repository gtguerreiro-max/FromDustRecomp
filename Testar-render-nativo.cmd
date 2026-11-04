@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\testar_render_nativo.ps1"
if errorlevel 1 pause
