@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ------------------------------------------------------------------
rem  OldBox test launcher: Heroes of Might and Magic II (DOS).
rem  A: = FreeDOS games floppy with HIMEMX, mouse and QCDROM/MSCDEX
rem  C: = 500 MB hard disk
rem  D: = Heroes II CD (native BIN/CUE, data and CD-DA tracks)
rem
rem  Usage:
rem      run-homm2.bat
rem      run-homm2.bat "D:\path\to\Heroes of Might and Magic 2.cue"
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

rem Mount the retail BIN/CUE directly: oldbox reads the 2352-byte MODE1 data
rem track as 2048-byte logical blocks and exposes the audio tracks as CD-DA.
rem An extracted .iso still works as a fallback for older asset trees.
set "CDIMAGE=C:\Isos\Heroes of Might and Magic 2.cue"
if not "%~1"=="" set "CDIMAGE=%~1"
if not exist "%CDIMAGE%" (
    if exist "assets\cdrom\heroes2.iso" set "CDIMAGE=assets\cdrom\heroes2.iso"
)
if not exist "%CDIMAGE%" (
    echo [ERROR] Heroes II CD image not found.
    echo         Expected: C:\Isos\Heroes of Might and Magic 2.cue
    echo         Put the original BIN/CUE into C:\Isos, or pass the image:
    echo             run-homm2.bat "D:\path\Heroes of Might and Magic 2.cue"
    echo             run-homm2.bat "D:\path\Heroes2.iso"
    pause
    exit /b 1
)
if not exist "%GAMEFDD%" (
    echo [ERROR] %GAMEFDD% not found. Build it with:
    echo         py -3 tools\make_games_floppy.py --cd
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
echo   OldBox - Heroes of Might and Magic II (DOS)
echo ==============================================================
echo.
echo   A: FreeDOS 1.2 + HIMEMX (DOS=HIGH) + MOUSE.COM (PS/2)
echo      + QCDROM.SYS + MSCDEX.EXE
if defined HDDARG echo   C: 500 MB HDD  ("%HDD%")
echo   D: Heroes II CD  ("%CDIMAGE%")
echo.
echo   At the A:\^> prompt type:
echo       D:
echo       CD \HEROES2
echo       INSTALL
echo.
echo   Install to C:\HEROES2 (about 60 MB), then run:
echo       C:
echo       CD \HEROES2
echo       HEROES2
echo.
echo   Mouse: click the window or press F1 to capture/release it.
echo.
echo   Note: CD-DA tracks are played through the AWE64 CD input, so choose
echo   CD Audio music in the game's sound options (MIDI also works).
echo.

"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%GAMEFDD%" --cdrom "%CDIMAGE%" %HDDARG%
exit /b %ERRORLEVEL%
