@echo off
rem Double-click: the NEXUS companion starts with Windows from now on, and now.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0nexus_host.ps1" -Install
pause
