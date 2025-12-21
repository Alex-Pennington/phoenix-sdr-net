# test-ci-build.ps1 - Simulate GitHub Actions build locally
# Run from PowerShell: .\test-ci-build.ps1

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

Write-Host "=== Phoenix SDR Net - CI Build Test ===" -ForegroundColor Cyan
Write-Host ""

$FindModule = "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake"
$FindModuleBak = "$FindModule.bak"
$RestoreFind = $false

try {
    # Hide SDRplay API to simulate GH environment
    Write-Host "[1/5] Hiding SDRplay API finder..." -ForegroundColor Yellow
    if (Test-Path $FindModule) {
        Move-Item $FindModule $FindModuleBak -Force
        $RestoreFind = $true
    }

    # Clean build
    Write-Host "[2/5] Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build"
    }

    # Configure
    Write-Host "[3/5] Configuring (cmake --preset msys2-ucrt64)..." -ForegroundColor Yellow
    cmake --preset msys2-ucrt64
    if ($LASTEXITCODE -ne 0) { throw "Configure failed" }

    # Build
    Write-Host "[4/5] Building (cmake --build --preset msys2-ucrt64)..." -ForegroundColor Yellow
    cmake --build --preset msys2-ucrt64
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }

    Write-Host ""
    Write-Host "=== BUILD SUCCESSFUL ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "Outputs:"
    Get-ChildItem "build\msys2-ucrt64\*.exe" | ForEach-Object { Write-Host "  $_" }
}
catch {
    Write-Host ""
    Write-Host "=== BUILD FAILED ===" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
finally {
    # Always restore
    Write-Host "[5/5] Restoring SDRplay API finder..." -ForegroundColor Yellow
    if ($RestoreFind -and (Test-Path $FindModuleBak)) {
        Move-Item $FindModuleBak $FindModule -Force
    }
}
