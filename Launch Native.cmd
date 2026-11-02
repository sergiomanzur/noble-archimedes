@echo off
setlocal
cd /d "%~dp0"
if not exist "NobleArchimedes.exe" goto :missing_exe
if not exist "Castlevania - Curse of Darkness (USA).iso" goto :missing_game
if not exist "extracted\SLUS_211.68" goto :missing_game
set "PS2_GS_BACKEND=pgs"
set "PS2_NATIVE_VIF1_WORKER=1"
set "PS2_VU_BACKEND=play"
set "NOBLE_VU_DEAD_FLAG_ELIMINATION=1"
set "PGS_SKIP_COMPILATION_TASKS=1"
set "PS2_PAD_TRACE=0"
set "PS2_MPEG_TRACE=0"
set "PS2_HOST_PROFILE=0"
"NobleArchimedes.exe"
if errorlevel 1 pause
exit /b %errorlevel%

:missing_exe
echo NobleArchimedes.exe is not included in the public source kit.
echo Build it locally from your own game by following README-WINDOWS.txt.
pause
exit /b 2

:missing_game
echo The supported SLUS-21168 ISO or extracted boot executable is missing.
echo Follow the Game selection section in README-WINDOWS.txt.
pause
exit /b 3
