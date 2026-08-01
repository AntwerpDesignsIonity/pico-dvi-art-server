@echo off
setlocal EnableDelayedExpansion
title Pico DVI Art Server - Antwerp Ionity
cd /d "%~dp0"

REM ===================================================================
REM  START.bat - the ONLY file you run. No questions, no options.
REM
REM  It finds Python, installs what it needs, provisions any Pico that
REM  is (or later gets) plugged in, and streams art forever. If the
REM  server ever dies it restarts itself.
REM
REM  Stop it by closing the window.
REM ===================================================================

echo.
echo   ==================================================================
echo      PICO DVI ART SERVER - starting up
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
echo   [1/4] Python  : %PYEXE% %PYARG%

REM ---- 2. dependencies ------------------------------------------------
"%PYEXE%" %PYARG% -c "import numpy, serial, mpremote" >nul 2>&1
if errorlevel 1 (
    echo   [2/4] Installing dependencies, this happens once...
    "%PYEXE%" %PYARG% -m pip install -q --disable-pip-version-check -r pc_server\requirements.txt mpremote pyserial
    if errorlevel 1 (
        echo   [X] Dependency install failed. Check your internet connection.
        pause
        exit /b 1
    )
) else (
    echo   [2/4] Dependencies: ok
)

REM ---- 3. background provisioner ---------------------------------------
REM  Rescans USB every 30s. Flashes MicroPython + our firmware onto any
REM  blank/BOOTSEL Pico automatically. Never touches foreign firmware.
echo   [3/4] Starting the USB auto-provisioner
start "Pico auto-provisioner" /min "%PYEXE%" %PYARG% tools\flash_pico.py --auto --watch 30

REM ---- 4. the art server, with auto-restart ----------------------------
echo   [4/4] Starting the art stream
echo.
echo   ------------------------------------------------------------------
echo.

:RUN
"%PYEXE%" %PYARG% pc_server\server.py
echo.
echo   [!] The server stopped. Restarting in 5 seconds...
echo       (close this window to quit)
timeout /t 5 /nobreak >nul
goto RUN
