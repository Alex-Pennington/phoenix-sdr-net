#!/usr/bin/env pwsh
# Phoenix SDR Net - Local Build and Release Deployment
# Builds with real SDRplay API and uploads to GitHub releases

param(
    [switch]$IncrementMajor,
    [switch]$IncrementMinor,
    [switch]$IncrementPatch,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

Write-Host "=== Phoenix SDR Net - Release Deployment ===" -ForegroundColor Cyan

# Paths
$projectRoot = $PSScriptRoot
$versionFile = Join-Path (Join-Path $projectRoot "include") "version.h"

if (-not (Test-Path $versionFile)) {
    Write-Error "version.h not found at: $versionFile"
    exit 1
}

# Read current version from version.h
$content = Get-Content $versionFile -Raw
if ($content -match '#define\s+PHOENIX_VERSION_MAJOR\s+(\d+)') { $major = [int]$matches[1] } else { Write-Error "Cannot parse MAJOR"; exit 1 }
if ($content -match '#define\s+PHOENIX_VERSION_MINOR\s+(\d+)') { $minor = [int]$matches[1] } else { Write-Error "Cannot parse MINOR"; exit 1 }
if ($content -match '#define\s+PHOENIX_VERSION_PATCH\s+(\d+)') { $patch = [int]$matches[1] } else { Write-Error "Cannot parse PATCH"; exit 1 }
if ($content -match '#define\s+PHOENIX_VERSION_BUILD\s+(\d+)') { $build = [int]$matches[1] } else { Write-Error "Cannot parse BUILD"; exit 1 }

Write-Host "Current version: $major.$minor.$patch+$build" -ForegroundColor Yellow

# Increment version based on flags
if ($IncrementMajor) {
    $major++
    $minor = 0
    $patch = 0
    Write-Host "Incrementing MAJOR -> $major.$minor.$patch" -ForegroundColor Green
}
elseif ($IncrementMinor) {
    $minor++
    $patch = 0
    Write-Host "Incrementing MINOR -> $major.$minor.$patch" -ForegroundColor Green
}
elseif ($IncrementPatch) {
    $patch++
    Write-Host "Incrementing PATCH -> $major.$minor.$patch" -ForegroundColor Green
}

# Always increment build
$build++

# Get git commit hash
Push-Location $projectRoot
$gitCommit = (git rev-parse --short HEAD).Trim()
$gitDirty = git diff-index --quiet HEAD
$dirtyStr = if ($LASTEXITCODE -ne 0) { "-dirty"; $true } else { ""; $false }
Pop-Location

$versionString = "$major.$minor.$patch"
$versionFull = "$versionString+$build.$gitCommit$dirtyStr"
$tag = "v$versionString"

Write-Host "New version: $versionFull" -ForegroundColor Cyan
Write-Host "Git tag: $tag" -ForegroundColor Cyan

# Update version.h
$newContent = $content -replace '(#define\s+PHOENIX_VERSION_MAJOR\s+)\d+', "`${1}$major"
$newContent = $newContent -replace '(#define\s+PHOENIX_VERSION_MINOR\s+)\d+', "`${1}$minor"
$newContent = $newContent -replace '(#define\s+PHOENIX_VERSION_PATCH\s+)\d+', "`${1}$patch"
$newContent = $newContent -replace '(#define\s+PHOENIX_VERSION_BUILD\s+)\d+', "`${1}$build"
$newContent = $newContent -replace '(#define\s+PHOENIX_VERSION_STRING\s+")[\d\.]+(")' , "`${1}$versionString`${2}"
$newContent = $newContent -replace '(#define\s+PHOENIX_VERSION_FULL\s+")[^"]+(")' , "`${1}$versionFull`${2}"
$newContent = $newContent -replace '(#define\s+PHOENIX_GIT_COMMIT\s+")[^"]+(")' , "`${1}$gitCommit`${2}"
$newContent = $newContent -replace '(#define\s+PHOENIX_GIT_DIRTY\s+)(true|false)' , "`${1}$($dirtyStr -ne '')"

Set-Content $versionFile $newContent -NoNewline
Write-Host "Updated $versionFile" -ForegroundColor Green

# Clean and build
Write-Host "`nCleaning build directory..." -ForegroundColor Yellow
if (Test-Path "build") {
    Remove-Item -Recurse -Force "build"
}

Write-Host "Configuring CMake..." -ForegroundColor Yellow
cmake --preset msys2-ucrt64
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

Write-Host "Building..." -ForegroundColor Yellow
cmake --build --preset msys2-ucrt64
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

# Check that executables exist
$exePath1 = Join-Path (Join-Path (Join-Path $projectRoot "build") "msys2-ucrt64") "sdr_server.exe"
$exePath2 = Join-Path (Join-Path (Join-Path $projectRoot "build") "msys2-ucrt64") "telem_logger.exe"

if (-not (Test-Path $exePath1)) { Write-Error "sdr_server.exe not found"; exit 1 }
if (-not (Test-Path $exePath2)) { Write-Error "telem_logger.exe not found"; exit 1 }

Write-Host "`nBuild successful!" -ForegroundColor Green

# Create release archive
$zipName = "phoenix-sdr-net-windows-$versionString.zip"
$zipPath = Join-Path $projectRoot $zipName

if (Test-Path $zipPath) { Remove-Item $zipPath }

# Collect all files for the release
$buildDir = Join-Path (Join-Path $projectRoot "build") "msys2-ucrt64"
$releaseFiles = @(
    $exePath1,
    $exePath2,
    (Join-Path $buildDir "sdrplay_api.dll"),
    (Join-Path $projectRoot "README.md"),
    (Join-Path $projectRoot "LICENSE"),
    (Join-Path (Join-Path $projectRoot "docs") "IQ_STREAMING.md")
)

# Verify all files exist
foreach ($file in $releaseFiles) {
    if (-not (Test-Path $file)) {
        Write-Error "Required file not found: $file"
        exit 1
    }
}

Compress-Archive -Path $releaseFiles -DestinationPath $zipPath
Write-Host "Created $zipName" -ForegroundColor Green

if (-not $Deploy) {
    Write-Host "`n=== BUILD COMPLETE (NOT DEPLOYED) ===" -ForegroundColor Yellow
    Write-Host "Version: $versionFull" -ForegroundColor Cyan
    Write-Host "Archive: $zipPath" -ForegroundColor Cyan
    Write-Host "`nTo deploy to GitHub, run with -Deploy flag" -ForegroundColor Yellow
    exit 0
}

# Commit version.h changes
Write-Host "`nCommitting version update..." -ForegroundColor Yellow
Push-Location $projectRoot
git add $versionFile
git commit -m "Bump version to $versionFull"
git push origin main
Pop-Location

# Create/update git tag
Write-Host "Creating git tag $tag..." -ForegroundColor Yellow
Push-Location $projectRoot
$ErrorActionPreference = "Continue"
git tag -d $tag 2>&1 | Out-Null
git push origin ":refs/tags/$tag" 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
git tag $tag
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to create tag"; Pop-Location; exit 1 }
git push origin $tag
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to push tag"; Pop-Location; exit 1 }
Pop-Location

# Upload to GitHub release
Write-Host "`nUploading to GitHub release $tag..." -ForegroundColor Yellow
$null = gh release create $tag $zipPath --title "Phoenix SDR Net $versionString" --notes "Release $versionFull" --repo Alex-Pennington/phoenix-sdr-net 2>&1
if ($LASTEXITCODE -ne 0) {
    # Release might already exist, try uploading to existing
    gh release upload $tag $zipPath --clobber --repo Alex-Pennington/phoenix-sdr-net
    if ($LASTEXITCODE -ne 0) { Write-Error "Failed to upload release"; exit 1 }
}

Write-Host "`n=== DEPLOYMENT COMPLETE ===" -ForegroundColor Green
Write-Host "Version: $versionFull" -ForegroundColor Cyan
Write-Host "Tag: $tag" -ForegroundColor Cyan
Write-Host "Release: https://github.com/Alex-Pennington/phoenix-sdr-net/releases/tag/$tag" -ForegroundColor Cyan
