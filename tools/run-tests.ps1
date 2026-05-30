#!/usr/bin/env pwsh
# ScaleFX HubFX pre-merge test gate (Rule 51 / 52).
#
# Runs every host-runnable test suite under tests/host/ and reports
# PASS / FAIL per suite + a final summary.  Exits non-zero on any failure.
#
# Three modes:
#
#   Default       Go unit tests only.  No hardware required.  ~10 s.
#                 Suitable for CI, no-HW machines, and the inner-loop check.
#
#   -Integration  Add Go integration tests.  Skips integration cleanly if
#                 no HubFX is reachable (SCALEFX_HUBFX_PORT or CH343
#                 auto-detect).  ~25 s with HW.
#
#   -Premerge     The strict gate.  Adds the firmware build
#                 (scalefx-flash build hubfx).  REQUIRES a reachable
#                 HubFX and a working IDF toolchain.  ~70 s.  Run this
#                 before merging to main (Rule 52).
#
# -Loud forwards -v to `go test` for flake-hunting.
#
# Rule 51: tests must build cleanly; refactors carry their tests with
# them.  This script enforces it as one command: a stale test that no
# longer compiles is a non-zero exit.

param(
    [switch] $Integration,
    [switch] $Premerge,
    [switch] $Loud
)

# Pre-merge implies integration.
if ($Premerge) { $Integration = $true }

# Repo root = parent of this script.
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# ---- Pretty output ----------------------------------------------------

function Write-Title { param([string] $text)
    Write-Host ""
    Write-Host ("=== " + $text + " ===") -ForegroundColor Cyan
}

function Write-Pass { param([string] $name, [string] $detail)
    Write-Host ("  " + $name.PadRight(40) + "PASS  " + $detail) -ForegroundColor Green
}

function Write-Fail { param([string] $name, [string] $detail)
    Write-Host ("  " + $name.PadRight(40) + "FAIL  " + $detail) -ForegroundColor Red
}

function Write-SuiteSkip { param([string] $name, [string] $detail)
    Write-Host ("  " + $name.PadRight(40) + "SKIP  " + $detail) -ForegroundColor Yellow
}

# ---- Discovery --------------------------------------------------------

function Get-GoTestSuites { param([string] $glob)
    # Resolve to an absolute path so discovery is robust against cwd
    # drift (Invoke-GoSuite Push/Pop, a test that fails mid-flight, etc.)
    $absPath = Join-Path $repoRoot $glob
    if (-not (Test-Path $absPath)) { return @() }
    Get-ChildItem -Path $absPath -Filter "go.mod" -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object { Split-Path -Parent $_.FullName }
}

# Results accumulators.
$failures = @()
$skipped  = @()
$passed   = @()

# ---- Run one Go test suite --------------------------------------------

