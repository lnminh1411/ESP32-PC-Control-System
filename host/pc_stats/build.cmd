@echo off
rem Builds build\ESP32-Helper.exe: one self-contained single-file .NET 10 exe,
rem the service (PcStats.cs) and its user-session half (AgyQuota.cs,
rem AgyUsage.cs, started by the service with --user). Needs the .NET 10 SDK;
rem packages come from nuget.org (nuget.config). The HOST_* keys of
rem firmware\main\settings.h are compiled in (gen_settings.ps1).
setlocal
cd /d "%~dp0"

where dotnet >nul 2>nul || (echo .NET SDK not found: install the .NET 10 SDK & goto :fail)

rem leftovers of the .NET Framework build; install.ps1 copies all of build\
if exist build del /q build\* 2>nul

rem HOST_* settings from firmware\main\settings.h -> Settings.g.cs
powershell -NoProfile -ExecutionPolicy Bypass -File gen_settings.ps1 || goto :fail

dotnet publish ESP32-Helper.csproj -c Release -o build --nologo -v quiet || goto :fail
del /q build\*.pdb 2>nul
echo OK: build\ESP32-Helper.exe
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
