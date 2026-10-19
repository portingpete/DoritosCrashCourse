@echo off
setlocal
cd /d "%~dp0"
py -3 tools\character_importer_ui.py
if errorlevel 1 pause
