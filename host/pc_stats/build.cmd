@echo off
rem Builds build\esp32-pcstats.exe. Fetches the lhwm-wrapper CI artifact
rem (OpenRGB's LibreHardwareMonitor build) into vendor\ on first run.
rem Needs only the .NET Framework 4 compiler that ships with Windows.
setlocal
cd /d "%~dp0"
set URL=https://gitlab.com/OpenRGBDevelopers/lhwm-wrapper/-/jobs/artifacts/master/download?job=main_build
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe

if not exist vendor\lhwm-wrapper.dll (
  if not exist vendor mkdir vendor
  echo Downloading lhwm-wrapper...
  curl -sSLf -o vendor\artifacts.zip "%URL%" || goto :fail
  tar -xf vendor\artifacts.zip -C vendor x64/Release/lhwm-wrapper.dll || goto :fail
  move /y vendor\x64\Release\lhwm-wrapper.dll vendor\ >nul
  rmdir /s /q vendor\x64
  del vendor\artifacts.zip
)

if not exist build mkdir build
"%CSC%" /nologo /optimize+ /platform:x64 /target:exe /out:build\esp32-pcstats.exe ^
  /r:vendor\lhwm-wrapper.dll /r:System.ServiceProcess.dll /r:System.Management.dll ^
  /r:System.Web.Extensions.dll ^
  PcStats.cs || goto :fail
copy /y vendor\lhwm-wrapper.dll build\ >nul
echo OK: build\esp32-pcstats.exe
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
