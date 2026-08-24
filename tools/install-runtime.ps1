# Installs the RTX Remix runtime into the SR3 game directory.
# Default: downloads the latest release from GitHub. Offline: pass -ZipPath to a release zip
# downloaded manually from https://github.com/NVIDIAGameWorks/rtx-remix/releases
param(
    [string]$ZipPath
)
$ErrorActionPreference = 'Stop'

$root    = Split-Path $PSScriptRoot -Parent
$gameDir = Join-Path $root 'Saints Row 3'
$runtimeStore = Join-Path $root 'runtime'

if (-not (Test-Path (Join-Path $gameDir 'SaintsRowTheThird.exe'))) {
    throw "Game exe not found in '$gameDir'"
}
New-Item -ItemType Directory -Force $runtimeStore | Out-Null

if (-not $ZipPath) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Host 'Querying latest RTX Remix release...'
    $rel = Invoke-RestMethod 'https://api.github.com/repos/NVIDIAGameWorks/rtx-remix/releases/latest'
    # Skip debug/symbols variants; want the plain runtime zip.
    $asset = $rel.assets | Where-Object { $_.name -match '\.zip$' -and $_.name -notmatch 'debug|symbols' } | Select-Object -First 1
    if (-not $asset) { throw "No suitable zip asset found in release $($rel.tag_name). Assets: $($rel.assets.name -join ', ')" }
    Write-Host "Downloading $($asset.name) ($($rel.tag_name))..."
    $ZipPath = Join-Path $runtimeStore $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $ZipPath
}

$extractDir = Join-Path $runtimeStore ([IO.Path]::GetFileNameWithoutExtension($ZipPath))
if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
Expand-Archive $ZipPath -DestinationPath $extractDir

# Release zips sometimes nest everything in a single top-level folder - flatten that.
$payload = $extractDir
$top = Get-ChildItem $extractDir
if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $payload = $top[0].FullName }

if (-not (Test-Path (Join-Path $payload 'd3d9.dll'))) {
    throw "d3d9.dll not found in extracted runtime at '$payload' - wrong zip? (debug/symbols package?)"
}

Write-Host "Installing runtime from '$payload' into game dir..."
Copy-Item (Join-Path $payload '*') $gameDir -Recurse -Force

Write-Host 'Done. Runtime files (d3d9.dll + .trex\) are now beside SaintsRowTheThird.exe.'
Write-Host 'Next: tools\deploy-conf.ps1, then tools\launch.ps1'
