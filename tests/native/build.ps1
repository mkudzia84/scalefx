#!/usr/bin/env pwsh
# tests/native/build.ps1 - compile and run all C++ native unit tests.
#
# Discovers every src/test_*.cpp file, compiles them together with the
# firmware sources they exercise (LibSources below) into a single
# binary, runs it, and exits with the binary's exit code.  Uses g++
# directly (no PlatformIO toolchain hop) so it runs anywhere g++ /
# clang++ is on PATH.
#
# Run:
#   ./tests/native/build.ps1
#
# Hooks into Rule 52's pre-merge gate via tools/run-tests.ps1.

param(
    [switch] $Loud
)

$here = $PSScriptRoot
$repoRoot = Resolve-Path (Join-Path $here "../..")
Set-Location $here

# ---- Source manifest --------------------------------------------------
#
# Test entry points (auto-discovered):
$TestSources = Get-ChildItem -Path "src" -Filter "test_*.cpp" | ForEach-Object { $_.FullName }

# Firmware sources from controllers/lib/* that the tests exercise.
# Add new entries when a new test pulls in a new .cpp.  Header-only
# targets (motion_profile.h, element_scaling.h) don't appear here.
$LibSources = @(
    "$repoRoot/controllers/lib/sfx_serial/serial/wire.cpp"
)

# Include directories — controllers/lib/* is laid out so each subsystem
# has its OWN top-level dir, then a subdir mirroring the namespace path.
# Tests #include "wire.h" (relative to its parent dir), so add each
# leaf dir we test from.
#
# `stubs/` MUST come first so its Arduino.h and platform/sfx_platform.h
# shadow the real ones (which #error out for non-MCU targets).
$IncludeDirs = @(
    "$here/stubs",
    "$here/lib/doctest",
    "$here/src",
    "$repoRoot/controllers/lib/sfx_serial",
    "$repoRoot/controllers/lib/sfx_serial/serial",
    "$repoRoot/controllers/lib/sfx_board/motion",
    "$repoRoot/controllers/lib/sfx_board/element",
    "$repoRoot/controllers/lib/sfx_platform/platform",
    "$repoRoot/controllers/lib/sfx_config/config"
)

# ---- Toolchain --------------------------------------------------------

$cxx = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $cxx) {
    $cxx = Get-Command clang++ -ErrorAction SilentlyContinue
}
if (-not $cxx) {
    Write-Host "ERROR: no g++ or clang++ on PATH." -ForegroundColor Red
    Write-Host "  Install MinGW-w64 (winget install MSYS2.MSYS2 or BrechtSanders.WinLibs.POSIX.UCRT.LLVM)" -ForegroundColor DarkYellow
    exit 2
}

# ---- Build ------------------------------------------------------------

$out = Join-Path $here ".tmp"
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }
$exe = Join-Path $out "tests_native.exe"

$flags = @(
    "-std=gnu++20",
    "-Wall",
    "-O0",
    "-g"
)
foreach ($d in $IncludeDirs) { $flags += "-I$d" }

$all = @()
$all += $TestSources
$all += $LibSources

if ($Loud) {
    Write-Host ("compiler: " + $cxx.Source) -ForegroundColor DarkGray
    Write-Host ("flags:    " + ($flags -join " ")) -ForegroundColor DarkGray
    Write-Host ("sources:") -ForegroundColor DarkGray
    foreach ($s in $all) { Write-Host ("  " + (Resolve-Path $s -Relative).Replace("\", "/")) -ForegroundColor DarkGray }
}

$start = Get-Date
$buildArgs = $flags + $all + @("-o", $exe)
& $cxx.Source @buildArgs 2>&1 | Tee-Object -Variable buildOut | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED" -ForegroundColor Red
    $buildOut | ForEach-Object { Write-Host ("  | " + $_) -ForegroundColor DarkRed }
    exit 1
}
$buildElapsed = ((Get-Date) - $start).TotalSeconds

# ---- Run --------------------------------------------------------------

if ($Loud) {
    Write-Host ("built " + (Split-Path $exe -Leaf) + " in {0:N1}s" -f $buildElapsed) -ForegroundColor DarkGray
}

$runStart = Get-Date
# Run with the COMPILER's bin dir first on PATH so the exe loads the SAME
# MinGW runtime DLLs (libstdc++-6 / libgcc_s_seh-1 / libwinpthread-1) it was
# linked against. Without this, a different libstdc++ found earlier on PATH
# triggers STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) — the exe builds fine but
# crashes at load. (2026-06-06: this masked an otherwise-green native suite.)
$cxxBin = Split-Path $cxx.Source -Parent
$savedPath = $env:PATH
$env:PATH = $cxxBin + [IO.Path]::PathSeparator + $env:PATH
try {
    & $exe 2>&1 | Tee-Object -Variable runOut
    $runExit = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
}
$runElapsed = ((Get-Date) - $runStart).TotalSeconds

if ($runExit -eq 0) {
    Write-Host ("PASS  (build {0:N1}s, run {1:N1}s)" -f $buildElapsed, $runElapsed) -ForegroundColor Green
} else {
    Write-Host ("FAIL  (build {0:N1}s, run {1:N1}s, exit {2})" -f $buildElapsed, $runElapsed, $runExit) -ForegroundColor Red
}
exit $runExit
