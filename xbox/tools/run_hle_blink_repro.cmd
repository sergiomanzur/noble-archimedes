@echo off
rem Interactive HLE blinking reproduction launcher.
rem Targets the rebuilt xbox/build/Release executable, not the older root copy.
rem Observe only the SDL window titled "Castlevania CoD | D3D8GL".
rem The software framebuffer window is intentionally not enabled.
setlocal
cd /d "%~dp0..\.."
set OUT=output\blink_repro
if not exist "%OUT%" mkdir "%OUT%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$exe=(Resolve-Path 'xbox\build\Release\curse_of_darkness_xbox_recomp.exe').Path; $dll=(Resolve-Path 'd3d8gl.dll').Path; " ^
  "$lines=@('executable='+$exe,'exe_sha256='+(Get-FileHash $exe -Algorithm SHA256).Hash,'dll='+$dll,'dll_sha256='+(Get-FileHash $dll -Algorithm SHA256).Hash,'window=Castlevania CoD ^| D3D8GL','backend=RECOMP_D3D8GL=1'); " ^
  "$lines | Set-Content -Encoding ascii '%OUT%\identity.txt'"
set RECOMP_D3D8GL=1
set RECOMP_PAD_SCRIPT=@xbox\tools\blink_repro_pad.txt
set RECOMP_PACE_STATS=1
set RECOMP_GPU_STATS=1
echo.
echo HLE blink reproduction: observe the SDL window "Castlevania CoD | D3D8GL".
echo Enter gameplay, hold movement around the courtyard, and note the time of any black flash.
echo The launcher keeps HLE diagnostic logs in the repository root.
echo.
"xbox\build\Release\curse_of_darkness_xbox_recomp.exe"
if exist xbox_stderr.log copy /y xbox_stderr.log "%OUT%\xbox_stderr.log" >nul
if exist xbox_stdout.log copy /y xbox_stdout.log "%OUT%\xbox_stdout.log" >nul
echo.
echo Logs copied to %OUT%. Identity:
type "%OUT%\identity.txt"
endlocal
