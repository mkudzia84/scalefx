# package-studio.ps1 — assemble a self-contained ScaleFX Studio distribution.
#
#   Bundles everything needed to RUN Studio + flash firmware on a clean Windows
#   box with no toolchain installed:
#     scalefx-studio.exe   the GUI
#     scalefx-cli.exe      the command-line client
#     scalefx-flash.exe    the firmware flasher
#     esptool.exe          ESP32-S3 flashing (found "colocated" next to
#                          scalefx-flash.exe — no Python needed)
#     README.md            quick-start
#
#   Usage:
#     pwsh tools/package-studio.ps1 [-Version 1.0.0-rc1] [-SkipBuild]
#
#   Produces  dist/scalefx-studio-v<Version>.zip  (+ the unzipped staging dir).
#   -SkipBuild packages the already-built binaries (skips go/wails builds).

param(
    [string]$Version = "1.0.0-rc1",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo  = Split-Path $PSScriptRoot -Parent          # tools/ -> repo root
$goDir = Join-Path $repo "app\go"
$name  = "scalefx-studio-v$Version"
$stage = Join-Path $repo "dist\$name"
$zip   = Join-Path $repo "dist\$name.zip"

Write-Host "== Packaging ScaleFX Studio $Version ==" -ForegroundColor Cyan

# 1. Build the host binaries (unless -SkipBuild) -----------------------------
if (-not $SkipBuild) {
    Write-Host "-- building cli + flash (go) --"
    Get-Process scalefx-studio -ErrorAction SilentlyContinue | Stop-Process -Force
    Push-Location $goDir
    try {
        & go build -o scalefx-cli.exe   ./cli/   ; if ($LASTEXITCODE) { throw "scalefx-cli build failed" }
        & go build -o scalefx-flash.exe ./flash/ ; if ($LASTEXITCODE) { throw "scalefx-flash build failed" }
    } finally { Pop-Location }

    Write-Host "-- building Studio (wails) --"
    Push-Location (Join-Path $goDir "studio")
    try { & wails build ; if ($LASTEXITCODE) { throw "wails build failed" } }
    finally { Pop-Location }
}

# 2. Ensure esptool is present (download once if missing) --------------------
$esptool = Join-Path $repo "tools\esptool\esptool.exe"
if (-not (Test-Path $esptool)) {
    Write-Host "-- fetching esptool --"
    & (Join-Path $goDir "scalefx-flash.exe") tools download
}
if (-not (Test-Path $esptool)) { throw "esptool.exe not found (download failed?)" }

# 3. Stage the package ------------------------------------------------------
Write-Host "-- staging $stage --"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

$studioExe = Join-Path $goDir "studio\build\bin\scalefx-studio.exe"
foreach ($f in @($studioExe,
                 (Join-Path $goDir "scalefx-cli.exe"),
                 (Join-Path $goDir "scalefx-flash.exe"),
                 $esptool)) {
    if (-not (Test-Path $f)) { throw "missing artifact: $f (build first, or drop -SkipBuild)" }
    Copy-Item $f $stage
}
$lic = Join-Path $repo "tools\esptool\LICENSE"
if (Test-Path $lic) { Copy-Item $lic (Join-Path $stage "esptool-LICENSE.txt") }

# README ---------------------------------------------------------------------
$readme = @"
# ScaleFX Studio $Version

Self-contained Windows distribution — no Python or toolchain required.

## Contents
| File | Purpose |
|------|---------|
| ``scalefx-studio.exe`` | Desktop GUI — configure boards, flash firmware, drive effects |
| ``scalefx-cli.exe``    | Command-line client (``scalefx-cli.exe -p COM# -c "<command>"``) |
| ``scalefx-flash.exe``  | Firmware build/flash tool |
| ``esptool.exe``        | ESP32-S3 flashing backend (used automatically by scalefx-flash) |

Keep these files **in the same folder** — ``scalefx-flash.exe`` finds
``esptool.exe`` next to itself.

## Quick start
1. Run ``scalefx-studio.exe``.
2. Connect a board over USB; pick its COM port in the connect dialog.
3. **Flash firmware:** open the **Firmware** tab, choose a GitHub release, click
   **Flash**. (HubFX = ESP32-S3 ``.bin``; LightFX / GearControl = Pico ``.uf2``,
   hold BOOTSEL.)

## Firmware downloads
Get the matching firmware from the GitHub releases page:
https://github.com/mkudzia84/scalefx/releases

## CLI examples
``````
scalefx-cli.exe -p COM16 -c "status"
scalefx-cli.exe -p COM16 -c "gear-status"
scalefx-cli.exe -p COM16 -c "subscribe"        # live telemetry / diagnostics
``````

## Flashing from the CLI
``````
scalefx-flash.exe flash hubfx          # ESP32-S3 master (uses esptool.exe here)
scalefx-flash.exe flash gearcontrol    # Pico expander (hold BOOTSEL)
``````

esptool is the Espressif standalone build (v5.2.0); see ``esptool-LICENSE.txt``.
"@
Set-Content -Path (Join-Path $stage "README.md") -Value $readme -Encoding utf8

# 4. Zip --------------------------------------------------------------------
Write-Host "-- zipping --"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "== Packaged: $zip ($mb MB) ==" -ForegroundColor Green
Get-ChildItem $stage | Select-Object Name, @{n='KB';e={[int]($_.Length/1KB)}} | Format-Table -AutoSize
