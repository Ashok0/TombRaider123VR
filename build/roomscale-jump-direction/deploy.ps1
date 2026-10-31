$ErrorActionPreference = 'Stop'
$source = 'C:\dev\TombRaider123VR\build\roomscale-jump-direction\bin\TombRaiderVR.dll'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$destination = Join-Path $game 'TombRaiderVR.dll'
$ini = Join-Path $game 'TombRaiderVR.ini'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Quit TR1-3 before replacing its DLL.' }
$newHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($newHash -ne 'F79BDDBFC2166DB841B3BC2D291A4BF2E7B0DE4FE8DC16FD5C0EF8C653C83354') { throw 'Release DLL changed after verification.' }
$oldHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
if ($oldHash -ne '68E1553632D1FF5AC2945BA5F58353E348345AFEF4AF0646175BC8973B8BAFAE') { throw 'Installed DLL changed since inspection.' }
$iniHash = (Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$backup = Join-Path $PSScriptRoot ('before-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $destination -Destination $backup
Copy-Item -LiteralPath $ini -Destination $backup
Copy-Item -LiteralPath $source -Destination $destination -Force
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $newHash) { throw 'Installed DLL hash mismatch.' }
if ((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $iniHash) { throw 'INI changed during deployment.' }
@{
    deployed_at = (Get-Date).ToString('o')
    backup = $backup
    old_sha256 = $oldHash
    new_sha256 = $newHash
    ini_sha256 = $iniHash
    regression_checks = 1215548
    native_direction_checks = 792
    release_build = 'passed'
    headset_verified = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding UTF8
Write-Output "Installed physical-turn jump direction fix. SHA256: $newHash"
Write-Output "Backup: $backup"
