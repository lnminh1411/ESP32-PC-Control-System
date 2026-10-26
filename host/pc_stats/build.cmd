@echo off
rem Builds build\ESP32-Helper.exe. Fetches the lhwm-wrapper CI artifact
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
  rem Windows tar (bsdtar) reads zip; a GNU tar earlier on PATH (Git Bash) cannot
  "%SystemRoot%\System32\tar.exe" -xf vendor\artifacts.zip -C vendor x64/Release/lhwm-wrapper.dll || goto :fail
  move /y vendor\x64\Release\lhwm-wrapper.dll vendor\ >nul
  rmdir /s /q vendor\x64
  del vendor\artifacts.zip
)

if not exist build mkdir build
rem the two exes this one replaced; install.ps1 copies all of build\
del /q build\esp32-pcstats.exe build\esp32-agyquota.exe 2>nul

set "WINMD="
set "KITS=%ProgramFiles(x86)%\Windows Kits\10\UnionMetadata"
if not exist "%KITS%" goto :fallback_winmd
pushd "%KITS%"
for /f "delims=" %%d in ('dir /b /ad /o-n 10.* 2^>nul') do (
  if not defined WINMD (
    if exist "%%d\Windows.winmd" (
      set WINMD="/r:%%~fd\Windows.winmd"
    )
  )
)
popd

:fallback_winmd
if not defined WINMD (
  set WINMD="/r:%WINDIR%\System32\WinMetadata\Windows.Media.winmd" "/r:%WINDIR%\System32\WinMetadata\Windows.Foundation.winmd" "/r:%WINDIR%\System32\WinMetadata\Windows.Storage.winmd"
)

rem One exe: the service (PcStats.cs) and its user-session half (AgyQuota.cs,
rem AgyUsage.cs, started by the service with --user)
"%CSC%" /nologo /optimize+ /platform:x64 /target:exe /out:build\ESP32-Helper.exe ^
  /r:vendor\lhwm-wrapper.dll /r:System.ServiceProcess.dll /r:System.Management.dll ^
  /r:System.Web.Extensions.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Core.dll ^
  /r:"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.WindowsRuntime.dll" ^
  /r:"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.dll" ^
  %WINMD% ^
  PcStats.cs AgyQuota.cs AgyUsage.cs || goto :fail
copy /y vendor\lhwm-wrapper.dll build\ >nul
echo OK: build\ESP32-Helper.exe

exit /b 0

:fail
echo BUILD FAILED
exit /b 1
