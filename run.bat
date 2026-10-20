@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ------------------------------------------------------------------
rem  OldBox test launcher: boots the original Award BIOS (EP-ZXA.bin)
rem  in a window.  Use ESC to close the emulator, F11 = fullscreen,
rem  F12 = save a screenshot (oldbox-screenshot.bmp).
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
set "FLOPPY=assets\floppy\freedos1440.img"
set "GAMEFLOPPY=assets\floppy\freedos-games.img"
set "GAMEDISK=assets\floppy\Megafortress_3.5_Disk.IMA"
set "MEMTEST=assets\floppy\MEMTEST.IMG"
set "DD2FLOPPY=assets\floppy\dd2.img"
set "HDD=assets\hdd\oldbox-hdd-500mb.img"
if not defined WIN95ISO set "WIN95ISO=C:\Isos\Windows95OSR2.iso"
set "WIN95FLOPPY=assets\floppy\freedos-win95.img"
set "WIN95HDD=assets\hdd\win95-hdd.img"

echo ==============================================================
echo   OldBox - Award BIOS ^(EP-ZXA^) test launcher
echo ==============================================================
echo.
echo   [1] Boot BIOS, press Del yourself to enter Setup
echo   [2] Boot BIOS, Del is pressed automatically -^> Setup
echo   [3] Boot BIOS with the FreeDOS 1.44M floppy  (A: only)
echo   [4] Boot BIOS with 500 MB primary-master HDD
echo   [5] FreeDOS A: + Megafortress B: + 500 MB HDD C:
echo       ^(install/run the game: B:INSTALL^)
echo   [6] Boot the Memtest86+ floppy
echo   [7] Headless smoke test 40 s, print text screen to console
echo   [8] Headless: auto-enter Setup and save setup.ppm
echo   [9] Boot Dangerous Dave 2 (FreeDOS + DD2 on one floppy)
echo   [10] Windows 95 OSR2 setup: FreeDOS A: + MSCDEX, CD D:, HDD C:
echo.
set /p "CHOICE=Choice [1-10]: "

if "%CHOICE%"=="1" goto :win
if "%CHOICE%"=="2" goto :win_del
if "%CHOICE%"=="3" goto :win_floppy
if "%CHOICE%"=="4" goto :win_hdd
if "%CHOICE%"=="5" goto :win_game
if "%CHOICE%"=="6" goto :win_memtest
if "%CHOICE%"=="7" goto :headless
if "%CHOICE%"=="8" goto :headless_shot
if "%CHOICE%"=="9" goto :win_dd2
if "%CHOICE%"=="10" goto :win95
echo Unknown choice.
pause
exit /b 1

:win
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --hdd "%HDD%"
goto :eof

:win_del
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --hdd "%HDD%" --press-del
goto :eof

:win_floppy
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%FLOPPY%" --hdd "%HDD%"
goto :eof

:win_hdd
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --hdd "%HDD%"
goto :eof

:win_game
echo.
echo   Boot order: A: = FreeDOS (HIMEMX, DOS=HIGH), B: = Megafortress 720K, C: = 500 MB HDD
echo   Install the game once:  B:  then  INSTALL  to install to C:.
echo   Run the installed game:  C:  then  CD \MEGA  then  MEGA
echo.
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%GAMEFLOPPY%" --floppy-b "%GAMEDISK%" --hdd "%HDD%"
goto :eof

:win_memtest
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%MEMTEST%"
goto :eof

:win_dd2
echo.
echo   Boots A: = FreeDOS + Dangerous Dave 2, AUTOEXEC starts DAVE2.EXE.
echo   Built from the correctly unpacked DAVE2_UNPACKED.EXE and the original
echo   data in C:\Work\dave2\re\ref\orig (not the holey C:\Games\DD2 copy).
echo.
if not exist "%DD2FLOPPY%" (
    echo [ERROR] %DD2FLOPPY% not found. Build it with:
    echo         python tools/make_dd2_floppy.py
    pause
    exit /b 1
)
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%DD2FLOPPY%"
goto :eof

:win95
echo.
echo   Boot order: A: = FreeDOS + HIMEM.SYS + QCDROM + MSCDEX
echo               C: = clean 500 MB HDD  (%WIN95HDD%)
echo               D: = Windows 95 OSR2 CD (%WIN95ISO%)
echo.
echo   At the A:\^> prompt type:  D:  then  SETUP
echo.
if not exist "%WIN95ISO%" (
    echo [ERROR] Windows 95 OSR2 ISO not found:
    echo         %WIN95ISO%
    echo         Put it there or set WIN95ISO before running run.bat.
    pause
    exit /b 1
)
if not exist "%WIN95FLOPPY%" (
    echo [ERROR] %WIN95FLOPPY% not found. Build it with:
    echo         py -3 tools\make_win95_cd_floppy.py
    pause
    exit /b 1
)
if not exist "%WIN95HDD%" (
    echo [ERROR] %WIN95HDD% not found. Build it with:
    echo         py -3 tools\make_win95_hdd.py
    pause
    exit /b 1
)
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy "%WIN95FLOPPY%" --cdrom "%WIN95ISO%" --hdd "%WIN95HDD%"
goto :eof

:headless
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --headless --seconds 40 --dump-text
goto :eof

:headless_shot
"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --headless --seconds 45 --press-del --screenshot setup.ppm
if exist setup.ppm echo Screenshot saved: setup.ppm  ^(convert with ffmpeg -i setup.ppm setup.png^)
goto :eof
