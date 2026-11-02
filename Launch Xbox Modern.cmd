@echo off
setlocal
cd /d "%~dp0"
if not exist "xbox\build\Release\curse_of_darkness_xbox_recomp.exe" (
  echo Build the Xbox host first. See xbox\README.md.
  exit /b 1
)
set "RECOMP_PAD_SCRIPT="
set "RECOMP_PAD_LIVE="
set "RECOMP_MODERN_INPUT=1"
set "RECOMP_USB=1"
set "RECOMP_VBLANK=1"
set "RECOMP_PB_EXEC=1"
set "RECOMP_PB_WALK=dma"
echo Press F2 for Controls. Renderer selection follows the existing configuration.
"xbox\build\Release\curse_of_darkness_xbox_recomp.exe"
