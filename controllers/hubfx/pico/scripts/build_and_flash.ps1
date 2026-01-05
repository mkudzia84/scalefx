# HubFX Pico - Build and Flash Script
# Performs verified build, checksum validation, and firmware upload

param(
    [switch]$SkipTests = $false
)

$ErrorActionPreference = "Stop"

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  HubFX Pico - Build & Flash Utility" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Step 1: Clean build
Write-Host "[1/6] Cleaning build directory..." -ForegroundColor Yellow
python -m platformio run --target clean | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "✗ Clean failed!" -ForegroundColor Red
    exit 1
}
Write-Host "✓ Clean complete`n" -ForegroundColor Green

# Step 2: Build firmware
Write-Host "[2/6] Building firmware..." -ForegroundColor Yellow
$buildOutput = python -m platformio run --environment pico 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "✗ Build failed!" -ForegroundColor Red
    Write-Host $buildOutput
    exit 1
}
$flashSize = $buildOutput | Select-String "Flash:" | Select-Object -Last 1
Write-Host "✓ Build complete - $flashSize`n" -ForegroundColor Green

# Step 3: Calculate source checksum
Write-Host "[3/6] Calculating firmware checksum..." -ForegroundColor Yellow
$firmwarePath = ".pio\build\pico\firmware.uf2"
if (!(Test-Path $firmwarePath)) {
    Write-Host "✗ Firmware file not found!" -ForegroundColor Red
    exit 1
}
$sourceHash = (Get-FileHash $firmwarePath -Algorithm MD5).Hash
Write-Host "  MD5: $sourceHash" -ForegroundColor Gray
Write-Host "✓ Checksum calculated`n" -ForegroundColor Green

# Step 4: Enter BOOTSEL mode
Write-Host "[4/6] Entering BOOTSEL mode..." -ForegroundColor Yellow
try {
    $port = New-Object System.IO.Ports.SerialPort "COM10", 115200
    $port.Open()
    Start-Sleep -Milliseconds 500
    $port.WriteLine("bootsel")
    Start-Sleep -Seconds 2
    $port.Close()
} catch {
    Write-Host "✗ Failed to send bootsel command: $_" -ForegroundColor Red
    exit 1
}

Start-Sleep -Seconds 3
$drive = (Get-Volume | Where-Object {$_.FileSystemLabel -eq 'RPI-RP2'}).DriveLetter
if (!$drive) {
    Write-Host "✗ Pico not in BOOTSEL mode!" -ForegroundColor Red
    exit 1
}
Write-Host "✓ Pico in BOOTSEL mode (Drive $drive`:)`n" -ForegroundColor Green

# Step 5: Copy firmware with checksum verification
Write-Host "[5/6] Uploading firmware..." -ForegroundColor Yellow
Copy-Item $firmwarePath "${drive}:\" -Force
Start-Sleep -Seconds 2

if (Test-Path "${drive}:\firmware.uf2") {
    $destHash = (Get-FileHash "${drive}:\firmware.uf2" -Algorithm MD5).Hash
    if ($sourceHash -eq $destHash) {
        Write-Host "✓ Checksum verified - Upload complete`n" -ForegroundColor Green
    } else {
        Write-Host "✗ Checksum mismatch!" -ForegroundColor Red
        Write-Host "  Source: $sourceHash" -ForegroundColor Gray
        Write-Host "  Dest:   $destHash" -ForegroundColor Gray
        exit 1
    }
} else {
    Write-Host "✓ Firmware copied - Pico is rebooting`n" -ForegroundColor Green
}

# Step 6: Wait for reboot and test
Write-Host "[6/6] Waiting for boot..." -ForegroundColor Yellow
Start-Sleep -Seconds 7

if (!$SkipTests) {
    Write-Host "`nRunning codec test..." -ForegroundColor Cyan
    python tests/test_codec.py
} else {
    Write-Host "✓ Skipping tests`n" -ForegroundColor Green
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Build & Flash Complete!" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan
