<#
    Installs the reshaped hair packfile into Saints Row: The Third Remastered.

    Backs the stock customize_item.vpp_pc up to customize_item.vpp_pc.bak on the
    first run and never overwrites an existing backup, so -Uninstall can always
    put the original back.

    Usage:
        .\install.ps1                      # install build\customize_item.vpp_pc
        .\install.ps1 -Uninstall           # restore the stock packfile
#>
[CmdletBinding()]
param(
    [string]$GameDir = 'F:\SteamLibrary\steamapps\common\Saints Row The Third Remastered',
    [string]$Source  = (Join-Path $PSScriptRoot '..\build\customize_item.vpp_pc'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $GameDir 'cache\customize_item.vpp_pc'
$backup = "$target.bak"

if (-not (Test-Path $target)) { throw "not found: $target" }

if ($Uninstall) {
    if (-not (Test-Path $backup)) { throw "no backup at $backup - nothing to restore" }
    Copy-Item $backup $target -Force
    Write-Host "restored stock packfile from $backup"
    return
}

if (-not (Test-Path $Source)) { throw "not found: $Source (run build_reshape.py then pack_vpp.py)" }

if (-not (Test-Path $backup)) {
    Write-Host "backing up stock packfile -> $backup"
    Copy-Item $target $backup
} else {
    Write-Host "backup already present, leaving it alone: $backup"
}

$free = (Get-PSDrive -Name $target.Substring(0,1)).Free
$need = (Get-Item $Source).Length
if ($free -lt $need) { throw "need $need bytes free on $($target.Substring(0,1)):, have $free" }

Copy-Item $Source $target -Force
Write-Host "installed $Source -> $target"
Write-Host "run with -Uninstall to revert."
