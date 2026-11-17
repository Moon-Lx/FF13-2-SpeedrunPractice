@echo off
rem FF13-2-SpeedrunPractice launcher -- practice tools OFF, for measured runs.
rem Parks dinput8.dll as dinput8.dll.off (so the mod cannot load) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

tasklist /fi "imagename eq ffxiii2img.exe" 2>nul | find /i "ffxiii2img.exe" >nul && goto running

if not exist "dinput8.dll" goto saves
rem Only the mod's own dll is parked: another dinput8.dll (the FPS Fix) stops everything here.
findstr /m /l /c:"ff13-2-srpractice" "dinput8.dll" >nul || goto foreign
if exist "dinput8.dll.off" goto both
ren "dinput8.dll" "dinput8.dll.off"
if errorlevel 1 goto locked

:saves
rem Everything here is put back by the mod itself on its next start (through ff13-2-practice.bat
rem or straight from Steam). Same-folder renames keep the file times the mod's records match on.
set "SAVEDIR=%LOCALAPPDATA%\SquareEnix\FinalFantasyXIII-2\save"
set "MODS=ff13-2-mods"
set "LENT=%MODS%\ff13-2-srpractice.lent"
if exist "%LENT%" goto lentagain

rem The category picked on the mod's settings screen carries this marker in its folder. Only the
rem mod gives lent saves back, so without its dll here nothing is lent.
set "LEND="
if not exist "dinput8.dll.off" goto parkhigh
findstr /m /l /c:"ff13-2-srpractice" "dinput8.dll.off" >nul || goto parkhigh
for /d %%d in ("%SAVEDIR%\categories\*") do if not defined LEND if exist "%%d\ff13-2-srpractice.nomod" set "LEND=%%d"
if not defined LEND goto parkhigh
rem A category switch or renumber the mod has not finished owns the files.
if exist "%MODS%\ff13-2-srpractice.categories.jnl" goto unfinished
if exist "%MODS%\ff13-2-srpractice.renum.jnl" goto unfinished

rem Every save in use is hidden, then the picked category's saves 1-40 take the folder. The lent
rem mark is written only once nothing in use is left showing: from then on, the mod takes every
rem save in the folder for the lent category's.
for %%a in (0 1 2 3 4 5 6 7 8 9) do for %%b in (0 1 2 3 4 5 6 7 8 9) do if exist "%SAVEDIR%\ff13_2-%%a%%b.dat" (
    if exist "%SAVEDIR%\ff13_2-%%a%%b.dat.parked" goto clash
    ren "%SAVEDIR%\ff13_2-%%a%%b.dat" "ff13_2-%%a%%b.dat.parked"
)
for %%a in (0 1 2 3 4 5 6 7 8 9) do for %%b in (0 1 2 3 4 5 6 7 8 9) do if exist "%SAVEDIR%\ff13_2-%%a%%b.dat" goto hidefail
if not exist "%MODS%\" mkdir "%MODS%"
> "%LENT%" echo Saves lent to the game without the mod by ff13-2-run.bat. The mod puts them back.
if not exist "%LENT%" goto markfail
rem From inside the category's folder: its name may hold %a or %b, which the loop would expand.
pushd "%LEND%" || goto lendfail
set "MOVEFAIL="
for %%a in (0 1 2 3) do for %%b in (0 1 2 3 4 5 6 7 8 9) do if exist "ff13_2-%%a%%b.dat" (
    if not exist "%SAVEDIR%\ff13_2-%%a%%b.dat" move "ff13_2-%%a%%b.dat" "%SAVEDIR%\ff13_2-%%a%%b.dat" >nul
    if not exist "%SAVEDIR%\ff13_2-%%a%%b.dat" set "MOVEFAIL=1"
)
popd
if defined MOVEFAIL goto lendfail
echo Saves: the category picked on the mod's settings screen, until the game starts with the mod
goto launch

:lentagain
echo Saves: still the category lent before, until the game starts with the mod
goto launch

:parkhigh
rem Saves past the game's 40 slots (ff13_2-40.dat = save 41 and up) close the unmodded game the
rem moment its load list opens, so they are hidden as .parked.
for /l %%n in (40,1,98) do if exist "%SAVEDIR%\ff13_2-%%n.dat" (
    if exist "%SAVEDIR%\ff13_2-%%n.dat.parked" goto clash
    ren "%SAVEDIR%\ff13_2-%%n.dat" "ff13_2-%%n.dat.parked"
    if exist "%SAVEDIR%\ff13_2-%%n.dat" goto parkfail
)
if exist "%SAVEDIR%\ff13_2-??.dat.parked" echo Saves numbered 41 and up: hidden as .parked until the game starts with the mod

:launch
echo Practice tools: OFF  (no dinput8.dll in the game folder)
start "" "steam://rungameid/292140"
timeout /t 3 >nul
exit /b 0

:running
echo [!] FINAL FANTASY XIII-2 is running. Close it, then run this again.
pause
exit /b 1

:foreign
echo [!] The dinput8.dll here is not this mod's (another mod, such as the FPS Fix?).
echo     Nothing was renamed or hidden, and the game was not started.
pause
exit /b 1

:both
echo [!] Both dinput8.dll and dinput8.dll.off are here. Nothing was renamed.
echo     Keep only this mod's dll (as dinput8.dll), then run this again.
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

:unfinished
echo [!] The mod has a category switch or renumber to finish. Start the game once with
echo     ff13-2-practice.bat, then run this again. Nothing was hidden.
pause
exit /b 1

:hidefail
echo [!] Could not hide every save in:
echo     %SAVEDIR%
echo     The game was not started. ff13-2-practice.bat puts the hidden ones back.
pause
exit /b 1

:markfail
echo [!] Could not write %LENT%
echo     The game was not started. ff13-2-practice.bat puts the hidden saves back.
pause
exit /b 1

:lendfail
echo [!] Could not move every save of the picked category into:
echo     %SAVEDIR%
echo     The game was not started. ff13-2-practice.bat puts everything back.
pause
exit /b 1
