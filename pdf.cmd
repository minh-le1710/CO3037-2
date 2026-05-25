@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0build-report.ps1"
exit /b %errorlevel%
