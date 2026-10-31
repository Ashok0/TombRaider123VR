$ErrorActionPreference = 'Stop'
$source = 'C:\dev\TombRaider123VR\build\climbing-arms\bin\TombRaiderVR.dll'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$destination = Join-Path $game 'TombRaiderVR.dll'
$ini = Join-Path $game 'TombRaiderVR.ini'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Quit TR1-3 before replacing its DLL.' }
$newHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($newHash -ne 'B20F636F125CEB728B76C90C69926174B057B48F78BEFF7BA6A742A7B5C74322') { throw 'Release DLL changed after verification.' }
$oldHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
if ($oldHash -ne '15F0BE5FD3DD17D978D40F4290A4FDF2325EBFEBF8E89A827CC10A1C3D43FFB7') { throw 'Installed DLL changed since inspection.' }
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
    regression_checks = 1210602
    release_build = 'passed'
    headset_verified = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding UTF8
Write-Output "Installed climbing arm improvements. SHA256: $newHash"
Write-Output "Backup: $backup"
