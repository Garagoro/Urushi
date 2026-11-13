param(
    [string]$Version = "0.1.0",
    [string]$Notes = "",
    [switch]$Build,
    [switch]$Push
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$distDir = Join-Path $root "dist"
New-Item -ItemType Directory -Force $distDir | Out-Null

if ($Build) {
    $msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
    if (-not (Test-Path $msbuild)) {
        throw "MSBuild not found at $msbuild"
    }
    & $msbuild "$root\build\ALL_BUILD.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Release build failed" }
}

$coreDll = Join-Path $root "build\core\Release\urushi.dll"
$proxyDll = Join-Path $root "build\proxy\Release\version.dll"
if (-not (Test-Path $coreDll)) { throw "Missing $coreDll" }
if (-not (Test-Path $proxyDll)) { throw "Missing $proxyDll" }

Copy-Item $coreDll (Join-Path $distDir "urushi.dll") -Force
Copy-Item $proxyDll (Join-Path $distDir "version.dll") -Force

# Read PE TimeDateStamp from dist/urushi.dll
$bytes = [System.IO.File]::ReadAllBytes((Join-Path $distDir "urushi.dll"))
$peOff = [BitConverter]::ToInt32($bytes, 60)
$linkTime = [BitConverter]::ToUInt32($bytes, $peOff + 8)

$manifest = @(
    "version=$Version"
    "link_time=$linkTime"
    "notes=$Notes"
) -join "`n"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $distDir "version.txt"), "$manifest`n", $utf8NoBom)

Write-Output "Staged dist/ for URUSHI v$Version (link_time=$linkTime)"

if ($Push) {
    git -C $root add -A
    git -C $root commit -m "Release v$Version"
    git -C $root push origin main
}
