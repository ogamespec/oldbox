@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-vs2026"

rem Locate CMake in this order:
rem   1) the copy bundled with Visual Studio 2026 (vswhere discovery)
rem   2) known Visual Studio 18 install directories
rem   3) whatever "cmake" is on PATH
set "CMAKE_EXE="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    set "VSINSTALL="
    "%VSWHERE%" -latest -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\oldbox_vs.txt" 2>nul
    if exist "%TEMP%\oldbox_vs.txt" set /p VSINSTALL=<"%TEMP%\oldbox_vs.txt"
    del "%TEMP%\oldbox_vs.txt" >nul 2>nul
    if defined VSINSTALL (
        if exist "!VSINSTALL!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=!VSINSTALL!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)

if not defined CMAKE_EXE (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)

if not defined CMAKE_EXE (
    for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
)

if not defined CMAKE_EXE (
    echo [error] CMake was not found.
    echo         Install Visual Studio 2026 with the "Desktop development with C++"
    echo         workload, or put cmake.exe on PATH.
    exit /b 1
)

set "CTEST_EXE="
for %%i in ("%CMAKE_EXE%") do if exist "%%~dpictest.exe" set "CTEST_EXE=%%~dpictest.exe"
if not defined CTEST_EXE set "CTEST_EXE=ctest"

echo Using CMake: %CMAKE_EXE%
"%CMAKE_EXE%" --version
if errorlevel 1 goto :fail

echo.
echo [1/3] Configure: %PRESET%
"%CMAKE_EXE%" --preset "%PRESET%"
if errorlevel 1 goto :fail

echo.
echo [2/3] Build: %PRESET%-release
"%CMAKE_EXE%" --build --preset "%PRESET%-release"
if errorlevel 1 goto :fail

echo.
echo [3/3] Build and run tests
"%CMAKE_EXE%" --build --preset "%PRESET%-release" --target oldbox_tests
if errorlevel 1 goto :fail
"%CTEST_EXE%" --preset "%PRESET%-release" --output-on-failure
if errorlevel 1 goto :fail

echo.
echo Build finished successfully.
echo Run: build\vs2026\Release\oldbox.exe --bios assets\bios\EP-ZXA.bin
exit /b 0

:fail
echo.
echo Build failed.
exit /b 1
