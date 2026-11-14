@echo off
rem FF13-2-SpeedrunPractice launcher -- practice tools OFF, for measured runs.
rem Parks dinput8.dll as dinput8.dll.off (so the mod cannot load) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

tasklist /fi "imagename eq ffxiii2img.exe" 2>nul | find /i "ffxiii2img.exe" >nul && goto running

if not exist "dinput8.dll" goto parksaves
ren "dinput8.dll" "dinput8.dll.off"
if errorlevel 1 goto locked

:parksaves
rem Saves past the game's 40 slots (ff13_2-40.dat = save 41 and up) close the unmodded game the
rem moment its load list opens, so they are hidden as .parked; ff13-2-practice.bat and the mod
rem itself put them back. A same-folder rename keeps the file time the mod's records match on.
set "SAVEDIR=%LOCALAPPDATA%\SquareEnix\FinalFantasyXIII-2\save"
for /l %%n in (40,1,98) do if exist "%SAVEDIR%\ff13_2-%%n.dat" (
    if exist "%SAVEDIR%\ff13_2-%%n.dat.parked" goto clash
    ren "%SAVEDIR%\ff13_2-%%n.dat" "ff13_2-%%n.dat.parked"
    if exist "%SAVEDIR%\ff13_2-%%n.dat" goto parkfail
)
if exist "%SAVEDIR%\ff13_2-??.dat.parked" echo Saves numbered 41 and up: hidden as .parked until ff13-2-practice.bat

:launch
echo Practice tools: OFF  (no dinput8.dll in the game folder)
start "" "steam://rungameid/292140"
timeout /t 3 >nul
exit /b 0

:running
echo [!] FINAL FANTASY XIII-2 is running. Close it, then run this again.
pause
exit /b 1

:locked
echo [!] Could not park dinput8.dll -- is the game still running?
echo     Close FINAL FANTASY XIII-2, then run this again.
pause
exit /b 1

:clash
echo [!] Both a save and its .parked copy exist in:
echo     %SAVEDIR%
echo     Nothing was overwritten. Keep one of the two, then run this again.
pause
exit /b 1

:parkfail
echo [!] Could not hide a save numbered 41 or higher in:
echo     %SAVEDIR%
echo     The game was not started: without the mod it would close at the load list.
pause
exit /b 1
