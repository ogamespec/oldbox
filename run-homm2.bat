@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ------------------------------------------------------------------
rem  OldBox test launcher: Heroes of Might and Magic II (DOS).
rem  A: = FreeDOS games floppy with HIMEMX, mouse and QCDROM/MSCDEX
rem  C: = 500 MB hard disk
rem  D: = Heroes II CD (data track extracted to a 2048-byte ISO)
rem
rem  Usage:
rem      run-homm2.bat
rem      run-homm2.bat "D:\path\to\heroes2.iso"
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

rem The retail CD is a BIN/CUE with a 2352-byte MODE1 data track followed by
rem CD audio tracks.  oldbox needs a 2048-byte ISO, so use the extracted data
rem track and keep the original CUE as the source for a rebuild.
set "CDISO=assets\cdrom\heroes2.iso"
set "CDSRC=C:\Isos\Heroes of Might and Magic 2.cue"

rem An .iso can be passed as the first argument; a .cue/.bin is converted.
if not "%~1"=="" (
    if /i "%~x1"==".iso" (
        set "CDISO=%~1"
    ) else (
        set "CDSRC=%~1"
    )
)

if not exist "%CDISO%" (
    if not exist "%CDSRC%" (
        echo [ERROR] Heroes II CD image not found.
        echo         Expected: "%CDISO%"
        echo         Source:   "%CDSRC%"
        echo         Put the original BIN/CUE into C:\Isos, or pass the image:
        echo             run-homm2.bat "D:\path\Heroes2.iso"
        pause
        exit /b 1
    )
    echo   Extracting the 2048-byte data track from the BIN/CUE...
    where py >nul 2>nul
    if not errorlevel 1 (
        py -3 tools\extract_cd_iso.py "%CDSRC%" --out "%CDISO%"
    ) else (
        python tools\extract_cd_iso.py "%CDSRC%" --out "%CDISO%"
    )
    if errorlevel 1 (
        echo [ERROR] Could not extract "%CDSRC%".
        pause
        exit /b 1
    )
)

if not exist "%CDISO%" (
    echo [ERROR] %CDISO% was not created.
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
echo   D: Heroes II CD  ("%CDISO%")
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
echo   Note: the ISO contains the data track only; oldbox does not play
echo   CD audio, so choose MIDI music in the game's sound options.
echo.

"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%GAMEFDD%" --cdrom "%CDISO%" %HDDARG%
exit /b %ERRORLEVEL%
