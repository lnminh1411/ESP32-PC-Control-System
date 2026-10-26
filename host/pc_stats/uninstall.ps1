[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

# Require administrator privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Write-Error "Administrator privileges required. Run PowerShell as Administrator."
  exit 1
}

$installDir = "C:\Program Files\ESP32 PC Controller\pc_stats"

# Scheduled task from installs before the merge into ESP32-Helper.exe
$taskName = "ESP32 AGY Quota"
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($task) {
  Write-Host "Stopping and unregistering old scheduled task $taskName..."
  Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
Get-Process -Name "esp32-agyquota" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# Stop service if it exists
$service = Get-Service -Name "ESP32PcStats" -ErrorAction SilentlyContinue
if ($service) {
  if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
    Write-Host "Stopping ESP32PcStats service..."
    Stop-Service -Name "ESP32PcStats" -Force
    $service.WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(10))
  }
}

# Delete service using sc.exe delete
Write-Host "Deleting ESP32PcStats service..."
& sc.exe delete ESP32PcStats | Out-Null

# Remove the install directory, leaving ProgramData log
if (Test-Path -LiteralPath $installDir) {
  Write-Host "Removing $installDir..."
  Remove-Item -LiteralPath $installDir -Recurse -Force
}

Write-Host "ESP32PcStats uninstalled successfully. (Log file at C:\ProgramData\ESP32PcStats\log.txt preserved)"
