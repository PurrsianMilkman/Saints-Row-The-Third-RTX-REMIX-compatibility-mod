# Snapshots the project plus the deployed game-dir state (confs, logs, Remix captures).
# The game install itself is NOT copied - it is ~10 GB and reinstallable. What is copied is
# everything that cannot be recovered by reinstalling: our sources, docs, RE artefacts, the
# runtime, and the live evidence the conclusions in docs/worklog.md rest on.
$ErrorActionPreference = 'Stop'
$root = 'D:\SR3RTXREMIXCOMP'
$game = Join-Path $root 'Saints Row 3'
$dest = "D:\SR3RTXREMIXCOMP-backup-$(Get-Date -Format yyyy-MM-dd)"
if (Test-Path $dest) { throw "$dest already exists - move or remove it first" }
New-Item -ItemType Directory -Path $dest | Out-Null
$gdest = Join-Path $dest '_deployed-game-dir'
New-Item -ItemType Directory -Path $gdest | Out-Null

# robocopy uses 0-7 for success (bits mean "copied", "extra files" etc); only >=8 is a failure.
# Its exit code lingers in $LASTEXITCODE and would otherwise make the whole script look failed,
# so it is swallowed here rather than at the end where the summary has already printed.
function Copy-Tree($from, $to) {
    robocopy $from $to /E /R:2 /W:1 /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) { throw "robocopy failed ($rc): $from" }
    cmd /c exit 0
}

Write-Host 'copying project...'
foreach ($d in '.git','build','configs','dist','docs','re','runtime','src','tools') {
    if (Test-Path (Join-Path $root $d)) {
        Write-Host "  $d"
        Copy-Tree (Join-Path $root $d) (Join-Path $dest $d)
    }
}
Get-ChildItem $root -File | Copy-Item -Destination $dest -Force
Get-ChildItem $root -File -Force -Filter '.gitignore' | Copy-Item -Destination $dest -Force

Write-Host 'copying deployed game-dir state...'
Get-ChildItem $game -File | Where-Object {
    $_.Extension -in '.conf','.ini','.asi','.log' -or $_.Name -like 'sr3-rtx*' -or
    $_.Name -in 'metrics.txt','nrc_session_log.txt'
} | Copy-Item -Destination $gdest -Force
foreach ($s in 'rtx-remix\captures','rtx-remix\logs') {
    $f = Join-Path $game $s
    if (Test-Path $f) {
        Write-Host "  $s"
        Copy-Tree $f (Join-Path $gdest (Split-Path $s -Leaf))
    }
}

$files = Get-ChildItem $dest -Recurse -File -Force
"{0} files, {1:N0} MB -> {2}" -f $files.Count, (($files|Measure-Object Length -Sum).Sum/1MB), $dest

# Verify rather than trust: robocopy reports per-directory, not per-file.
foreach ($d in '.git','build','configs','dist','docs','re','runtime','src','tools') {
    $a = (Get-ChildItem (Join-Path $root $d) -Recurse -File -Force -EA SilentlyContinue).Count
    $b = (Get-ChildItem (Join-Path $dest $d) -Recurse -File -Force -EA SilentlyContinue).Count
    "{0,-10} {1,7} -> {2,-7} {3}" -f $d, $a, $b, $(if ($a -eq $b) { 'OK' } else { '** DIFF **' })
}
exit 0
