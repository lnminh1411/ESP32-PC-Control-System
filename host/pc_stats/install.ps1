[CmdletBinding()]
param(
  [switch]$ForceConfig
)

$ErrorActionPreference = "Stop"

# Require administrator privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Write-Error "Administrator privileges required. Please run PowerShell as Administrator."
  exit 1
}

$scriptDir = $PSScriptRoot
$buildExe = Join-Path -Path $scriptDir -ChildPath "build\esp32-pcstats.exe"
$buildAgyExe = Join-Path -Path $scriptDir -ChildPath "build\esp32-agyquota.exe"
$buildCmd = Join-Path -Path $scriptDir -ChildPath "build.cmd"
$buildDir = Join-Path -Path $scriptDir -ChildPath "build"
$statsIni = Join-Path -Path $scriptDir -ChildPath "stats.ini"
$agyIni = Join-Path -Path $scriptDir -ChildPath "agy_quota.ini"
$installDir = "C:\Program Files\ESP32 PC Controller\pc_stats"
$targetExe = Join-Path -Path $installDir -ChildPath "esp32-pcstats.exe"
$targetAgyExe = Join-Path -Path $installDir -ChildPath "esp32-agyquota.exe"
$targetIni = Join-Path -Path $installDir -ChildPath "stats.ini"
$targetAgyIni = Join-Path -Path $installDir -ChildPath "agy_quota.ini"

# Run build.cmd if build\esp32-pcstats.exe or build\esp32-agyquota.exe is missing
if (-not (Test-Path -LiteralPath $buildExe) -or -not (Test-Path -LiteralPath $buildAgyExe)) {
  Write-Host "Build artifacts missing. Running build.cmd..."
  & cmd.exe /c "`"$buildCmd`""
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $buildExe) -or -not (Test-Path -LiteralPath $buildAgyExe)) {
    Write-Error "Build failed."
    exit 1
  }
}

# Stop the service if it exists
$service = Get-Service -Name "ESP32PcStats" -ErrorAction SilentlyContinue
if ($service) {
  if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
    Write-Host "Stopping existing ESP32PcStats service..."
    Stop-Service -Name "ESP32PcStats" -Force
    $service.WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(10))
  }
}

# Stop existing scheduled task / process if running so file copy succeeds
$taskName = "ESP32 AGY Quota"
$existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existingTask) {
  Write-Host "Stopping existing scheduled task $taskName..."
  Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
}
Get-Process -Name "esp32-agyquota" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# Copy build\* to "C:\Program Files\ESP32 PC Controller\pc_stats\"
if (-not (Test-Path -LiteralPath $installDir)) {
  New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}
# stats.ini and agy_quota.ini are handled below (never overwrite the user's copy by accident)
Get-ChildItem -LiteralPath $buildDir | Where-Object { $_.Name -ne "stats.ini" -and $_.Name -ne "agy_quota.ini" } | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination $installDir -Force
}

# Copy stats.ini there ONLY if not already present (keep user edits) -- with a -ForceConfig switch to overwrite
if ($ForceConfig -or -not (Test-Path -LiteralPath $targetIni)) {
  Copy-Item -LiteralPath $statsIni -Destination $targetIni -Force
  Write-Host "Installed stats.ini to $installDir"
} else {
  Write-Host "Preserved existing stats.ini in $installDir (use -ForceConfig to overwrite)"
}

# Copy agy_quota.ini there ONLY if not already present (respect -ForceConfig)
if ($ForceConfig -or -not (Test-Path -LiteralPath $targetAgyIni)) {
  Copy-Item -LiteralPath $agyIni -Destination $targetAgyIni -Force
  Write-Host "Installed agy_quota.ini to $installDir"
} else {
  Write-Host "Preserved existing agy_quota.ini in $installDir (use -ForceConfig to overwrite)"
}

# Create service "ESP32PcStats" if missing
$service = Get-Service -Name "ESP32PcStats" -ErrorAction SilentlyContinue
if (-not $service) {
  Write-Host "Creating service ESP32PcStats..."
  $binPath = "`"$targetExe`""
  New-Service -Name "ESP32PcStats" `
              -BinaryPathName $binPath `
              -DisplayName "ESP32 PC Controller - PC stats" `
              -Description "Streams hardware sensor statistics to the ESP32-S3 carrier board" `
              -StartupType Automatic | Out-Null
}

# Set failure recovery
Write-Host "Configuring failure recovery..."
& sc.exe failure ESP32PcStats reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null

# Start service
Write-Host "Starting ESP32PcStats service..."
Start-Service -Name "ESP32PcStats"

# Register Scheduled Task "ESP32 AGY Quota"
Write-Host "Registering scheduled task $taskName..."
$currentUser = $env:USERNAME
$action = New-ScheduledTaskAction -Execute $targetAgyExe -WorkingDirectory $installDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $currentUser
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "Scheduled task $taskName started."

# Print status + log path
$service = Get-Service -Name "ESP32PcStats"
Write-Host "Service status: $($service.Status)"
Write-Host "Log path: C:\ProgramData\ESP32PcStats\log.txt"