function Invoke-GoSuite { param($dir, [switch]$NoCache, [switch]$Integration)
    $name = (Resolve-Path $dir -Relative).Replace("\", "/").TrimStart(".", "/")
    # Strip "tests/host/" prefix for compactness in the summary table.
    $short = $name -replace "^tests/host/", ""

    $start = Get-Date
    Push-Location $dir
    try {
        # `go test` caches results by default and an `ok` cached result
        # masks recently-broken HW (e.g. cable unplugged after a previous
        # successful run).  Integration tier passes -NoCache so the suite
        # actually re-runs.  Unit tests can stay cached for speed.
        #
        # `-v` forced for integration so the per-test `--- SKIP:` /
        # `--- PASS:` lines appear in stdout — without them, an all-tests-
        # skipped run looks identical to an all-tests-passed run in the
        # default `go test` output (just a single `ok` line either way).
        $goArgs = @("test")
        if ($Loud -or $Integration) { $goArgs += "-v" }
        if ($NoCache)               { $goArgs += "-count=1" }
        $goArgs += "./..."
        $output = & go @goArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $elapsed = (Get-Date) - $start

    # Detection markers from go test output:
    #   "ok "       at start of a line — suite ran, no fails
    #   "FAIL "     at start of a line — suite has fails
    #   "--- SKIP:" tests that called t.Skip
    #   "--- PASS:" tests that actually executed and passed
    $okLine    = $output | Select-String -Pattern "^ok\s+"     | Select-Object -First 1
    $failLine  = $output | Select-String -Pattern "^FAIL\s+"   | Select-Object -First 1
    $skipLines = @($output | Select-String -Pattern "--- SKIP:")
    $passLines = @($output | Select-String -Pattern "--- PASS:")

    $detail = ("{0:N1}s" -f $elapsed.TotalSeconds)

    if ($exitCode -ne 0 -or $failLine) {
        Write-Fail $short $detail
        $script:failures += $short
        if (-not $Loud) {
            $output | Where-Object { $_ -match "FAIL|--- FAIL|panic|Error|error:" } |
                Select-Object -First 20 |
                ForEach-Object { Write-Host ("    | " + $_) -ForegroundColor DarkRed }
        }
        return
    }

    # `ok` line means go test loop exited cleanly.  Distinguish three cases:
    #   1. PASS lines > 0 AND skip lines == 0 → all tests ran
    #   2. PASS lines > 0 AND skip lines > 0  → mixed (still PASS, some skipped)
    #   3. PASS lines == 0 AND skip lines > 0 → EVERY test skipped (suite SKIP)
    #   4. PASS == 0 AND SKIP == 0            → empty suite (rare; treat PASS)
    if ($passLines.Count -eq 0 -and $skipLines.Count -gt 0) {
        Write-SuiteSkip $short ("all " + $skipLines.Count + " tests skipped")
        $script:skipped += $short
    } else {
        $skipNote = ""
        if ($skipLines.Count -gt 0) { $skipNote = " (" + $skipLines.Count + " skipped)" }
        Write-Pass $short ($detail + $skipNote)
        $script:passed += $short
    }
}

# ---- Stage 1a: Go unit tests ------------------------------------------

Write-Title "Go unit tests"
$unitSuites = Get-GoTestSuites "tests/host/go_unit"
if ($unitSuites.Count -eq 0) {
    Write-Host "  (no suites discovered under tests/host/go_unit/)" -ForegroundColor DarkYellow
} else {
    foreach ($d in $unitSuites) {
        Invoke-GoSuite $d
    }
}

# ---- Stage 1b: Native C++ unit tests ----------------------------------

Write-Title "Native C++ unit tests"
$nativeBuild = Join-Path $repoRoot "tests/native/build.ps1"
if (-not (Test-Path $nativeBuild)) {
    Write-Host "  (tests/native/build.ps1 not present - skipping)" -ForegroundColor DarkYellow
} else {
    $start = Get-Date
    # Invoke directly — .ps1 runs under whichever PowerShell host we're
    # already in (5.1 / 7+), avoiding `pwsh` not-on-PATH issues on
    # Windows boxes that only have the legacy 5.1 host.
    $nativeOut = & $nativeBuild 2>&1
    $exitCode = $LASTEXITCODE
    $elapsed = (Get-Date) - $start
    $detail = ("{0:N1}s" -f $elapsed.TotalSeconds)

    # doctest summary line: "[doctest] test cases: N | N passed | N failed | N skipped"
    $summary = $nativeOut | Select-String -Pattern "test cases:" | Select-Object -First 1
    $detailExtra = ""
    if ($summary -and $summary.Line -match "(\d+)\s+passed\s*\|\s*(\d+)\s+failed") {
        $detailExtra = (" (" + $matches[1] + " cases)")
    }

    if ($exitCode -eq 0) {
        Write-Pass "native/doctest" ($detail + $detailExtra)
        $passed += "native/doctest"
    } else {
        Write-Fail "native/doctest" ($detail + $detailExtra)
        $failures += "native/doctest"
        if (-not $Loud) {
            # Echo doctest failure block.
            $nativeOut | Where-Object { $_ -match "ERROR|FAIL|Status: FAILURE|TEST CASE|BUILD FAILED" } |
                Select-Object -First 30 |
                ForEach-Object { Write-Host ("    | " + $_) -ForegroundColor DarkRed }
        }
    }
}

# ---- Stage 2: Go integration tests ------------------------------------

if ($Integration) {
    Write-Title "Go integration tests"

    # Verify HW reachability up-front so Premerge can fail fast.
    $port = $env:SCALEFX_HUBFX_PORT
    $hwReachable = $false
    if ($port) {
        $hwReachable = $true
        Write-Host ("  (HubFX port from env: " + $port + ")") -ForegroundColor DarkGray
    } else {
        # Auto-detect via CH343 VID:PID 1A86:55D3 - mirror of
        # firmware.DetectESP32Port (the `.` in the pattern matches the
        # literal `&` between VID and PID in the InstanceId).
        $hubfxPattern = 'VID_1A86.PID_55D3'
        $ch343 = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                 Where-Object { $_.InstanceId -match $hubfxPattern } |
                 Select-Object -First 1
        if ($ch343) {
            $hwReachable = $true
            Write-Host ("  (HubFX detected via CH343: " + $ch343.FriendlyName + ")") -ForegroundColor DarkGray
        }
    }

    if (-not $hwReachable) {
        if ($Premerge) {
            Write-Host "  no HubFX reachable - -Premerge requires hardware" -ForegroundColor Red
            $failures += "integration: no HubFX reachable"
        } else {
            Write-Host "  no HubFX reachable; skipping integration tier (use -Premerge to fail on this)" -ForegroundColor Yellow
            $skipped += "integration: no HubFX reachable"
        }
    } else {
        $integSuites = Get-GoTestSuites "tests/host/go_integration"
        if ($integSuites.Count -eq 0) {
            Write-Host "  (no suites discovered under tests/host/go_integration/)" -ForegroundColor DarkYellow
        } else {
            # Each suite is a separate process that opens its own connection to
            # the HubFX.  The host link is a CH343 USB-UART: back-to-back port
            # open/close cycles degrade the driver/link, so by the last suite a
            # sustained transfer (the 1.4 MB stream-upload) can stall mid-flight
            # even though the connect itself succeeds (the client now retries
            # IDENTIFY).  A short settle between suites lets the link fully
            # recover, so the gate reflects firmware health, not link wear.
            $first = $true
            foreach ($d in $integSuites) {
                if (-not $first) { Start-Sleep -Seconds 2 }
                $first = $false
                Invoke-GoSuite $d -NoCache -Integration
            }
        }
    }
}

# ---- Stage 3: Firmware build (Premerge only) --------------------------

if ($Premerge) {
    Write-Title "Firmware builds"
    $start = Get-Date
    $flasher = Join-Path $repoRoot "app/go/scalefx-flash.exe"
    if (-not (Test-Path $flasher)) {
        Write-Fail "hubfx-esp32s3" "scalefx-flash.exe missing - 'go build ./app/go/flash' first"
        $failures += "firmware: flasher missing"
    } else {
        $output = & $flasher build hubfx --no-clean 2>&1
        $exitCode = $LASTEXITCODE
        $elapsed = (Get-Date) - $start
        $detail = ("{0:N1}s" -f $elapsed.TotalSeconds)
        if ($exitCode -eq 0) {
            Write-Pass "hubfx-esp32s3" $detail
            $passed += "firmware:hubfx"
        } else {
            Write-Fail "hubfx-esp32s3" $detail
            $failures += "firmware:hubfx"
            $output | Select-Object -Last 10 | ForEach-Object {
                Write-Host ("    | " + $_) -ForegroundColor DarkRed
            }
        }
    }
}

# ---- Summary ----------------------------------------------------------

Write-Title "Summary"
Write-Host ("  passed:  {0}" -f $passed.Count) -ForegroundColor Green
if ($skipped.Count -gt 0) {
    Write-Host ("  skipped: {0}" -f $skipped.Count) -ForegroundColor Yellow
}
if ($failures.Count -gt 0) {
    Write-Host ("  FAILED:  {0}" -f $failures.Count) -ForegroundColor Red
    Write-Host ""
    Write-Host "Failed suites:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host ("    - " + $f) -ForegroundColor Red
    }
}
Write-Host ""

if ($failures.Count -eq 0) {
    if ($Premerge) {
        Write-Host "READY TO MERGE" -ForegroundColor Green
    } else {
        Write-Host "OK" -ForegroundColor Green
    }
    exit 0
} else {
    if ($Premerge) {
        Write-Host "DO NOT MERGE" -ForegroundColor Red
    } else {
        Write-Host "FAILED" -ForegroundColor Red
    }
    exit 1
}
