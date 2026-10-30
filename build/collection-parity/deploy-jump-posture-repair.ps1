$ErrorActionPreference = 'Stop'
$gameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$sourceDll = 'C:\dev\TombRaider123VR\build\collection-parity\bin\TombRaiderVR.dll'
$destinationDll = Join-Path $gameDirectory 'TombRaiderVR.dll'
$iniPath = Join-Path $gameDirectory 'TombRaiderVR.ini'
$expectedSource = '760B34C50AB2B82EC26F386CF20B9BAB5F50F85813D414CB00280D37839E8BD5'
$expectedInstalled = '31ACCB7E4B1EAE8B262AD047B86FB867AB581C2E76CEA9A2FAE55E84871DD6CA'
$expectedIni = '3E44B99853A4850FE1BC812A6FCE4796F0F3BB62126BD596F913181E3746C0B6'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Close Tomb Raider before replacing its loaded mod.' }
if ((Get-FileHash -LiteralPath $sourceDll).Hash -ne $expectedSource) { throw 'Build changed after verification.' }
if ((Get-FileHash -LiteralPath $destinationDll).Hash -ne $expectedInstalled) { throw 'Installed DLL changed since preflight.' }
if ((Get-FileHash -LiteralPath $iniPath).Hash -ne $expectedIni) { throw 'Settings changed since preflight.' }
$backupDll = $destinationDll + '.pre-jump-posture-repair-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
Copy-Item -LiteralPath $destinationDll -Destination $backupDll
if ((Get-FileHash -LiteralPath $backupDll).Hash -ne $expectedInstalled) { throw 'Rollback backup verification failed.' }
try {
    Copy-Item -LiteralPath $sourceDll -Destination $destinationDll -Force
    if ((Get-FileHash -LiteralPath $destinationDll).Hash -ne $expectedSource) { throw 'Deployed DLL hash mismatch.' }
} catch {
    Copy-Item -LiteralPath $backupDll -Destination $destinationDll -Force
    throw
}
if ((Get-FileHash -LiteralPath $iniPath).Hash -ne $expectedIni) { throw 'Settings unexpectedly changed during deployment.' }
$receipt = [ordered]@{
    deployedAt = (Get-Date).ToString('o')
    source = $sourceDll
    destination = $destinationDll
    sha256 = $expectedSource
    previousSha256 = $expectedInstalled
    backup = $backupDll
    iniSha256 = $expectedIni
    fixes = @('Directional jump analog guard', 'Directional jump HD body/hair visibility', 'Head-pitch posture compensation', 'Side/back arm stability', 'Left-turn landing classification', 'Backjump collision-root camera arc', 'Base-camera crate/wall clearance')
    locomotionChecks = 1145888
    trackingChecks = 67387
    bodyChecks = 15654
    inventoryChecks = 1888
    binaryAddressChecks = 3677
    coreTests = 'Pass (0 failures)'
    remainingGameplayParityDifferences = 9
    audit = 'C:\dev\TombRaider123VR\docs\collection-gap-audit-20261028.md'
    headsetVisualValidation = 'Pending'
}
$receipt | ConvertTo-Json | Set-Content -LiteralPath 'C:\dev\TombRaider123VR\build\collection-parity\deployment-jump-posture-repair.json' -Encoding UTF8
$receipt | ConvertTo-Json
