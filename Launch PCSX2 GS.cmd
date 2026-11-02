@echo off
setlocal
cd /d "%~dp0"
if not exist "NobleArchimedes.exe" goto :missing_exe
if not exist "noble_pcsx2_gs.dll" goto :missing_adapter
if not exist "Castlevania - Curse of Darkness (USA).iso" goto :missing_game
if not exist "extracted\SLUS_211.68" goto :missing_game
set "PS2_GS_BACKEND=pcsx2"
set "PS2_NATIVE_VIF1_WORKER=1"
"NobleArchimedes.exe"
if errorlevel 1 pause
exit /b %errorlevel%

:missing_exe
echo NobleArchimedes.exe is not included in the public source kit.
echo Build it locally from your own game by following README-WINDOWS.txt.
pause
exit /b 2

:missing_adapter
echo noble_pcsx2_gs.dll and its licensed runtime bundle are missing.
echo Follow the Hybrid renderer section in README-WINDOWS.txt.
pause
exit /b 3

:missing_game
echo The supported SLUS-21168 ISO or extracted boot executable is missing.
echo Follow the Game selection section in README-WINDOWS.txt.
pause
exit /b 4
