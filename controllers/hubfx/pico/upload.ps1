# HubFX Pico Upload Script
# Builds and uploads firmware via software-triggered BOOTSEL mode
# Usage: .\upload.ps1 [-NoBuild] [-ComPort COM10]

param(
    [switch]$NoBuild,
    [string]$ComPort = "COM10",
    [int]$Timeout = 15
)

$ErrorActionPreference = "Stop"

Write-Host "`n=== HubFX Pico Upload Script ===" -ForegroundColor Cyan

# Step 1: Build (unless -NoBuild specified)
if (-not $NoBuild) {
    Write-Host "`n[1/3] Building firmware..." -ForegroundColor Yellow
    python -m platformio run --environment pico
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Build failed!" -ForegroundColor Red
        exit 1
    }
    Write-Host "[OK] Build successful" -ForegroundColor Green
} else {
    Write-Host "`n[1/3] Skipping build (-NoBuild specified)" -ForegroundColor Gray
}

# Step 2: Trigger BOOTSEL mode via serial command
Write-Host "`n[2/3] Triggering BOOTSEL mode via serial..." -ForegroundColor Yellow

try {
    # Check if COM port exists
    if (-not (Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $ComPort })) {
        Write-Host "[WARN] $ComPort not found. Device may already be in BOOTSEL mode or disconnected." -ForegroundColor Yellow
    } else {
        # Send bootsel command
        $port = New-Object System.IO.Ports.SerialPort $ComPort, 115200
        $port.ReadTimeout = 2000
        $port.WriteTimeout = 2000
        $port.Open()
        Start-Sleep -Milliseconds 300
        $port.WriteLine("bootsel")
        Start-Sleep -Milliseconds 500
        $port.Close()
        Write-Host "[OK] BOOTSEL command sent" -ForegroundColor Green
    }
} catch {
    Write-Host "[WARN] Could not send bootsel command: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host "       Device may already be in BOOTSEL mode or not responding." -ForegroundColor Yellow
}

# Step 3: Wait for RPI-RP2 drive and copy firmware
Write-Host "`n[3/3] Waiting for RPI-RP2 drive..." -ForegroundColor Yellow

$elapsed = 0
$found = $false

while ($elapsed -lt $Timeout) {
    $drive = Get-Volume | Where-Object { $_.FileSystemLabel -eq "RPI-RP2" }
    if ($drive) {
        $driveLetter = $drive.DriveLetter
        $found = $true
        break
    }
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
    Write-Host "." -NoNewline
}
Write-Host ""

if (-not $found) {
    Write-Host "[ERROR] RPI-RP2 drive not found within ${Timeout}s timeout!" -ForegroundColor Red
    Write-Host "        Try: Hold BOOTSEL button and press RESET manually" -ForegroundColor Yellow
    exit 1
}

Write-Host "[OK] RPI-RP2 drive found at ${driveLetter}:" -ForegroundColor Green

# Copy firmware
$firmwarePath = ".pio\build\pico\firmware.uf2"
if (-not (Test-Path $firmwarePath)) {
    Write-Host "[ERROR] Firmware file not found: $firmwarePath" -ForegroundColor Red
    exit 1
}

Write-Host "     Copying firmware.uf2..." -ForegroundColor Cyan
Copy-Item $firmwarePath -Destination "${driveLetter}:\" -Force

# Wait for device to reboot
Write-Host "     Waiting for device to reboot..." -ForegroundColor Cyan
Start-Sleep -Seconds 3

# Verify device is back online
$elapsed = 0
while ($elapsed -lt 10) {
    if (Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $ComPort }) {
        Write-Host "`n[SUCCESS] Upload complete! Device back online at $ComPort" -ForegroundColor Green
        exit 0
    }
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
}

Write-Host "`n[WARN] Upload complete but device not responding on $ComPort" -ForegroundColor Yellow
Write-Host "       Check device manually." -ForegroundColor Yellow
exit 0
