<#
Vampire Rain (Xbox 360) - PC builder

Builds the PC version from your own copy of the game disc. Nothing from the
game is included in this repository: the game's executable and data are read
from your disc image, recompiled and extracted on your computer.

Usage (PowerShell, from this folder):
    powershell -ExecutionPolicy Bypass -File .\build.ps1
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -Iso "D:\Games\Vampire Rain (USA, Europe).iso"

Options:
    -Iso <path>      Disc image to use. Without it, a single .iso in this folder
                     is used, or you are asked to pick one.
    -Arch <level>    CPU level for the game code: x86-64-v3 (default, AVX2) or
                     x86-64-v2 for CPUs without AVX2.
    -Force           Rebuild every step even if its output already exists.
    -NoBuild         Prepare the game files and generated code, but don't
                     compile the runtime.

Needs: Windows 10/11 x64, Git, and Visual Studio 2022 (or Build Tools) with
"Desktop development with C++" and "C++ Clang tools for Windows".
#>
param(
    [string]$Iso,
    [ValidateSet("x86-64-v2", "x86-64-v3")][string]$Arch = "x86-64-v3",
    [switch]$Force,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------
# The game build this project supports (Vampire Rain, USA/Europe disc).
$ExpectedXexSha1 = "9c88712627ff06629f061ac01271d0320d336961"
$GameDirName     = "Vampire Rain (USA, Europe)"

# Third-party source code fetched at build time, at the commits the project
# was built and tested with.
$Deps = @(
    @{ Name = "XenonRecomp"; Url = "https://github.com/hedge-dev/XenonRecomp.git"; Ref = "ddd128bcca99fe8bfbb99bea583c972351fa6ace"; Submodules = $true },
    @{ Name = "FFmpeg";      Url = "https://github.com/xenia-project/FFmpeg.git";  Ref = "15ece0882e8d5875051ff5b73c5a8326f7cee9f5"; Submodules = $false }
)

$DepsDir    = Join-Path $Root "deps"
$ToolsBuild = Join-Path $Root "tools\build"
$GameDir    = Join-Path $Root $GameDirName
$WorkDir    = Join-Path $Root "work"
$RecompDir  = Join-Path $Root "recomp_out"
$RuntimeDir = Join-Path $Root "runtime"
$RuntimeBuild = Join-Path $RuntimeDir "build"

$script:Step = 0
$StepCount = 8
function Step($text) {
    $script:Step++
    Write-Host ""
    Write-Host ("[{0}/{1}] {2}" -f $script:Step, $StepCount, $text) -ForegroundColor Cyan
}
function Info($text) { Write-Host "      $text" }
function Fail($text) {
    Write-Host ""
    Write-Host "ERROR: $text" -ForegroundColor Red
    exit 1
}
# Runs a program and returns its exit code, hiding its output. (Windows
# PowerShell turns a program's error output into script errors otherwise.)
function Quiet($exe, [string[]]$arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @arguments 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
    return $LASTEXITCODE
}
function Run($exe, [string[]]$arguments, $what) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE). See the messages above." }
}

Write-Host "VampireRainRecomp builder" -ForegroundColor White

# ---------------------------------------------------------------------------
# 1. Build tools
# ---------------------------------------------------------------------------
Step "Checking build tools"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "Git was not found. Install Git for Windows (https://git-scm.com/download/win) and run this again."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio was not found. Install Visual Studio 2022 (Community is free) or the Build Tools with 'Desktop development with C++' and 'C++ Clang tools for Windows'."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) { Fail "Visual Studio is installed without the C++ tools. Add 'Desktop development with C++' in the Visual Studio Installer." }
$clangDir = Join-Path $vsPath "VC\Tools\Llvm\x64\bin"
if (-not (Test-Path (Join-Path $clangDir "clang-cl.exe"))) {
    Fail "clang-cl was not found. In the Visual Studio Installer, modify your installation and add 'C++ Clang tools for Windows'."
}

# Load the Visual Studio x64 build environment (compiler, Windows SDK, CMake, Ninja).
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "call `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}
$env:PATH = "$clangDir;$env:PATH"
foreach ($tool in "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool was not found. Add 'C++ CMake tools for Windows' in the Visual Studio Installer, or install $tool and put it on PATH."
    }
}
Info "Visual Studio: $vsPath"
Info "clang-cl:      $((& clang-cl --version | Select-Object -First 1))"

$CMakeCommon = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")

