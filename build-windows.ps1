param(
    [ValidateSet("windows-vs2026", "windows-vs2022")]
    [string]$Preset = "windows-vs2026",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

function Find-VsCMake {
    # Use the CMake that ships with Visual Studio 2026, exactly like the
    # Developer PowerShell / "Open Folder" integration does.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return "cmake" }
    $install = & $vswhere -latest -version "[18.0,19.0)" `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $install) { return "cmake" }
    $candidate = Join-Path $install "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $candidate) { return $candidate }
    return "cmake"
}

$cmake = Find-VsCMake
$config = $Configuration.ToLowerInvariant()

$ctest = Join-Path (Split-Path $cmake) "ctest.exe"
if (-not (Test-Path $ctest)) { $ctest = "ctest" }

Write-Host "Using CMake: $cmake"
& $cmake --version
if ($LASTEXITCODE -ne 0) { throw "CMake not found" }

Write-Host "[1/3] Configure: $Preset"
& $cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "[2/3] Build: $Preset-$config"
& $cmake --build --preset "$Preset-$config"
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

Write-Host "[3/3] Test"
& $cmake --build --preset "$Preset-$config" --target oldbox_tests
if ($LASTEXITCODE -ne 0) { throw "Test build failed" }
& $ctest --preset "$Preset-$config" --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "CTest failed" }

$exeDir = Join-Path (Join-Path (Join-Path (Get-Location) "build") ($Preset -replace "windows-", "")) $Configuration
Write-Host "Build finished. Executables: $exeDir"
