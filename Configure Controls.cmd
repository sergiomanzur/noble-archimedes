@echo off
setlocal
cd /d "%~dp0"
if not exist "xbox\build\Release\NobleControls.exe" (
  echo Build NobleControls first. See xbox\README.md.
  exit /b 1
)
start "" "xbox\build\Release\NobleControls.exe"
