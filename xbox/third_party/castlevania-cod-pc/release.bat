@echo off
setlocal
rem A release a player can download: Castlevania.exe, CastlevaniaEU.exe and the launcher, without the game code. The
rem exe of the player's release (Castlevania.exe starts CastlevaniaEU.exe for a European copy) builds its DLL on its first
rem start from the player's own default.xbe with the compiler folder (the SDK's runtime\app\game_code.cpp): the
rem recompiler, Clang and LLD, the game's configs and the headers the code includes.
rem   release.bat [version]      writes out\release\CastlevaniaCOD-PC-<version>.zip, by default CMakeLists.txt's version
rem Needs what build.bat needs, and both releases' default.xbe: the USA one in assets, the European one in assets\eu (the
rem build reads them for their SHA-1s, nothing of them is packed).
cd /d "%~dp0"
set "VERSION=%~1"
set "OUT=out\build\package"
set "PKG=out\release\CastlevaniaCOD-PC"
if not exist assets\default.xbe (echo   Put the game's default.xbe in assets first, see README.md. & goto :fail)
if not exist assets\eu\default.xbe (echo   Put the European release's default.xbe in assets\eu first, see README.md. & goto :fail)

echo [1/4] Toolchain
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo       Visual Studio with the C++ workload was not found, see build.bat. & goto :fail)
rem its vswhere.exe on PATH, as in build.bat
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
rem the Visual C++ runtime beside the executables, so a fresh Windows needs nothing more
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRT=%%c"
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do if not exist "%VCRT%\%%f.dll" (echo       missing the Visual C++ runtime's %%f.dll & goto :fail)

echo [2/4] Both releases' exes, the launcher and the compiler folder (no game code: no DLL is built)
cmake --preset package > release_build_log.txt 2>&1 || (echo       configuring failed, see release_build_log.txt & goto :fail)
cmake --build %OUT% --target Castlevania CastlevaniaEU CastlevaniaLauncher >> release_build_log.txt 2>&1 || (echo       the build failed, see release_build_log.txt & goto :fail)
set /p SDK=<%OUT%\sdk_dir.txt
rem no version given: the one in CMakeLists.txt, which the exes carry
if not defined VERSION set /p VERSION=<%OUT%\version.txt

echo [3/4] Package
if exist out\release rd /s /q out\release
mkdir "%PKG%\assets" "%PKG%\textures"
for %%f in (Castlevania.exe CastlevaniaEU.exe CastlevaniaLauncher.exe) do copy /y "%OUT%\%%f" "%PKG%\" >nul
rem the compiler folder without its work folder, where the recompiled code is made
robocopy "%OUT%\compiler" "%PKG%\compiler" /e /xd work /njh /njs /ndl /nfl /np >nul
if errorlevel 8 (echo       copying the compiler folder failed & goto :fail)
rem xbe_recomp.exe needs its own copy: Windows looks for a DLL beside the exe it starts
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do (
    copy /y "%VCRT%\%%f.dll" "%PKG%\" >nul
    copy /y "%VCRT%\%%f.dll" "%PKG%\compiler\" >nul
)
copy /y tools\release\README.txt "%PKG%\README.txt" >nul
copy /y LICENSE "%PKG%\LICENSE.txt" >nul
copy /y "%SDK%\THIRD_PARTY.txt" "%PKG%\THIRD_PARTY.txt" >nul
echo Put your game here: its disc image (.iso), or the extracted disc (default.xbe with its folders).> "%PKG%\assets\PUT YOUR GAME HERE.txt"
echo Optional: an HD texture pack made for PCSX2. Put its replacement pictures here (.png or .dds files, folders inside are fine).> "%PKG%\textures\PUT A TEXTURE PACK HERE.txt"

rem nothing of the game goes out: no game code or its sources, no xbe or disc image, no icon made from its pictures
rem (for /r names a file without a wildcard in every folder, there or not)
for /r "%PKG%" %%f in (*.xbe *.iso *.cpp *.obj *.ico x86_recomp* image_symbols.h Castlevania.dll CastlevaniaEU.dll) do if exist "%%f" (echo       game file in the package: %%f & goto :fail)
if exist "%PKG%\compiler\work" (echo       the compiler's work folder is in the package & goto :fail)

echo [4/4] Zip
rem Windows' own tar (bsdtar) makes a zip; a GNU tar first on PATH (Git's usr\bin) would make a tar named .zip
"%SystemRoot%\System32\tar.exe" -a -cf "out\release\CastlevaniaCOD-PC-%VERSION%.zip" -C out\release CastlevaniaCOD-PC || goto :fail
for %%z in ("out\release\CastlevaniaCOD-PC-%VERSION%.zip") do echo       %%~fz  (%%~zz bytes)
exit /b 0

:fail
echo.
echo   RELEASE FAILED
exit /b 1
