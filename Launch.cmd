@echo off
setlocal
cd /d "%~dp0"
if not exist "build_native\native\Release\DoritosNative.exe" (
    echo Native executable has not been built. Run tools\build.ps1 first.
    exit /b 1
)
py -3 tools\character_importer.py launch -- %*
exit /b %errorlevel%
