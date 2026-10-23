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
$buildCmd = Join-Path -Path $scriptDir -ChildPath "build.cmd"
$buildDir = Join-Path -Path $scriptDir -ChildPath "build"
$statsIni = Join-Path -Path $scriptDir -ChildPath "stats.ini"
$installDir = "C:\Program Files\ESP32 PC Controller\pc_stats"
$targetExe = Join-Path -Path $installDir -ChildPath "esp32-pcstats.exe"
$targetIni = Join-Path -Path $installDir -ChildPath "stats.ini"

# Run build.cmd if build\esp32-pcstats.exe is missing
if (-not (Test-Path -LiteralPath $buildExe)) {
  Write-Host "build\esp32-pcstats.exe missing. Running build.cmd..."
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

# Copy build\* to "C:\Program Files\ESP32 PC Controller\pc_stats\"
if (-not (Test-Path -LiteralPath $installDir)) {
  New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}
# stats.ini is handled below (never overwrite the user's copy by accident)
Get-ChildItem -LiteralPath $buildDir | Where-Object { $_.Name -ne "stats.ini" } | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination $installDir -Force
}

# Copy stats.ini there ONLY if not already present (keep user edits) -- with a -ForceConfig switch to overwrite
if ($ForceConfig -or -not (Test-Path -LiteralPath $targetIni)) {
  Copy-Item -LiteralPath $statsIni -Destination $targetIni -Force
  Write-Host "Installed stats.ini to $installDir"
} else {
  Write-Host "Preserved existing stats.ini in $installDir (use -ForceConfig to overwrite)"
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

# Print status + log path
$service = Get-Service -Name "ESP32PcStats"
Write-Host "Service status: $($service.Status)"
Write-Host "Log path: C:\ProgramData\ESP32PcStats\log.txt"
