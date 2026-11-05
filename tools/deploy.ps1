# SPDX-License-Identifier: GPL-3.0-or-later
# Deploy DLSSNR_LiteDX12 into a game folder: copy and overwrite.
#
#   .\tools\deploy.ps1                        list the games and what they have now
#   .\tools\deploy.ps1 -Target jedi           copy the current dist\Release build there
#   .\tools\deploy.ps1 -Target jedi -Build    build first, then copy
#   .\tools\deploy.ps1 -Path D:\Some\Folder   a folder that is not in the list
#   .\tools\deploy.ps1 -Target jedi -OverwriteModel   replace an existing nvngx_dlssnr.dll
#
# Per target it writes dxgi.dll and nvngx.dll_litedx12.dll (always overwritten,
# no backup), plus DLSSNR_LiteDX12.ini and nvngx_dlssnr.dll *only when the folder
# has none*: an existing settings file and an existing model are kept.
# Nothing else in the folder is touched.
#
# This project is Direct3D 12 only, so do not put DX11 games in this list.
param(
    [string]$Target = '',
    [string]$Path = '',
    [string]$Source = '',
    [string]$Model = '',
    [switch]$OverwriteModel,
    [switch]$HDR,
    [switch]$Standard,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# The list: maintained by hand, one line per game.
$games = [ordered]@{
    jedi = 'G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64'
    gta  = 'G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced'
    wolong = 'G:\XboxGames\Wo Long- Fallen Dynasty\Content'
    mhwilds = 'F:\SteamLibrary\steamapps\common\MonsterHunterWilds'
    ronin = 'G:\SteamLibrary\steamapps\common\Ronin'
    wdlegion = 'G:\UBISoft\Watch Dogs Legion\bin'
    fh6 = 'G:\XboxGames\Forza Horizon 6\Content'
    kcd2 = 'G:\XboxGames\Kingdom Come- Deliverance II\Content'
    starfield = 'G:\XboxGames\Starfield\Content'
    alanwake2 = 'G:\EpicGames\AlanWake2'
    uncharted = 'G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection'
    dd2 = 'F:\SteamLibrary\steamapps\common\Dragons Dogma 2'
    tsushima = "F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT"
    nioh3 = 'F:\SteamLibrary\steamapps\common\Nioh3'
    dsdc = 'F:\XboxGames\DSDC\Content'
    msfs2024 = 'F:\XboxGames\Microsoft Flight Simulator 2024\Content'
    sm2 = 'F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc'
    stellarblade = 'F:\SteamLibrary\steamapps\common\StellarBlade\SB\Binaries\Win64'
    manorlords = 'F:\XboxGames\Manor Lords\Content\ManorLords\Binaries\WinGDK'
    coldwar = 'G:\XboxGames\Call of Duty- Black Ops Cold War\Content'
    pbz = 'F:\SteamLibrary\steamapps\common\PBZ\platform\steam'
    gow = 'F:\SteamLibrary\steamapps\common\God of War Ragnarok'
}
$files = @('dxgi.dll', 'nvngx.dll_litedx12.dll')   # always overwritten
$iniName = 'DLSSNR_LiteDX12.ini'                   # written only when the folder has none
$modelName = 'nvngx_dlssnr.dll'                    # written only when the folder has none
# Folders that must run the HDR10 variant. -HDR forces it elsewhere; -Standard
# overrides this list, so a standard build can still be installed on purpose.
$hdrTargets = @()   # HDR variant removed; every target is standard
$useHdr = [bool]$HDR
if (-not $useHdr -and $Target -and ($hdrTargets -contains $Target)) { $useHdr = $true }
if ($Standard) { $useHdr = $false }
if (-not $Source) { $Source = Join-Path $root $(if ($useHdr) { 'dist\Release-HDR' } else { 'dist\Release' }) }
if (-not $Model) { $Model = Join-Path (Split-Path -Parent $root) 'DLSSNR_Model\nvngx_dlssnr_2026_10\nvngx_dlssnr.dll' }

# Which variant a folder currently holds, decided by size: the HDR build links
# the HDR modules and is always larger than the standard one.
function Get-Variant([string]$runtime) {
    if (-not (Test-Path -LiteralPath $runtime)) { return 'not deployed' }
    $length = (Get-Item -LiteralPath $runtime).Length
    foreach ($pair in @(@('dist\Release', 'standard'), @('dist\Release-HDR', 'HDR'))) {
        $known = Join-Path $root (Join-Path $pair[0] 'nvngx.dll_litedx12.dll')
        if ((Test-Path -LiteralPath $known) -and ((Get-Item -LiteralPath $known).Length -eq $length)) { return $pair[1] }
    }
    return 'other'
}

function Show-State {
    foreach ($name in $games.Keys) {
        $folder = $games[$name]
        if (-not (Test-Path $folder)) { '{0,-6} {1,-50} (folder missing)' -f $name, $folder; continue }
        $runtime = Join-Path $folder 'nvngx.dll_litedx12.dll'
        $state = if (Test-Path $runtime) { 'runtime ' + (Get-FileHash $runtime -Algorithm MD5).Hash.Substring(0, 12) } else { 'not deployed' }
        $hasModel = if (Test-Path (Join-Path $folder $modelName)) { 'model ok' } else { 'NO MODEL' }
        $hasIni = if (Test-Path (Join-Path $folder $iniName)) { 'ini ok' } else { 'NO INI' }
        $want = if ($hdrTargets -contains $name) { 'HDR' } else { 'standard' }
        '{0,-12} {1,-50} {2,-24} want {3,-8} has {4,-12} {5}' -f $name, $folder, $state, $want, (Get-Variant $runtime), "$hasModel, $hasIni"
    }
    $built = if (Test-Path (Join-Path $Source 'nvngx.dll_litedx12.dll')) { (Get-Item (Join-Path $Source 'nvngx.dll_litedx12.dll')).LastWriteTime } else { 'missing' }
    "source : $Source  (runtime built $built)"
    "model  : $modelName  from  $Model"
}
if (-not $Target -and -not $Path) { Show-State; return }

if ($Build) {
    Push-Location $root
    try { & (Join-Path $root 'Build.ps1') -HDR:$useHdr }
    finally { Pop-Location }
}
foreach ($file in $files + $iniName) {
    if (-not (Test-Path (Join-Path $Source $file))) { throw "missing in ${Source}: $file" }
}

$targets = @()
if ($Path) { $targets += @{ name = 'custom'; folder = $Path } }
else {
    if (-not $games.Contains($Target)) { throw "unknown target '$Target'" }
    $targets += @{ name = $Target; folder = $games[$Target] }
}

foreach ($entry in $targets) {
    $folder = $entry.folder
    if (-not (Test-Path $folder)) { throw "folder missing: $folder" }
    Write-Host "== $($entry.name): $folder"
    foreach ($file in $files) {
        $destination = Join-Path $folder $file
        Copy-Item (Join-Path $Source $file) $destination -Force
        Write-Host ("   wrote      {0,-26} {1,9} bytes  {2}" -f $file, (Get-Item $destination).Length, (Get-FileHash $destination -Algorithm MD5).Hash.Substring(0, 12))
    }
    $hdrLicense = Join-Path $Source 'Licenses\RenoDX_HDR_MIT.txt'
    if ($useHdr -and (Test-Path -LiteralPath $hdrLicense)) {
        $licenseFolder = Join-Path $folder 'Licenses'
        New-Item -ItemType Directory -Path $licenseFolder -Force | Out-Null
        Copy-Item -LiteralPath $hdrLicense -Destination (Join-Path $licenseFolder 'RenoDX_HDR_MIT.txt') -Force
    }
    # Settings are the user's: an INI that is already in the folder is left alone.
    $iniDestination = Join-Path $folder $iniName
    if (Test-Path $iniDestination) {
        Write-Host ("   kept       {0,-26} {1,9} bytes  {2}" -f $iniName, (Get-Item $iniDestination).Length, (Get-FileHash $iniDestination -Algorithm MD5).Hash.Substring(0, 12))
    } else {
        Copy-Item (Join-Path $Source $iniName) $iniDestination -Force
        Write-Host ("   wrote      {0,-26} {1,9} bytes  {2}" -f $iniName, (Get-Item $iniDestination).Length, (Get-FileHash $iniDestination -Algorithm MD5).Hash.Substring(0, 12))
    }
    $modelDestination = Join-Path $folder $modelName
    if (Test-Path $modelDestination) {
        $hash = (Get-FileHash $modelDestination -Algorithm MD5).Hash.Substring(0, 12)
        if (-not $OverwriteModel) {
            Write-Host ("   kept       {0,-26} {1,9} bytes  {2}   (use -OverwriteModel to replace)" -f $modelName, (Get-Item $modelDestination).Length, $hash)
        } else {
            Copy-Item $Model $modelDestination -Force
            Write-Host ("   replaced   {0,-26} {1,9} bytes  {2}   (was {3})" -f $modelName, (Get-Item $modelDestination).Length, (Get-FileHash $modelDestination -Algorithm MD5).Hash.Substring(0, 12), $hash)
        }
    } elseif (Test-Path $Model) {
        Copy-Item $Model $modelDestination -Force
        Write-Host ("   wrote      {0,-26} {1,9} bytes  {2}   (NR model)" -f $modelName, (Get-Item $modelDestination).Length, (Get-FileHash $modelDestination -Algorithm MD5).Hash.Substring(0, 12))
    } else {
        Write-Host ("   MISSING    {0,-26} source not found: {1}" -f $modelName, $Model)
    }
}
Write-Host 'done - nothing else in the game folder was touched'
