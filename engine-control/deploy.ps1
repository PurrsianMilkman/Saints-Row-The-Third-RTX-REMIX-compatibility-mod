# Deploy (or remove) sr3-engine.asi + sr3-engine.ini in the game folder.
#
#   .\deploy.ps1            build, deploy, and hash-verify
#   .\deploy.ps1 -Remove    take it back out of the game folder
#
# Touches ONLY sr3-engine.*. The main shim's files - sr3-rtx.asi, sr3-rtx.ini, rtx.conf,
# bridge.conf - are never read or written by this script.
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$here    = $PSScriptRoot
$root    = Split-Path $here -Parent
$gameDir = Join-Path $root 'Saints Row 3'
if (-not (Test-Path $gameDir)) { throw "game directory not found at $gameDir" }

$asiTarget = Join-Path $gameDir 'sr3-engine.asi'
$iniTarget = Join-Path $gameDir 'sr3-engine.ini'

if ($Remove) {
    foreach ($f in @($asiTarget, $iniTarget)) {
        if (Test-Path $f) { Remove-Item $f -Force; Write-Host "removed $(Split-Path $f -Leaf)" }
        else { Write-Host "$(Split-Path $f -Leaf) was not deployed" }
    }
    Write-Host "`nThe game folder now has only the main shim. sr3-rtx.* untouched."
    return
}

& (Join-Path $here 'src\build.ps1')

Copy-Item (Join-Path $here 'build\sr3-engine.asi')   $asiTarget -Force
Copy-Item (Join-Path $here 'configs\sr3-engine.ini') $iniTarget -Force

Write-Host "`ndeployed:"
foreach ($f in @($asiTarget, $iniTarget)) {
    $h = (Get-FileHash $f -Algorithm MD5).Hash.ToLower()
    Write-Host ("  {0,-18} {1}  {2} bytes" -f (Split-Path $f -Leaf), $h, (Get-Item $f).Length)
}

# The deployed binary must be the one just built, not a stale copy.
$a = (Get-FileHash $asiTarget -Algorithm MD5).Hash
$b = (Get-FileHash (Join-Path $here 'build\sr3-engine.asi') -Algorithm MD5).Hash
if ($a -ne $b) { throw 'deployed .asi does not match the build' }
Write-Host '  deployed .asi matches the build: YES'

Write-Host "`nmain shim, untouched by this script:"
foreach ($n in @('sr3-rtx.asi','sr3-rtx.ini')) {
    $f = Join-Path $gameDir $n
    if (Test-Path $f) {
        Write-Host ("  {0,-18} {1}" -f $n, (Get-FileHash $f -Algorithm MD5).Hash.ToLower())
    }
}
Write-Host "`nLog will be written to: $gameDir\sr3-engine.log"
