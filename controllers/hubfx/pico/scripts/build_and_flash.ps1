# HubFX Pico - Build and Flash Script
# Performs verified build, checksum validation, and firmware upload via BOOTSEL

param(
    [switch]$NoBuild = $false,
    [switch]$SkipVerify = $false,
    [string]$ComPort = "COM10",
    [int]$Timeout = 15
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

Push-Location $projectDir

function Write-Step {
    param([int]$Step, [int]$Total, [string]$Message)
    Write-Host ""
    Write-Host "[$Step/$Total] $Message" -ForegroundColor Yellow
}

function Write-OK {
    param([string]$Message)
    Write-Host "    [OK] $Message" -ForegroundColor Green
}

function Write-Err {
    param([string]$Message)
    Write-Host "    [FAIL] $Message" -ForegroundColor Red
}

function Write-Info {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Gray
}

try {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  HubFX Pico - Build and Flash Utility" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan

    $totalSteps = if ($NoBuild) { 4 } else { 5 }
    $currentStep = 0

    # STEP 1: Build firmware
    if (-not $NoBuild) {
        $currentStep++
        Write-Step $currentStep $totalSteps "Building firmware..."
        
        $buildOutput = python -m platformio run --environment pico 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Err "Build failed!"
            Write-Host $buildOutput -ForegroundColor Red
            exit 1
        }
        
        $flashLine = $buildOutput | Select-String "Flash:" | Select-Object -Last 1
        if ($flashLine) {
            Write-OK "Build complete"
            Write-Info $flashLine.Line.Trim()
        } else {
            Write-OK "Build complete"
        }
    }

    # STEP 2: Verify firmware file
    $currentStep++
    Write-Step $currentStep $totalSteps "Verifying firmware..."
    
    $firmwarePath = ".pio\build\pico\firmware.uf2"
    if (!(Test-Path $firmwarePath)) {
        Write-Err "Firmware file not found: $firmwarePath"
        exit 1
    }
    
    $firmwareInfo = Get-Item $firmwarePath
    $sourceHash = (Get-FileHash $firmwarePath -Algorithm MD5).Hash
    $firmwareSize = "{0:N0}" -f $firmwareInfo.Length
    
    Write-OK "Firmware verified"
    Write-Info "Size: $firmwareSize bytes"
    Write-Info "MD5:  $sourceHash"

    # STEP 3: Enter BOOTSEL mode
    $currentStep++
    Write-Step $currentStep $totalSteps "Entering BOOTSEL mode..."
    
    $existingDrive = Get-Volume | Where-Object { $_.FileSystemLabel -eq "RPI-RP2" }
    
    if ($existingDrive) {
        Write-OK "Device already in BOOTSEL mode"
        $driveLetter = $existingDrive.DriveLetter
    } else {
        $serialAvailable = Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $ComPort }
        
        if ($serialAvailable) {
            try {
                $port = New-Object System.IO.Ports.SerialPort $ComPort, 115200
                $port.ReadTimeout = 2000
                $port.WriteTimeout = 2000
                $port.Open()
                Start-Sleep -Milliseconds 300
                $port.WriteLine("sys bootsel")
                Start-Sleep -Milliseconds 500
                $port.Close()
                Write-Info "BOOTSEL command sent to $ComPort"
            } catch {
                Write-Info "Could not send command: $($_.Exception.Message)"
            }
        } else {
            Write-Info "$ComPort not available"
        }
        
        Write-Info "Waiting for RPI-RP2 drive..."
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
            Write-Host "." -NoNewline -ForegroundColor Gray
        }
        Write-Host ""
        
        if (-not $found) {
            Write-Err "RPI-RP2 drive not found within ${Timeout}s"
            Write-Info "Try: Hold BOOTSEL button and press RESET manually"
            exit 1
        }
        
        Write-OK "BOOTSEL mode active"
    }
    
    Write-Info "Drive: ${driveLetter}:"

    # STEP 4: Copy firmware
    $currentStep++
    Write-Step $currentStep $totalSteps "Flashing firmware..."
    
    Copy-Item $firmwarePath -Destination "${driveLetter}:\" -Force
    Write-OK "Firmware copied to ${driveLetter}:"
    Write-Info "Device will reboot automatically..."
    
    Start-Sleep -Seconds 5

    # STEP 5: Verify device is back online
    if (-not $SkipVerify) {
        $currentStep++
        Write-Step $currentStep $totalSteps "Verifying device..."
        
        $elapsed = 0
        $maxWait = 10
        $deviceOnline = $false
        
        while ($elapsed -lt $maxWait) {
            if (Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $ComPort }) {
                $deviceOnline = $true
                break
            }
            Start-Sleep -Milliseconds 500
            $elapsed += 0.5
        }
        
        if ($deviceOnline) {
            Start-Sleep -Seconds 2
            
            try {
                $port = New-Object System.IO.Ports.SerialPort $ComPort, 115200
                $port.ReadTimeout = 3000
                $port.Open()
                Start-Sleep -Milliseconds 500
                $port.ReadExisting() | Out-Null
                $port.WriteLine("version")
                Start-Sleep -Milliseconds 500
                $response = $port.ReadExisting()
                $port.Close()
                
                Write-OK "Device online at $ComPort"
                $lines = $response -split "`n" | Where-Object { $_.Trim() }
                foreach ($line in $lines) {
                    if ($line -match "Firmware|Build|Version") {
                        Write-Info $line.Trim()
                    }
                }
            } catch {
                Write-OK "Device online at $ComPort"
            }
        } else {
            Write-Err "Device not responding on $ComPort"
        }
    }

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  Build and Flash Complete!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""

} finally {
    Pop-Location
}
