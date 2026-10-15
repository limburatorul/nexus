@echo off
rem Builds build\bridger.exe (XP SP3 .. Windows 11) and build\Bridger-Setup-<version>.exe.
rem Needs MinGW-w64 i686 with the msvcrt runtime (WinLibs) on PATH or in %MINGW%, and NSIS 3.
setlocal
cd /d "%~dp0"
if defined MINGW set "PATH=%MINGW%\bin;%PATH%"
where gcc >nul 2>nul || if exist "%USERPROFILE%\tools\mingw32\bin\gcc.exe" set "PATH=%USERPROFILE%\tools\mingw32\bin;%PATH%"
where gcc >nul 2>nul || (echo gcc not found: set MINGW to the WinLibs i686 folder & exit /b 1)

set /p VERSION=<VERSION
for /f "tokens=1-3 delims=." %%a in ("%VERSION%") do set "MAJ=%%a" & set "MIN=%%b" & set "PAT=%%c"
if not exist build mkdir build

rem 0x0501 hides post-XP APIs in the headers; the subsystem version lets XP's loader accept the exe.
(echo #define BRIDGER_VERSION "%VERSION%"& echo #define VER_MAJOR %MAJ%& echo #define VER_MINOR %MIN%& echo #define VER_PATCH %PAT%) > build\version.h
set DEFS=-DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D_WIN32_IE=0x0600 -Ibuild
windres -I src -I build src\bridger.rc -O coff -o build\bridger.res || exit /b 1
gcc -std=gnu11 -Os -Wall -Wextra -march=i686 -mtune=generic -municode -mwindows -static %DEFS% ^
    src\bridger.c build\bridger.res -o build\bridger.exe ^
    -Wl,--subsystem,windows:5.01 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -s ^
    -lgdi32 -luser32 -lshell32 -ladvapi32 || exit /b 1

python tools\check_xp_imports.py build\bridger.exe || exit /b 1

set "NSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%NSIS%" set "NSIS=makensis"
"%NSIS%" /V2 /DVERSION=%VERSION% installer\bridger.nsi || exit /b 1
echo Built build\bridger.exe and build\Bridger-Setup-%VERSION%.exe
