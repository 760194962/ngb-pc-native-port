@echo off
rem Ninja Gaiden Black (native loader): play with a copy of the user's save (run\play\hdd_user), newest build (build\ngb2).
rem Extra options pass through, e.g. play.bat --audio ac3 (stereo, mono, surround, ac3, dts). Log and screenshots go to run\play. Alt+Enter toggles fullscreen.
cd /d "%~dp0"
if not exist "run\play\shots" mkdir "run\play\shots"
type nul > "run\play\presses.txt"
start "" "build\ngb2\RelWithDebInfo\Ninja Gaiden Black Any.exe" --hdd run\play\hdd_user --log run\play\black.log --symbols work\functions_x21.csv --screenshot run\play\shots --shot-every 100000000 --press-file run\play\presses.txt %* game\x21\default.xbe
