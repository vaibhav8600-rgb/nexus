@echo off
rem Double-click: stop the NEXUS companion and stop it starting with Windows.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0nexus_host.ps1" -Uninstall
pause
