@echo off
setlocal
cd /d "%~dp0..\.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ build tools were not found.
  exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b 1
clang++ -std=c++20 -fuse-ld=lld -Ioutput/jmafort-sdk/runtime xbox/tests/frame_copy_shader_test.cpp output/jmafort-sdk/runtime/gpu/shaders.cpp -ld3dcompiler -o output/frame-copy-shader-test.exe
if errorlevel 1 exit /b 1
output\frame-copy-shader-test.exe
