@echo off
rem Ninja Gaiden Black x2.1 native port: start the game (see README.md / README.en.md, "Running").
rem Paths are relative to this file's folder. To use other locations, set EXE / GAME / HDD / LOGDIR before
rem calling this file, or drop a default.xbe onto it.
setlocal
cd /d "%~dp0"
if not defined EXE set "EXE=build\RelWithDebInfo\Ninja Gaiden Black Any.exe"
if not defined GAME set "GAME=game\x21\default.xbe"
if not defined HDD set "HDD=savedata"
if not defined LOGDIR set "LOGDIR=logs"
if not "%~1"=="" set "GAME=%~1"

if not exist "%EXE%" (
    echo Program not found: %EXE%
    echo Build it first ^(README: Building^), or set EXE to its path.
    pause
    exit /b 1
)
if not exist "%GAME%" (
    echo Game not found: %GAME%
    echo Extract the x2.1 image to game\x21, drop default.xbe onto run.bat, or set GAME.
    pause
    exit /b 1
)
if not exist "%HDD%" mkdir "%HDD%"
if not exist "%LOGDIR%\shots" mkdir "%LOGDIR%\shots"
type nul > "%LOGDIR%\presses.txt"

rem --shot-every 100000000: screenshots only on request (shot:NAME in presses.txt); the default of one a second stutters
start "" "%EXE%" --windowed --hdd "%HDD%" --log "%LOGDIR%\black.log" --screenshot "%LOGDIR%\shots" --shot-every 100000000 --press-file "%LOGDIR%\presses.txt" "%GAME%"