# ---------------------------------------------------------------------------
# 2. Disc image
# ---------------------------------------------------------------------------
Step "Finding the disc image"

$haveGame = Test-Path (Join-Path $GameDir "default.xex")
if (-not $Iso -and (-not $haveGame -or $Force)) {
    $found = @(Get-ChildItem -Path $Root -Filter "*.iso" -File -ErrorAction SilentlyContinue)
    if ($found.Count -eq 1) {
        $Iso = $found[0].FullName
    } else {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select your Vampire Rain disc image"
        $dlg.Filter = "Xbox 360 disc image (*.iso)|*.iso|All files (*.*)|*.*"
        $dlg.InitialDirectory = $Root
        if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "No disc image selected." }
        $Iso = $dlg.FileName
    }
}
if ($Iso) {
    if (-not (Test-Path $Iso)) { Fail "Disc image not found: $Iso" }
    Info "Using $Iso"
} else {
    Info "Game files already extracted in '$GameDirName' (use -Force to extract again)"
}

# ---------------------------------------------------------------------------
# 3. Third-party source
# ---------------------------------------------------------------------------
Step "Fetching XenonRecomp and FFmpeg source"

New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null
foreach ($d in $Deps) {
    $dir = Join-Path $DepsDir $d.Name
    $current = ""
    if (Test-Path (Join-Path $dir ".git")) {
        $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        $current = (& git -C $dir rev-parse HEAD 2>$null | Out-String).Trim()
        $ErrorActionPreference = $old
    }
    if ($current -ne $d.Ref) {
        # Fetch just that commit (no history): plain line endings, so the
        # patch below applies the same way on every machine.
        Info "Downloading $($d.Name) ($($d.Ref.Substring(0, 10)))"
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Run git @("-C", $dir, "init", "--quiet") "git init"
        Run git @("-C", $dir, "config", "core.autocrlf", "false") "git config"
        Run git @("-C", $dir, "remote", "add", "origin", $d.Url) "git remote"
        Run git @("-C", $dir, "fetch", "--quiet", "--depth", "1", "origin", $d.Ref) "Downloading $($d.Name)"
        Run git @("-C", $dir, "checkout", "--quiet", "FETCH_HEAD") "Checking out $($d.Name)"
        if ($d.Submodules) {
            Run git @("-C", $dir, "-c", "core.autocrlf=false", "submodule", "update", "--init", "--recursive", "--depth", "1") "Downloading $($d.Name) submodules"
        }
    } else {
        Info "$($d.Name) already downloaded"
    }
}

# XenonRecomp needs a few extra instructions and fixes for this game.
$XenonRecompDir = Join-Path $DepsDir "XenonRecomp"
$patch = Join-Path $Root "patches\xenonrecomp.patch"
if ((Quiet git @("-C", $XenonRecompDir, "apply", "--check", "--reverse", "--ignore-whitespace", $patch)) -eq 0) {
    Info "XenonRecomp patch already applied"
} else {
    if ((Quiet git @("-C", $XenonRecompDir, "apply", "--check", "--ignore-whitespace", $patch)) -ne 0) {
        Fail "patches\xenonrecomp.patch doesn't apply to deps\XenonRecomp. Delete the deps folder and run this again."
    }
    Run git @("-C", $XenonRecompDir, "apply", "--ignore-whitespace", "--whitespace=nowarn", $patch) "Patching XenonRecomp"
    Info "Patched XenonRecomp"
}

# ---------------------------------------------------------------------------
# 4. Recompiler and setup tools
# ---------------------------------------------------------------------------
Step "Building XenonRecomp and the setup tool"

$xrBuild = Join-Path $XenonRecompDir "build"
$xrExe = Get-ChildItem -Path $xrBuild -Recurse -Filter "XenonRecomp.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $xrExe -or $Force) {
    Run cmake (@("-S", $XenonRecompDir, "-B", $xrBuild) + $CMakeCommon) "Configuring XenonRecomp"
    Run cmake @("--build", $xrBuild, "--target", "XenonRecomp") "Building XenonRecomp"
    $xrExe = Get-ChildItem -Path $xrBuild -Recurse -Filter "XenonRecomp.exe" | Select-Object -First 1
}
if (-not $xrExe) { Fail "XenonRecomp.exe was not produced." }
Info "XenonRecomp: $($xrExe.FullName)"

