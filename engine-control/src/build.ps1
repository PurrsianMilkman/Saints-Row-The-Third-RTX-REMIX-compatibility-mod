# Build sr3-engine.asi (32-bit DLL) - the standalone engine renderer control plugin.
#
# Deliberately separate from src\sr3-rtx\build.ps1 so the main shim's build is never touched.
# Output goes to engine-control\build\, not the main build\ directory.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$out = Join-Path (Split-Path $src -Parent) 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars32.bat not found at $vcvars" }

# /MAP is not optional here either: it is what proves the 74 thunks were emitted as 74 distinct
# functions rather than folded into one by /OPT:ICF, which would silently destroy the per-opcode
# attribution that is the entire point of the census.
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /O2 /W3 /EHsc /MT /LD " +
       "`"$src\sr3engine.cpp`" /Fe:`"$out\sr3-engine.asi`" /Fo:`"$out\sr3engine.obj`" " +
       "/link user32.lib /MAP:`"$out\sr3-engine.map`""

Write-Host "building sr3-engine.asi..."

# Delete first: a FAILED compile must not leave the previous binary in place to be reported OK
# and then deployed as though it were the current source.
Remove-Item "$out\sr3-engine.asi" -ErrorAction SilentlyContinue

$result = cmd /c $cmd 2>&1
$result | ForEach-Object { Write-Host "  $_" }

if (Test-Path "$out\sr3-engine.asi") {
    $item = Get-Item "$out\sr3-engine.asi"
    Write-Host "`nOK: $($item.FullName) ($($item.Length) bytes)"
    $thunks = (Select-String -Path "$out\sr3-engine.map" -Pattern 'EngineOpThunk' | Measure-Object).Count
    Write-Host "thunks emitted: $thunks (expected 74 - fewer means /OPT:ICF folded them)"
} else {
    throw "build failed - no output produced"
}
