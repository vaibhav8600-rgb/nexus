@echo off
rem Double-click to talk to Buddy out loud, with the companion running.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0nexus_host.ps1" -Voice
pause
