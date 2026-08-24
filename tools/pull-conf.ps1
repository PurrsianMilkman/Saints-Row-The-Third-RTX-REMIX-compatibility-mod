# Pulls the in-game-tuned rtx.conf back into configs\ so tuning survives redeploys.
$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root 'Saints Row 3\rtx.conf'
if (-not (Test-Path $src)) { throw "No rtx.conf in game dir yet." }
Copy-Item $src (Join-Path $root 'configs\rtx.conf') -Force
Write-Host 'Pulled game-dir rtx.conf into configs\rtx.conf - review the diff before committing.'
