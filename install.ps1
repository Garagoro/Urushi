# URUSHI Installer / Deployer
param (
    [string]$GamePath = "C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=======================================" -ForegroundColor Cyan
Write-Host " URUSHI Installation Script            " -ForegroundColor Cyan
Write-Host "=======================================" -ForegroundColor Cyan

if (-not (Test-Path $GamePath)) {
    Write-Error "Game path does not exist: $GamePath"
}

$BuildDir = Join-Path $ScriptDir "build"
$VersionDll = Join-Path $BuildDir "proxy\Release\version.dll"
$CoreDll = Join-Path $BuildDir "core\Release\urushi.dll"
$ScriptsSource = Join-Path $ScriptDir "scripts"
$ShadersSource = Join-Path $ScriptDir "shaders"

if (-not (Test-Path $VersionDll) -or -not (Test-Path $CoreDll)) {
    Write-Host "Building project first..." -ForegroundColor Yellow
    $CMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    & $CMake --build $BuildDir --config Release
    if (Test-Path (Join-Path $BuildDir "core\Release\urushi.dll")) {
        $CoreDll = Join-Path $BuildDir "core\Release\urushi.dll"
    }
}

Write-Host "Copying version.dll (Proxy Loader)..." -ForegroundColor Green
Copy-Item -Path $VersionDll -Destination (Join-Path $GamePath "version.dll") -Force

Write-Host "Copying urushi.dll (Core Engine)..." -ForegroundColor Green
Copy-Item -Path $CoreDll -Destination (Join-Path $GamePath "urushi.dll") -Force

$DestScripts = Join-Path $GamePath "scripts"
Write-Host "Deploying scripts to $DestScripts..." -ForegroundColor Green
if (-not (Test-Path $DestScripts)) {
    New-Item -ItemType Directory -Path $DestScripts -Force | Out-Null
}
Copy-Item -Path (Join-Path $ScriptsSource "*") -Destination $DestScripts -Recurse -Force

$DestShaders = Join-Path $GamePath "shaders"
Write-Host "Deploying shaders to $DestShaders..." -ForegroundColor Green
if (-not (Test-Path $DestShaders)) {
    New-Item -ItemType Directory -Path $DestShaders -Force | Out-Null
}
if (Test-Path $ShadersSource) {
    Copy-Item -Path (Join-Path $ShadersSource "*") -Destination $DestShaders -Recurse -Force
}

$DestReplays = Join-Path $GamePath "replays"
if (-not (Test-Path $DestReplays)) {
    New-Item -ItemType Directory -Path $DestReplays -Force | Out-Null
}

Write-Host "`nInstallation completed successfully!" -ForegroundColor Green
Write-Host "Launch CarX Drift Racing Online 2 via Steam to play with the mod." -ForegroundColor Cyan
Write-Host "Press F4 in-game to open the Replay & Ghost Mod Menu." -ForegroundColor Yellow
