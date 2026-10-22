@echo off
rem Ninja Gaiden Black (native loader): play with the user's save, newest build (build\ngb2).
rem Log and screenshots go to run\play. Alt+Enter toggles fullscreen.
cd /d "%~dp0"
if not exist "run\play\shots" mkdir "run\play\shots"
type nul > "run\play\presses.txt"
start "" "build\ngb2\RelWithDebInfo\Ninja Gaiden Black Hurricane.exe" --windowed --hdd run\r002_d3d\hdd --log run\play\black.log --symbols work\functions_x21.csv --screenshot run\play\shots --shot-every 100000000 --press-file run\play\presses.txt game\x21\default.xbe
