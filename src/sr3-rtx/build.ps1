# Build sr3-rtx.asi (32-bit DLL) with the MSVC Build Tools toolchain.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$out = Join-Path (Split-Path (Split-Path $src -Parent) -Parent) 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars32.bat not found at $vcvars" }

# /LD builds the DLL; the .asi extension is just a renamed DLL. Object and output paths
# are given as full filenames - a trailing backslash inside quotes escapes the quote.
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /O2 /W3 /EHsc /MT /LD " +
       "`"$src\sr3rtx.cpp`" /Fe:`"$out\sr3-rtx.asi`" /Fo:`"$out\sr3rtx.obj`" " +
       "/link user32.lib shlwapi.lib"

Write-Host "building..."

# Delete the previous output first. Without this a FAILED compile leaves the old .asi in place
# and the check below reports "OK" for a binary that does not contain the current source -
# which can then be deployed and tested as though it were the new build.
Remove-Item "$out\sr3-rtx.asi" -ErrorAction SilentlyContinue

$result = cmd /c $cmd 2>&1
$result | ForEach-Object { Write-Host "  $_" }

if (Test-Path "$out\sr3-rtx.asi") {
    $item = Get-Item "$out\sr3-rtx.asi"
    Write-Host "`nOK: $($item.FullName) ($($item.Length) bytes)"
} else {
    throw "build failed - no output produced"
}
