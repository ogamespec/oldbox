@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ------------------------------------------------------------------
rem  OldBox test launcher: FreeDOS games floppy (A:) + WORLD9 CD (D:).
rem  The floppy loads HIMEMX, the PS/2 mouse driver and QCDROM/MSCDEX,
rem  so the WORLD9 disc is available as D:.
rem
rem  Usage:
rem      run-world9.bat
rem      run-world9.bat "D:\path\to\another.iso"
rem ------------------------------------------------------------------

set "EXE=build\vs2026\Release\oldbox.exe"
if not exist "%EXE%" set "EXE=build\vs2022\Release\oldbox.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found.
    echo         Build it first: run  build-windows.bat
    pause
    exit /b 1
)

set "BIOS=assets\bios\EP-ZXA.bin"
set "VGABIOS=assets\vga\S3Trio64_1_14-04.bin"
set "GAMEFDD=assets\floppy\freedos-games-cd.img"
rem Clear HDD to boot with A: and D: only (no C:).
set "HDD=assets\hdd\oldbox-hdd-500mb.img"

rem A CD image can be passed as the first argument.  Otherwise look for
rem the usual WORLD9 files in C:\Isos (.iso first, then the raw .bin,
rem finally the .cue sheet, whose FILE entry is resolved below).
if not "%~1"=="" (
    set "WORLD9=%~1"
    goto :cd_found
)
if defined WORLD9 goto :cd_found
set "WORLD9=C:\Isos\WORLD9.iso"
if exist "%WORLD9%" goto :cd_found
set "WORLD9=C:\Isos\WORLD9.bin"
if exist "%WORLD9%" goto :cd_found
set "WORLD9=C:\Isos\WORLD9.cue"
:cd_found

if not exist "%WORLD9%" goto :cd_ready
if /i not "%WORLD9:~-4%"==".cue" goto :cd_ready
for %%I in ("%WORLD9%") do set "CUEDIR=%%~dpI"
set "CUEBIN="
for /f "usebackq tokens=1,2 delims= " %%A in ("%WORLD9%") do (
    if /i "%%~A"=="FILE" if not defined CUEBIN set "CUEBIN=%%~B"
)
if not defined CUEBIN (
    echo [ERROR] No FILE entry found in "%WORLD9%".
    echo         Pass the .iso/.bin image directly as the first argument.
    pause
    exit /b 1
)
set "WORLD9=%CUEDIR%%CUEBIN%"
:cd_ready

if not exist "%WORLD9%" (
    echo [ERROR] WORLD9 CD image not found:
    echo         "%WORLD9%"
    echo         Put WORLD9.iso or WORLD9.bin into C:\Isos, or pass the
    echo         image as the first argument:
    echo             run-world9.bat "D:\path\WORLD9.iso"
    pause
    exit /b 1
)
if not exist "%GAMEFDD%" (
    echo [ERROR] %GAMEFDD% not found. Build it with:
    echo         py -3 tools\make_games_floppy.py --cd --out assets\floppy\freedos-games-cd.img
    pause
    exit /b 1
)
if not exist "%BIOS%" (
    echo [ERROR] BIOS dump not found: %BIOS%
    pause
    exit /b 1
)
if not exist "%VGABIOS%" (
    echo [ERROR] S3 Trio64 option ROM not found: %VGABIOS%
    pause
    exit /b 1
)

set "HDDARG="
if exist "%HDD%" set HDDARG=--hdd "%HDD%"

echo ==============================================================
echo   OldBox - WORLD9 on the FreeDOS games floppy
echo ==============================================================
echo.
echo   A: FreeDOS 1.2 + HIMEMX (DOS=HIGH) + MOUSE.COM (PS/2)
echo      + QCDROM.SYS + MSCDEX.EXE
if defined HDDARG echo   C: 500 MB HDD  ("%HDD%")
echo   D: WORLD9 CD  ("%WORLD9%")
echo.
echo   At the A:\^> prompt type:  D:  then  INSTALL.EXE
echo.
echo   Mouse: click the window or press F1 to capture/release it.
echo.
if not defined HDDARG echo   Note: "%HDD%" not found, booting without C:.
echo.

"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%GAMEFDD%" --cdrom "%WORLD9%" %HDDARG%
exit /b %ERRORLEVEL%
