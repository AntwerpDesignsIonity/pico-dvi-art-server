@echo off
setlocal EnableDelayedExpansion
title Pico DVI Art Studio - Antwerp Ionity
cd /d "%~dp0"

REM ===================================================================
REM  START.bat - the ONLY file you run. No questions, no options.
REM
REM  It finds Python, installs what it needs, provisions any Pico that
REM  is (or later gets) plugged in, and opens preview/device controls.
REM  Art renders on the Pico and continues when this app is closed.
REM
REM  Stop it by closing the window.
REM ===================================================================

echo.
echo   ==================================================================
echo      PICO DVI ART STUDIO - starting up
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

REM ---- 2. dependencies ------------------------------------------------
"%PYEXE%" %PYARG% -c "import numpy, serial, mpremote" >nul 2>&1
if errorlevel 1 (
    echo   [2/3] Installing dependencies, this happens once...
    "%PYEXE%" %PYARG% -m pip install -q --disable-pip-version-check -r pc_server\requirements.txt mpremote pyserial
    if errorlevel 1 (
        echo   [X] Dependency install failed. Check your internet connection.
        pause
        exit /b 1
    )
) else (
    echo   [2/3] Dependencies: ok
)

REM ---- 3. launch the desktop app ---------------------------------------
REM  app\studio.py is a native Tkinter application. It runs the control server
REM  in-process, exposes preview settings, shows a desktop preview and can
REM  build/flash/OTA the Pico - so there is nothing else to start and no
REM  port or path for anyone to type in.
echo   [3/3] Opening Pico DVI Art Studio
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
