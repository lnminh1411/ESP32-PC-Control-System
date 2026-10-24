@echo off
rem idf.py inside the ESP-IDF v5.5.6 environment (C:\Espressif), run from this folder.
rem   idf build            idf -p COM12 flash monitor
setlocal
set IDF_TOOLS_PATH=C:\Espressif
set MSYSTEM=
call C:\Espressif\frameworks\esp-idf-v5.5.6\export.bat >"%TEMP%\idf-export.log" 2>&1 || exit /b 1
cd /d "%~dp0"
idf.py %*
