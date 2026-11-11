# Makes the release zip: the source and the builder, without any game files.
#
#   powershell -ExecutionPolicy Bypass -File .\package.ps1 -Version 0.1.0
#
# Creates dist\VampireRainRecomp-<version>-source.zip. A built VampireRain.exe is
# never included: it contains the game's own code (recompiled), so everyone
# builds it from their own disc with build.ps1.
param([string]$Version = "0.1.0")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "VampireRainRecomp-$Version-source"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# What goes in (paths relative to the repository folder).
$items = @(
    "README.md", "LICENSE", "build.ps1", "package.ps1", ".gitignore",
    "config", "patches", "tools\vr_tool",
    "runtime\CMakeLists.txt", "runtime\README.md", "runtime\src", "runtime\third_party", "runtime\tools"
)
foreach ($item in $items) {
    $src = Join-Path $root $item
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $stage $item
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    Copy-Item $src $dst -Recurse -Force
}

# Safety check: nothing from the game, no built programs, no logs or dumps.
$blocked = ".xex", ".iso", ".exe", ".dll", ".pdb", ".dds", ".png", ".ppm", ".log", ".cso", ".bin", ".vrs", ".vrp"
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $blocked -contains $_.Extension.ToLower() -or $_.Length -gt 2MB }
if ($bad) {
    throw "Not packaging, these files don't belong in a source release:`n  " + (($bad | ForEach-Object { $_.FullName }) -join "`n  ")
}

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Write-Host ("Made {0} ({1:N0} KB)" -f $zip, ((Get-Item $zip).Length / 1KB))
