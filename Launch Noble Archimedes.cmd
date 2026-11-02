@echo off
setlocal
cd /d "%~dp0output\jmafort-port\out\build\release"
if errorlevel 1 exit /b 1
if /i "%~1"=="eu" (
    cd eu-play
    if errorlevel 1 exit /b 1
)
if not exist NobleArchimedesLauncher.exe (
    echo Run Build Noble Archimedes.cmd first.
    exit /b 1
)
start "" NobleArchimedesLauncher.exe