$vrTool = Join-Path $ToolsBuild "vr_tool.exe"
if (-not (Test-Path $vrTool) -or $Force) {
    Run cmake (@("-S", (Join-Path $Root "tools\vr_tool"), "-B", $ToolsBuild, "-DXENONRECOMP_DIR=$XenonRecompDir") + $CMakeCommon) "Configuring vr_tool"
    Run cmake @("--build", $ToolsBuild) "Building vr_tool"
}

# ---------------------------------------------------------------------------
# 5. Game files
# ---------------------------------------------------------------------------
Step "Extracting the game files"

if ($Iso -and (-not $haveGame -or $Force)) {
    if (Test-Path $GameDir) { Remove-Item -Recurse -Force $GameDir }
    Run $vrTool @("extract", $Iso, $GameDir) "Extracting the disc image"
}
$xex = Join-Path $GameDir "default.xex"
if (-not (Test-Path $xex)) { Fail "default.xex is missing from '$GameDirName' - is this a Vampire Rain disc image?" }

$sha1 = (& $vrTool sha1 $xex).Trim()
if ($sha1 -ne $ExpectedXexSha1) {
    Fail "This disc's default.xex ($sha1) isn't the supported version of Vampire Rain (expected $ExpectedXexSha1, the USA/Europe release). The recompiled code would not match it."
}
Info "default.xex matches the supported version"

# ---------------------------------------------------------------------------
# 6. Executable
# ---------------------------------------------------------------------------
Step "Decrypting the game executable"

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$xexDec = Join-Path $WorkDir "default_dec.xex"
if (-not (Test-Path $xexDec) -or $Force -or (Get-Item $xexDec).LastWriteTime -lt (Get-Item $xex).LastWriteTime) {
    Run $vrTool @("decrypt", $xex, $xexDec) "Decrypting default.xex"
}
# The game's icon, for the PC version's window and exe (optional).
$icon = Join-Path $WorkDir "icon.ico"
if (-not (Test-Path $icon) -or $Force) {
    if ((Quiet $vrTool @("icon", $xex, $icon)) -ne 0) { Info "(no icon found in the executable - the game will use the default one)" }
}
Copy-Item (Join-Path $Root "config\vampirerain.toml") $WorkDir -Force
Copy-Item (Join-Path $Root "config\switch_tables.toml") $WorkDir -Force

# ---------------------------------------------------------------------------
# 7. Recompile
# ---------------------------------------------------------------------------
Step "Recompiling the game code (PowerPC -> C++)"

$stamp = Join-Path $RecompDir ".inputs"
$inputs = @($xexDec, (Join-Path $WorkDir "vampirerain.toml"), (Join-Path $WorkDir "switch_tables.toml"), $patch, $xrExe.FullName) |
    ForEach-Object { (Get-FileHash $_ -Algorithm SHA1).Hash }
$inputsText = $inputs -join "`n"
$upToDate = (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $inputsText.Trim()) -and (Test-Path (Join-Path $RecompDir "ppc_recomp_shared.h"))
if ($upToDate -and -not $Force) {
    Info "Generated code is up to date"
} else {
    New-Item -ItemType Directory -Force -Path $RecompDir | Out-Null  # XenonRecomp doesn't create it
    Run $xrExe.FullName @((Join-Path $WorkDir "vampirerain.toml"), (Join-Path $XenonRecompDir "XenonUtils\ppc_context.h")) "XenonRecomp"
    Set-Content -Path $stamp -Value $inputsText
}

# ---------------------------------------------------------------------------
# 8. Runtime
# ---------------------------------------------------------------------------
Step "Building the game (this takes a while the first time)"

if ($NoBuild) {
    Info "Skipped (-NoBuild)"
} else {
    $cmakeArgs = @("-S", $RuntimeDir, "-B", $RuntimeBuild) + $CMakeCommon + @(
        "-DRECOMP_DIR=$RecompDir",
        "-DXENONRECOMP_DIR=$XenonRecompDir",
        "-DFFMPEG_DIR=$(Join-Path $DepsDir 'FFmpeg')",
        "-DVR_ARCH=$Arch")
    Run cmake $cmakeArgs "Configuring the runtime"
    Run cmake @("--build", $RuntimeBuild) "Building the runtime"

    # (The build also copies VampireRain.exe into this folder.)
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
if (-not $NoBuild) {
    Write-Host "Start the game with VampireRain.exe in this folder."
    Write-Host "Saves and settings are kept in the 'userdata' folder."
}
