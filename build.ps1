<#
  build.ps1 - one-shot ESP32-S3-WROOM-1 N16R8 (16MB flash, 8MB octal PSRAM)
  build script for esp32_wifi_streamer - the Wi-Fi/streaming half of the
  two-chip split. Reuses the ESP-ADF/ESP-IDF checkout vendored one level up
  in ..\esp-adf rather than duplicating it.

  2026-08-22: migrated from an ESP32-WROOM-32U (4MB flash, no PSRAM) - see
  sdkconfig.defaults/main/app_config.h for what that changed. Port default
  below corrected to this project's actual port (COM10, CH340) - it used to
  read COM7, which is esp32_bt_speaker's port, not this project's.

  Usage:
    powershell -NoExit -ExecutionPolicy Bypass -File "C:\Users\Ashish\Github\esp32_bt_int_radio\esp32_wifi_streamer\build.ps1"
    powershell -NoExit -ExecutionPolicy Bypass -File "...\build.ps1" -Flash -Port COM10
    powershell -NoExit -ExecutionPolicy Bypass -File "...\build.ps1" -Clean
#>

param(
    [switch]$Clean,          # wipe build/ and managed_components/ before building (use after dependency/space issues)
    [switch]$Flash,          # flash + monitor after a successful build
    [string]$Port = "COM10"  # serial port to use with -Flash
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "C:\Users\Ashish\Github\esp32_bt_int_radio\esp32_wifi_streamer"
$AdfExport   = "C:\Users\Ashish\Github\esp32_bt_int_radio\esp-adf\export.ps1"

if (-not (Test-Path $AdfExport)) {
    Write-Error "esp-adf export.ps1 not found at $AdfExport"
    exit 1
}

Set-Location $ProjectRoot

$IdfProfile = "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1"
if (-not (Test-Path $IdfProfile)) {
    Write-Error "IDF profile script not found at $IdfProfile"
    exit 1
}

# Only build the ESP-ADF components this project's CMakeLists.txt actually
# REQUIRES (main/CMakeLists.txt has no `bt`/`bluetooth_service` at all on
# this chip), instead of ESP-ADF's entire component tree.
$env:MINIMAL_BUILD = "1"

Write-Host "== Activating ESP-IDF v5.5.5 (official profile) ==" -ForegroundColor Cyan
. $IdfProfile

Write-Host "== Activating ESP-ADF (ADF_PATH + patches) ==" -ForegroundColor Cyan
. $AdfExport

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Error "idf.py not on PATH after activation - environment setup failed, see output above."
    exit 1
}

if ($Clean) {
    Write-Host "== Clean requested: removing build/, managed_components/, dependencies.lock ==" -ForegroundColor Yellow
    Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "managed_components" -ErrorAction SilentlyContinue
    Remove-Item -Force "dependencies.lock" -ErrorAction SilentlyContinue
}

# sdkconfig.defaults changes are only picked up when sdkconfig doesn't already
# exist, so force a clean regeneration every run.
Write-Host "== Regenerating sdkconfig from sdkconfig.defaults ==" -ForegroundColor Cyan
Remove-Item -Force "sdkconfig" -ErrorAction SilentlyContinue

idf.py set-target esp32s3
if ($LASTEXITCODE -ne 0) { Write-Error "set-target failed"; exit $LASTEXITCODE }

Write-Host "== Building ==" -ForegroundColor Cyan
idf.py build
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit $LASTEXITCODE }

Write-Host "== Build succeeded ==" -ForegroundColor Green

if ($Flash) {
    Write-Host "== Flashing + monitoring on $Port ==" -ForegroundColor Cyan
    idf.py -p $Port flash monitor
}
