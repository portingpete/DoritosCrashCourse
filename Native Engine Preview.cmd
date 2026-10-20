@echo off
setlocal
cd /d "%~dp0"
if not exist "build_native\engine\engine\Release\DoritosNativeEngine.exe" (
    echo Run powershell -ExecutionPolicy Bypass -File tools\build_engine.ps1 first.
    exit /b 1
)
build_native\engine\engine\Release\DoritosNativeEngine.exe --assets "%~dp0build_native\engine_inputs" %*
exit /b %errorlevel%
