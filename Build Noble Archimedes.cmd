@echo off
setlocal
cd /d "%~dp0"
call "Build Xbox D3D12.cmd" --full-game %*
exit /b %errorlevel%
