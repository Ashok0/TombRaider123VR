$ErrorActionPreference = 'Stop'
$source = 'C:\dev\TombRaider123VR\build\physical-body-hide\bin\TombRaiderVR.dll'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$destination = Join-Path $game 'TombRaiderVR.dll'
$ini = Join-Path $game 'TombRaiderVR.ini'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Quit TR1-3 before replacing its DLL.' }
$newHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($newHash -ne 'D05D41758CDB8FB358484AA062E8B97E18F2BF0890EEE92201D509C84D8EBA01') { throw 'Release DLL changed after verification.' }
$oldHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
if ($oldHash -ne 'FCD859569F4F9201E149BF10E5E171194E462EC031CBFF8E758B7B916A3D8B20') { throw 'Installed DLL changed since inspection.' }
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
    destination = $destination
    backup = $backup
    old_sha256 = $oldHash
    new_sha256 = $newHash
    ini_sha256 = $iniHash
    release_build = 'passed'
    regression_checks = 1200477
    headset_verified = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding UTF8
Write-Output "Installed TR1/2 physical crouch body hiding. SHA256: $newHash"
Write-Output "Backup: $backup"
