@echo off
rem Drag Pauserick.exe (downloaded from the draft release on GitHub) onto this file:
rem it writes Pauserick.exe.sig next to it, signed with your private key. See RELEASING.md.
if "%~1"=="" (echo Drag Pauserick.exe onto this file. & pause & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\sign-release.ps1" -Exe "%~1"
pause
