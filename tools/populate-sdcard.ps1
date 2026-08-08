<#
.SYNOPSIS
  Populate a ScaleFX SD card with the media/sounds library.

.DESCRIPTION
  Copies media/sounds/* onto the card as \sounds\* — the exact layout the
  HubFX firmware expects (/sounds/sys, /sounds/KA50, /sounds/EC665,
  /sounds/2A42, /sounds/PEWPEW, ...).  MP3 and WAV both play; the mixer
  dispatches by extension.

  Only SOUNDS live on the SD card.  YAML presets (media/presets/*) go to
  the board's LittleFS flash — deploy those with scalefx-cli config-save
  or Studio's file manager, not this script.

.PARAMETER Drive
  Target drive letter, e.g. "D:".  Omitted: auto-detects the single
  removable drive and refuses to guess when there are several.

.PARAMETER Mirror
  Also DELETE files under \sounds on the card that are not in the repo
  (robocopy /MIR).  Default is additive copy (/E — never deletes).

.PARAMETER DryRun
  List what would be copied without writing anything.

.EXAMPLE
  .\tools\populate-sdcard.ps1              # auto-detect card, additive copy
  .\tools\populate-sdcard.ps1 -Drive E: -Mirror
#>
param(
    [string]$Drive,
    [switch]$Mirror,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repoRoot 'media\sounds'
if (-not (Test-Path $src)) { throw "Source not found: $src (run from the scalefx repo)" }

if (-not $Drive) {
    $removable = @(Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 2 })
    if ($removable.Count -eq 0) { throw 'No removable drive found - insert the SD card or pass -Drive X:' }
    if ($removable.Count -gt 1) {
        throw "Multiple removable drives ($(($removable | ForEach-Object DeviceID) -join ', ')) - pass -Drive X: explicitly"
    }
    $Drive = $removable[0].DeviceID
    Write-Host "Auto-detected removable drive $Drive" -ForegroundColor Cyan
} else {
    $Drive = $Drive.TrimEnd('\').TrimEnd(':') + ':'
    $disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$Drive'"
    if (-not $disk) { throw "Drive $Drive not found" }
    if ($disk.DriveType -ne 2) {
        throw "Drive $Drive is not removable (DriveType=$($disk.DriveType)) - refusing; SD cards enumerate as removable"
    }
}

$dst = Join-Path "$Drive\" 'sounds'

$mode = '/E'
if ($Mirror) { $mode = '/MIR' }
$flags = @($mode, '/NJH', '/NDL', '/NP')
if ($DryRun) { $flags += '/L' }

Write-Host "robocopy $src -> $dst  ($mode$(if ($DryRun) { ', dry run' }))" -ForegroundColor Cyan
robocopy $src $dst @flags
$rc = $LASTEXITCODE
# robocopy: 0 = nothing to do, 1-7 = copied/extra (success), >=8 = failure.
if ($rc -ge 8) { throw "robocopy failed with exit code $rc" }

if (-not $DryRun) {
    $files = Get-ChildItem $dst -Recurse -File
    $mb = ($files | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ("OK - {0} files, {1:N1} MB under {2}" -f $files.Count, $mb, $dst) -ForegroundColor Green
    Get-ChildItem $dst -Directory | ForEach-Object {
        $n = (Get-ChildItem $_.FullName -Recurse -File).Count
        Write-Host ("  {0,-12} {1} file(s)" -f $_.Name, $n)
    }
}
exit 0
