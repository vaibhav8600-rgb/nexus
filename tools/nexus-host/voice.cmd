@echo off
rem Double-click to talk to Buddy out loud, with the companion running. Opens
rem a small window; no console stays behind.
start "" powershell -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0nexus_host.ps1" -Voice
