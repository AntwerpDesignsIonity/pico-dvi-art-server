@echo off
setlocal EnableDelayedExpansion
title Pico DVI Firmware Studio - Antwerp Ionity
cd /d "%~dp0"

REM ===================================================================
REM  START.bat - source launcher for the Pico-only firmware workspace.
REM
REM  It finds Python, installs the small desktop dependency set the first
REM  time, and opens the native firmware build/flash tool.
REM ===================================================================

echo.
echo   ==================================================================
echo      PICO DVI FIRMWARE STUDIO - starting up
echo   ==================================================================
echo.

REM ---- 1. locate Python ----------------------------------------------
set "VCHECK=import sys;sys.exit(0 if sys.version_info>=(3,9) else 1)"
set "PYEXE="
set "PYARG="

py -3 -c "%VCHECK%" >nul 2>&1
if !errorlevel! equ 0 (
    set "PYEXE=py"
    set "PYARG=-3"
)

if not defined PYEXE (
    python -c "%VCHECK%" >nul 2>&1
    if !errorlevel! equ 0 set "PYEXE=python"
)

if not defined PYEXE (
    for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do (
        if not defined PYEXE if exist "%%~fD\python.exe" (
            "%%~fD\python.exe" -c "%VCHECK%" >nul 2>&1
            if !errorlevel! equ 0 set "PYEXE=%%~fD\python.exe"
        )
    )
)

if not defined PYEXE (
    echo   [X] Python 3.9 or newer is required but was not found.
    echo       Get it from https://python.org and tick "Add python.exe to PATH".
    echo.
    pause
    exit /b 1
)
echo   [1/3] Python  : %PYEXE% %PYARG%

REM ---- 2. dependencies -----------------------------------------------
"%PYEXE%" %PYARG% -c "import serial" >nul 2>&1
if errorlevel 1 (
    echo   [2/3] Installing dependencies, this happens once...
    "%PYEXE%" %PYARG% -m pip install -q --disable-pip-version-check pyserial
    if errorlevel 1 (
        echo   [X] Dependency install failed. Check your internet connection.
        pause
        exit /b 1
    )
) else (
    echo   [2/3] Dependencies: ok
)

REM ---- 3. launch the desktop app -------------------------------------
echo   [3/3] Opening Pico DVI Firmware Studio
echo.

:RUN
"%PYEXE%" %PYARG% app\studio.py
if errorlevel 1 (
    echo.
    echo   [!] The app closed unexpectedly. Restarting in 5 seconds...
    echo       ^(close this window to quit^)
    timeout /t 5 /nobreak >nul
    goto RUN
)
