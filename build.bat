@echo off
rem Builds dist\Pauserick.exe with MinGW-w64 GCC (e.g. w64devkit: https://github.com/skeeto/w64devkit).
rem gcc and windres must be on PATH, or set W64DEVKIT to the w64devkit folder.
rem The version is in src\version.h. Releases are built by GitHub Actions: see RELEASING.md.
setlocal
cd /d "%~dp0"
if defined W64DEVKIT set "PATH=%W64DEVKIT%\bin;%PATH%"
where gcc >nul 2>nul || (echo gcc not found. Install w64devkit and add its bin folder to PATH. & exit /b 1)

if not exist build mkdir build
if not exist dist mkdir dist

set NEED_ICONS=
if not exist src\pauserick.ico set NEED_ICONS=1
if not exist src\pauserick_paused.ico set NEED_ICONS=1
if defined NEED_ICONS (
    gcc -O2 -o build\mkicon.exe tools\mkicon.c -lm || exit /b 1
    if not exist src\pauserick.ico build\mkicon.exe src\pauserick.ico || exit /b 1
    if not exist src\pauserick_paused.ico build\mkicon.exe src\pauserick_paused.ico paused || exit /b 1
)

windres -O coff --include-dir src -i src\pauserick.rc -o build\pauserick.res || exit /b 1

gcc -std=c11 -Os -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows ^
    -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -s -Wl,--gc-sections ^
    -o dist\Pauserick.exe src\pauserick.c build\pauserick.res ^
    -lcomctl32 -lshcore -lwtsapi32 -lole32 -lruntimeobject -ldwmapi -lshell32 -ladvapi32 -lgdi32 -luser32 ^
    -lwinhttp -lbcrypt -lversion || exit /b 1

rem the signing tool (tools\pksign.c): makes the key pair; also signs, like tools\sign-release.ps1
gcc -std=c11 -O2 -o build\pksign.exe tools\pksign.c -lbcrypt || exit /b 1

echo Built dist\Pauserick.exe
