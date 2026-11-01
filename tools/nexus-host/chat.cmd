@echo off
rem Double-click to talk to Buddy through the companion that is running.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0nexus_host.ps1" -Chat
pause
