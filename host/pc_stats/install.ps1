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
$buildExe = Join-Path -Path $scriptDir -ChildPath "build\ESP32-Helper.exe"
$buildCmd = Join-Path -Path $scriptDir -ChildPath "build.cmd"
$buildDir = Join-Path -Path $scriptDir -ChildPath "build"
$statsIni = Join-Path -Path $scriptDir -ChildPath "stats.ini"
$agyIni = Join-Path -Path $scriptDir -ChildPath "agy_quota.ini"
$installDir = "C:\Program Files\ESP32 PC Controller\pc_stats"
$targetExe = Join-Path -Path $installDir -ChildPath "ESP32-Helper.exe"
$targetIni = Join-Path -Path $installDir -ChildPath "stats.ini"
$targetAgyIni = Join-Path -Path $installDir -ChildPath "agy_quota.ini"

# Run build.cmd if build\ESP32-Helper.exe is missing
if (-not (Test-Path -LiteralPath $buildExe)) {
  Write-Host "Build artifacts missing. Running build.cmd..."
  & cmd.exe /c "`"$buildCmd`""
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $buildExe)) {
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

# Before the merge the user-session half was a separate exe run by a scheduled
# task; the service now starts it itself. Remove the task and the old exes.
$taskName = "ESP32 AGY Quota"
$existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existingTask) {
  Write-Host "Removing old scheduled task $taskName..."
  Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
$procs = Get-Process -Name "esp32-agyquota", "esp32-pcstats", "ESP32-Helper" -ErrorAction SilentlyContinue
$procs | Stop-Process -Force -ErrorAction SilentlyContinue
$procs | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue   # the exe stays locked until the process is gone
foreach ($old in "esp32-pcstats.exe", "esp32-agyquota.exe") {
  $oldPath = Join-Path -Path $installDir -ChildPath $old
  if (Test-Path -LiteralPath $oldPath) { Remove-Item -LiteralPath $oldPath -Force }
}
# Files the two halves used to pass data through; the pipe replaced them.
# shot_request.txt holds a webhook URL, so it must not linger.
# Kept: log.txt, agy_quota.txt (last-known quota), shot_pending.txt (deletes still due).
$dataDir = "C:\ProgramData\ESP32PcStats"
if (Test-Path -LiteralPath $dataDir) {
  Get-ChildItem -LiteralPath $dataDir -File | Where-Object {
    $_.Name -in @("media.txt", "media_art.bin", "shot_request.txt") -or
    $_.Name -like "*.tmp.*" -or $_.Name -like "*~RF*.TMP"
  } | Remove-Item -Force -ErrorAction SilentlyContinue
}

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

# Create service "ESP32PcStats" if missing; an existing one is pointed at the new exe
$binPath = "`"$targetExe`""
$displayName = "ESP32 PC Controller - Helper"
$description = "Streams hardware sensor statistics to the ESP32-S3 carrier board and runs its user-session half (media, quota, screenshots)"
$service = Get-Service -Name "ESP32PcStats" -ErrorAction SilentlyContinue
if (-not $service) {
  Write-Host "Creating service ESP32PcStats..."
  New-Service -Name "ESP32PcStats" `
              -BinaryPathName $binPath `
              -DisplayName $displayName `
              -Description $description `
              -StartupType Automatic | Out-Null
} else {
  # WMI, not sc.exe: PowerShell 5 drops the inner quotes of a quoted path with spaces
  Write-Host "Pointing service ESP32PcStats at $targetExe..."
  $res = Get-CimInstance -ClassName Win32_Service -Filter "Name='ESP32PcStats'" |
    Invoke-CimMethod -MethodName Change -Arguments @{ PathName = $binPath; DisplayName = $displayName }
  if ($res.ReturnValue -ne 0) {
    Write-Error "Could not update the service path (Win32_Service.Change returned $($res.ReturnValue))."
    exit 1
  }
  & sc.exe description ESP32PcStats $description | Out-Null
}

# Set failure recovery
Write-Host "Configuring failure recovery..."
& sc.exe failure ESP32PcStats reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null

# Start service
Write-Host "Starting ESP32PcStats service..."
Start-Service -Name "ESP32PcStats"

# Print status + log path
$service = Get-Service -Name "ESP32PcStats"
Write-Host "Service status: $($service.Status)"
Write-Host "Log path: C:\ProgramData\ESP32PcStats\log.txt"
