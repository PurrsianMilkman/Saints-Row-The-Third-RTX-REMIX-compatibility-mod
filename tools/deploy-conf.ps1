# Pushes configs\ into the game directory. Backs up originals once (*.pre-remix).
$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$gameDir = Join-Path $root 'Saints Row 3'
$configs = Join-Path $root 'configs'

$displayTarget = Join-Path $gameDir 'display.ini'
$displayBackup = "$displayTarget.pre-remix"
if ((Test-Path $displayTarget) -and -not (Test-Path $displayBackup)) {
    Copy-Item $displayTarget $displayBackup
    Write-Host "Backed up display.ini -> display.ini.pre-remix"
}
Copy-Item (Join-Path $configs 'display.remix.ini') $displayTarget -Force
Write-Host 'Deployed display.remix.ini -> display.ini'

Copy-Item (Join-Path $configs 'rtx.conf') (Join-Path $gameDir 'rtx.conf') -Force
Write-Host 'Deployed rtx.conf'
