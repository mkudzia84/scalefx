# sync-presets.ps1 — copy /media/presets/ → /app/go/studio/assets/presets/
#
# Go's //go:embed directive can't escape its package directory, so the
# Studio binary embeds files from app/go/studio/assets/.  This script
# keeps that mirror in sync with the canonical content in media/.
#
# Safe to re-run; identical files are skipped.  Run after adding or
# editing anything under /media/presets/.

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '../../..')
$source   = Join-Path $repoRoot 'media\presets'
$dest     = Join-Path $PSScriptRoot 'assets\presets'

if (-not (Test-Path $source)) {
    Write-Error "Source not found: $source"
    exit 1
}

if (-not (Test-Path $dest)) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
}

# robocopy returns 1 on "files copied" (success), 0 on "no change".
# /MIR mirrors the tree (deletes orphans in dest that aren't in source).
# /XF excludes README.md — the studio side has its own.
robocopy $source $dest /MIR /NJH /NJS /NDL /NP /XF README.md | Out-Null

$rc = $LASTEXITCODE
if ($rc -ge 8) {
    Write-Error "robocopy failed (exit $rc)"
    exit 1
}

Write-Host "Synced /media/presets/ -> $dest (robocopy exit $rc)"
