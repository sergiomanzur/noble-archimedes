@echo off
setlocal
cd /d "%~dp0"
if not exist "xbox\build\Release\curse_of_darkness_xbox_recomp.exe" (
  echo Build the Xbox host first. See xbox\README.md.
  exit /b 1
)
if not exist "Widescreen Settings.cmd" (
  echo Missing Widescreen Settings.cmd.
  exit /b 1
)
call "%~dp0Widescreen Settings.cmd"
set "RECOMP_PAD_SCRIPT="
set "RECOMP_PAD_LIVE="
set "RECOMP_MODERN_INPUT=1"
set "RECOMP_USB=1"
set "RECOMP_VBLANK=1"
set "RECOMP_PB_EXEC=1"
set "RECOMP_PB_WALK=dma"
set "RECOMP_D3D8GL=1"
set "RECOMP_WIDE_EXPERIMENT=1"
set "RECOMP_WIDE_GUEST_PROJECTION=1"
echo Experimental widescreen. Press F2 for controls.
"xbox\build\Release\curse_of_darkness_xbox_recomp.exe"
