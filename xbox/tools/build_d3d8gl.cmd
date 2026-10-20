@echo off
setlocal
cd /d "%~dp0..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set COMMON=/nologo /c /O2 /Zi /Fdoutput\d3d8gl-objects.pdb /std:c11 /D_M_IX86=1 /D_USE_MATH_DEFINES /DDEBUG_KEYBOARD /Ixbox\src\d3d8gl /Ioutput\OpenCE\port\windows\include /Ioutput\OpenCE\port\windows\include\posix /Ioutput\OpenCE\port\windows\include\crt /Ioutput\OpenCE\port\include\xdk /Ioutput\sdl3\SDL3-3.4.18\include /Ioutput\sdl3\SDL3-3.4.18\include\SDL3 /FIhalo_windows_prefix.h
set OBJECTS=
for %%f in (d3d8_gl d3d8_resources nv2a_vsh nv2a_psh xbox_textures xgpu_text gl_functions stubs hires_stubs exports) do (
  clang-cl %COMMON% /Foxbox\src\d3d8gl\%%f.obj xbox\src\d3d8gl\%%f.c
  if errorlevel 1 exit /b 1
)
clang-cl /nologo /c /std:c11 /Ioutput\OpenCE\port\windows\include /Ioutput\OpenCE\port\windows\include\posix /Ioutput\OpenCE\port\windows\include\crt /Foxbox\src\d3d8gl\win32_posix.obj output\OpenCE\port\windows\src\win32_posix.c
if errorlevel 1 exit /b 1
set OBJECTS=xbox\src\d3d8gl\d3d8_gl.obj xbox\src\d3d8gl\d3d8_resources.obj xbox\src\d3d8gl\nv2a_vsh.obj xbox\src\d3d8gl\nv2a_psh.obj xbox\src\d3d8gl\xbox_textures.obj xbox\src\d3d8gl\xgpu_text.obj xbox\src\d3d8gl\gl_functions.obj xbox\src\d3d8gl\stubs.obj xbox\src\d3d8gl\hires_stubs.obj xbox\src\d3d8gl\exports.obj xbox\src\d3d8gl\win32_posix.obj
set LIBS=output\sdl3\SDL3-3.4.18\lib\x64\SDL3.lib opengl32.lib user32.lib gdi32.lib winmm.lib
link /nologo /DEBUG /INCREMENTAL:NO /MAP:output\d3d8gl.map /PDB:output\d3d8gl.pdb /DLL /OUT:output\d3d8gl.dll %OBJECTS% %LIBS%
if errorlevel 1 exit /b 1
if /i "%~1"=="test" (
  clang-cl %COMMON% /Foxbox\src\d3d8gl\d3d8gl_abi_test.obj xbox\tests\d3d8gl_abi_test.c
  if errorlevel 1 exit /b 1
  link /nologo /SUBSYSTEM:CONSOLE /OUT:output\d3d8gl_abi_test.exe xbox\src\d3d8gl\d3d8gl_abi_test.obj %OBJECTS% %LIBS%
  if errorlevel 1 exit /b 1
  output\d3d8gl_abi_test.exe
  if errorlevel 1 exit /b 1
)
if /i "%~1"=="gpu-test" (
  clang-cl %COMMON% /Foxbox\src\d3d8gl\d3d8gl_draw_test.obj xbox\tests\d3d8gl_draw_test.c
  if errorlevel 1 exit /b 1
  link /nologo /DEBUG /SUBSYSTEM:CONSOLE /OUT:output\d3d8gl_draw_test.exe xbox\src\d3d8gl\d3d8gl_draw_test.obj %OBJECTS% %LIBS%
  if errorlevel 1 exit /b 1
  output\d3d8gl_draw_test.exe
  if errorlevel 1 exit /b 1
)
copy /y output\d3d8gl.dll d3d8gl.dll >nul
if errorlevel 1 exit /b 1
exit /b 0
