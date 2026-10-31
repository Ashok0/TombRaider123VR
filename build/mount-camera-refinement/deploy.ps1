$ErrorActionPreference = 'Stop'
$source = 'C:\dev\TombRaider123VR\build\mount-camera-refinement\bin\TombRaiderVR.dll'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$destination = Join-Path $game 'TombRaiderVR.dll'
$ini = Join-Path $game 'TombRaiderVR.ini'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Quit TR1-3 before replacing its DLL.' }
$newHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($newHash -ne 'E8232F08D9750B2F09DE94B90A4D4DEF0307607AFFF60D81FD8072721C3B68E5') { throw 'Release DLL changed after verification.' }
$oldHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
if ($oldHash -ne '4A58F99B069B714F8F120EC9D56A098857615A3529E7A966371219CCCA659280') { throw 'Installed DLL changed since inspection.' }
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
    regression_checks = 1213584
    release_build = 'passed'
    headset_verified = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding UTF8
Write-Output "Installed refined mount camera smoothing. SHA256: $newHash"
Write-Output "Backup: $backup"
