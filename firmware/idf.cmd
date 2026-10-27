@echo off
rem idf.py inside the ESP-IDF v5.5.6 environment (C:\Espressif), run from this folder.
rem   idf build            idf -p COM12 flash monitor
rem export.bat takes ~3.5 s, so what it sets is cached in %LOCALAPPDATA%\esp-idf-env\
rem (idf_env_cache.py). Delete that folder after changing the ESP-IDF install.
setlocal
set IDF_TOOLS_PATH=C:\Espressif
set MSYSTEM=
set "IDF_ENV_CACHE=%LOCALAPPDATA%\esp-idf-env\v5.5.6.cmd"
set "IDF_OLD_PATH=%PATH%"
if not exist "%IDF_ENV_CACHE%" goto export
call "%IDF_ENV_CACHE%"
if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" if exist "%IDF_PATH%\tools\idf.py" goto run
rem stale cache (ESP-IDF moved or reinstalled): undo it and export again
set "PATH=%IDF_OLD_PATH%"

:export
set "IDF_ENV_BEFORE=%TEMP%\idf-env-before-%RANDOM%.txt"
set >"%IDF_ENV_BEFORE%"
call C:\Espressif\frameworks\esp-idf-v5.5.6\export.bat >"%TEMP%\idf-export.log" 2>&1 || exit /b 1
python "%~dp0idf_env_cache.py" "%IDF_ENV_BEFORE%" "%IDF_ENV_CACHE%" >nul 2>&1
del "%IDF_ENV_BEFORE%" 2>nul

:run
cd /d "%~dp0"
idf.py %*
