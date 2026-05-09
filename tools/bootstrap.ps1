# Idempotent setup for a fresh checkout on Windows.
# Run from any directory; this script cd's to the repo root itself.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

New-Item -ItemType Directory -Force -Path external | Out-Null

if (-not (Test-Path 'external\vcpkg\.git')) {
    Write-Host '[1/3] Cloning vcpkg into external\vcpkg (full history — needed for versioning)...'
    git clone https://github.com/microsoft/vcpkg.git external\vcpkg
} else {
    Write-Host '[1/3] external\vcpkg already present — checking it has full history'
    $isShallow = git -C external\vcpkg rev-parse --is-shallow-repository
    if ($isShallow.Trim() -eq 'true') {
        Write-Host '       (shallow clone detected — fetching full history)'
        git -C external\vcpkg fetch --unshallow
    }
}

if (-not (Test-Path 'external\vcpkg\vcpkg.exe')) {
    Write-Host '[2/3] Bootstrapping vcpkg...'
    & external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
} else {
    Write-Host '[2/3] vcpkg binary already built — skipping'
}

$manifest = Get-Content vcpkg.json -Raw
if ($manifest -notmatch '"builtin-baseline"') {
    Write-Host '[3/3] Pinning vcpkg builtin-baseline for reproducibility...'
    & external\vcpkg\vcpkg.exe x-update-baseline --add-initial-baseline
} else {
    Write-Host '[3/3] builtin-baseline already pinned'
}

Write-Host ''
Write-Host '----------------------------------------------------------------'
Write-Host 'Setup complete. Next:'
Write-Host ''
Write-Host '    cmake --preset windows-release'
Write-Host '    cmake --build --preset windows-release'
Write-Host '    ctest --preset windows-release'
Write-Host ''
Write-Host 'CMakePresets.json points the toolchain at external\vcpkg, so no'
Write-Host 'VCPKG_ROOT environment variable is needed.'
Write-Host ''
Write-Host 'First configure will compile Qt from source via vcpkg — expect'
Write-Host '30-60 minutes the very first time. Subsequent configures are fast'
Write-Host "(seconds) thanks to vcpkg's binary cache."
Write-Host '----------------------------------------------------------------'
