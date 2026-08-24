# Launches the DX9 exe directly, not via game_launcher.exe.
$root    = Split-Path $PSScriptRoot -Parent
$gameDir = Join-Path $root 'Saints Row 3'
Start-Process -FilePath (Join-Path $gameDir 'SaintsRowTheThird.exe') -WorkingDirectory $gameDir
Write-Host 'Launched SaintsRowTheThird.exe (DX9). Alt+X in game for the Remix menu.'
