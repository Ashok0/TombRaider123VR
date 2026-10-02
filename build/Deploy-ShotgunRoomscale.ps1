$ErrorActionPreference = 'Stop'
$gameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$builtDll = 'C:\dev\TombRaider123VR\build\x64\Release\TombRaiderVR.dll'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Close Tomb Raider before installing the DLL.' }
$backupDir = Join-Path 'C:\dev\TombRaider123VR\build' ('pre-shotgun-roomscale-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backupDir | Out-Null
foreach ($name in @('TombRaiderVR.dll','TombRaiderVR.ini','TombRaiderVR.log')) {
    $installed = Join-Path $gameDir $name
    if (Test-Path -LiteralPath $installed) { Copy-Item -LiteralPath $installed -Destination $backupDir }
}
$targetDll = Join-Path $gameDir 'TombRaiderVR.dll'
Copy-Item -LiteralPath $builtDll -Destination $targetDll -Force
$sourceHash = (Get-FileHash -LiteralPath $builtDll -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $targetDll -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed DLL hash mismatch.' }
Write-Output ('Installed and verified: ' + $sourceHash)
Write-Output ('Backup: ' + $backupDir)
Get-Content -LiteralPath (Join-Path $gameDir 'TombRaiderVR.ini') | Select-String 'FirstPersonMotionGunRaiseMetres'
