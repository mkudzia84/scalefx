<#
.SYNOPSIS
    Bootstrap the IDF-component dependency tree for the HubFX ESP32-S3
    build on a fresh clone.

.DESCRIPTION
    pioarduino's Arduino-as-IDF-component build path downloads ~430 MB
    of ESP-IDF component sources into
    `controllers/hubfx/esp32s3/managed_components/` via the IDF
    Component Manager.  Those are gitignored — this script populates
    them on demand so a fresh clone can build without manual steps.

    The mechanism is simple: kick off a `pio run` against the HubFX
    env, which triggers pioarduino to:
      1. Generate the IDF skeleton (CMakeLists.txt, src/CMakeLists.txt,
         sdkconfig.defaults, sdkconfig.esp32s3, .dummy/)
      2. Resolve component dependencies (Arduino-ESP32 + transitive)
      3. Download every component archive into managed_components/
      4. Compile the firmware

    First invocation takes 15-30 min (one-time toolchain + component
    download).  Subsequent runs from a cached state finish in 30-90 s.

    The script is idempotent — re-running it with everything cached
    just verifies + recompiles.

.PARAMETER Clean
    Delete managed_components/, sdkconfig*, .dummy/, and the .pio build
    cache before kicking off the build.  Use when the dependency tree
    is corrupted or you want to validate a fresh-clone scenario.

.PARAMETER SkipBuild
    Stop after the component download phase; don't link.  Useful when
    you only need the dependency tree populated (e.g. for IDE indexing).

.EXAMPLE
    .\tools\setup-hubfx-idf-deps.ps1
    Idempotent: ensures everything is downloaded and builds the firmware.

.EXAMPLE
    .\tools\setup-hubfx-idf-deps.ps1 -Clean
    Wipe + redownload — the "rm -rf and start over" path.

.NOTES
    PowerShell ONLY.  The IDF tools refuse to run under MSYS/Mingw
    (Git-Bash) and the build will fail with cryptic FileNotFoundError
    errors if you try.  Run from a Windows PowerShell prompt.

    Requires:
      - pio (PlatformIO core, installed by Studio's install script or
        manually via the PlatformIO docs)
      - Python 3.10+ (pio pulls its own venv)
      - ~5 GB free disk for component cache + build output
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path "$PSScriptRoot\.."
$hubfxDir = Join-Path $repoRoot "controllers\hubfx\esp32s3"

if (-not (Test-Path $hubfxDir)) {
    Write-Error "HubFX project dir not found at: $hubfxDir"
    exit 1
}

# Sanity check — refuse to run under MSYS/MinGW (Git-Bash).  ESP-IDF's
# idf_tools.py explicitly errors out in that environment.
if ($env:MSYSTEM -or $env:OSTYPE -match "msys|cygwin") {
    Write-Error "ESP-IDF tools cannot run under MSYS/MinGW shells.  Re-run from PowerShell or cmd."
    exit 1
}

if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
    Write-Error "pio not on PATH.  Install PlatformIO Core (https://docs.platformio.org/page/core/installation.html) or run from a Studio terminal."
    exit 1
}

Push-Location $hubfxDir
try {
    if ($Clean) {
        Write-Host "[clean] removing IDF-component artifacts under $hubfxDir" -ForegroundColor Cyan
        @(".dummy", "managed_components", ".pio") | ForEach-Object {
            $p = Join-Path $hubfxDir $_
            if (Test-Path $p) {
                Write-Host "  rm -rf $p"
                Remove-Item -Recurse -Force $p
            }
        }
        @("sdkconfig.defaults", "sdkconfig.esp32s3", "sdkconfig.esp32s3_debug",
          "sdkconfig.esp32s3_release", "dependencies.lock", "CMakeLists.txt",
          "src\CMakeLists.txt") | ForEach-Object {
            $p = Join-Path $hubfxDir $_
            if (Test-Path $p) {
                Write-Host "  rm $p"
                Remove-Item -Force $p
            }
        }
    }

    $target = if ($SkipBuild) { "compiledb" } else { "" }
    Write-Host "[pio] running 'pio run -e esp32s3' (first invocation: 15-30 min)..." -ForegroundColor Cyan
    if ($target) {
        & pio run -e esp32s3 -t $target
    } else {
        & pio run -e esp32s3
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Error "pio run failed (exit $LASTEXITCODE).  See output above for diagnostics."
        exit $LASTEXITCODE
    }

    $mcPath = Join-Path $hubfxDir "managed_components"
    if (Test-Path $mcPath) {
        $size = (Get-ChildItem $mcPath -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
        $sizeMB = [math]::Round($size / 1MB, 1)
        $componentCount = (Get-ChildItem $mcPath -Directory).Count
        Write-Host ""
        Write-Host "[done] $componentCount IDF components in managed_components/ ($sizeMB MB)" -ForegroundColor Green
    } else {
        Write-Warning "managed_components/ not populated — pioarduino's IDF-component path may not have triggered."
    }
} finally {
    Pop-Location
}
