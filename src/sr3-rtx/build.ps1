# Build BOTH outputs with the MSVC Build Tools toolchain (phase 1 host/logic split):
#   build\sr3-rtx.asi        - the persistent HOST (src\sr3-rtx-host\host.cpp); the ASI loader loads this
#   build\sr3-rtx-logic.dll  - today's shim (sr3rtx.cpp); loaded by the host, NOT by the ASI loader
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
#
# /MAP is not optional. The shim installs a crash handler and writes sr3-rtx-crash.dmp, and on
# 2026-08-28 that dump named a fault at sr3-rtx.asi+0x198ee - which nothing in the project could
# turn into a line of source, because no map or symbols were ever produced. A crash address you
# cannot resolve is a crash you have to guess at.
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /O2 /W3 /EHsc /MT /LD " +
       "`"$src\sr3rtx.cpp`" /Fe:`"$out\sr3-rtx-logic.dll`" /Fo:`"$out\sr3rtx.obj`" " +
       "/link user32.lib shlwapi.lib /MAP:`"$out\sr3-rtx-logic.map`""

$hostDir = Join-Path (Split-Path $src -Parent) 'sr3-rtx-host'
$hostCmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /O2 /W3 /EHsc /MT /LD " +
       "`"$hostDir\host.cpp`" /Fe:`"$out\sr3-rtx.asi`" /Fo:`"$out\sr3-rtx-host.obj`" " +
       "/link kernel32.lib /MAP:`"$out\sr3-rtx.map`""

Write-Host "building..."

# Delete the previous output first. Without this a FAILED compile leaves the old .asi in place
# and the check below reports "OK" for a binary that does not contain the current source -
# which can then be deployed and tested as though it were the new build.
Remove-Item "$out\sr3-rtx.asi", "$out\sr3-rtx-logic.dll" -ErrorAction SilentlyContinue

$result = cmd /c $cmd 2>&1
$result | ForEach-Object { Write-Host "  $_" }
$result = cmd /c $hostCmd 2>&1
$result | ForEach-Object { Write-Host "  $_" }

foreach ($f in "sr3-rtx.asi", "sr3-rtx-logic.dll") {
    if (Test-Path "$out\$f") {
        $item = Get-Item "$out\$f"
        Write-Host "`nOK: $($item.FullName) ($($item.Length) bytes)"
    } else {
        throw "build failed - no output produced: $f"
    }
}
