@echo off
rem FF13-2-SpeedrunPractice launcher -- practice tools ON.
rem Restores dinput8.dll (from dinput8.dll.off if it was parked) and starts
rem the game through Steam. Idempotent: declares a state, does not toggle.
rem ASCII only on purpose: cmd.exe parses this file in the OEM codepage.
setlocal
cd /d "%~dp0"

tasklist /fi "imagename eq ffxiii2img.exe" 2>nul | find /i "ffxiii2img.exe" >nul && goto running

if exist "dinput8.dll" goto unpark
if not exist "dinput8.dll.off" goto missing
ren "dinput8.dll.off" "dinput8.dll"
if errorlevel 1 goto locked

:unpark
rem Saves 41 and up that ff13-2-run.bat hid. One whose name is taken again stays hidden.
set "SAVEDIR=%LOCALAPPDATA%\SquareEnix\FinalFantasyXIII-2\save"
set "CLASH="
for /l %%n in (40,1,98) do if exist "%SAVEDIR%\ff13_2-%%n.dat.parked" (
    if exist "%SAVEDIR%\ff13_2-%%n.dat" (
        echo [!] ff13_2-%%n.dat already exists -- ff13_2-%%n.dat.parked was left as it is.
        set "CLASH=1"
    ) else (
        ren "%SAVEDIR%\ff13_2-%%n.dat.parked" "ff13_2-%%n.dat"
    )
)
if defined CLASH pause

:launch
echo Practice tools: ON   (dinput8.dll in place)
start "" "steam://rungameid/292140"
timeout /t 3 >nul
exit /b 0

:running
echo [!] FINAL FANTASY XIII-2 is running. Close it, then run this again.
pause
exit /b 1

:missing
echo [!] dinput8.dll not found here (neither active nor parked as dinput8.dll.off).
echo     Put the mod's dinput8.dll next to this file first.
pause
exit /b 1

:locked
echo [!] Could not restore dinput8.dll -- is the game still running?
echo     Close FINAL FANTASY XIII-2, then run this again.
pause
exit /b 1
