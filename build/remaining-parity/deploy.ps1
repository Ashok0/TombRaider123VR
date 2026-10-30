$ErrorActionPreference = 'Stop'
$gameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$sourceDll = 'C:\dev\TombRaider123VR\build\remaining-parity\bin\TombRaiderVR.dll'
$destinationDll = Join-Path $gameDirectory 'TombRaiderVR.dll'
$iniPath = Join-Path $gameDirectory 'TombRaiderVR.ini'
$expectedSource = '72B2FC3F0944CD029762D48F9B924B96515D53AC86C0D2584F5A316FF470D335'
$expectedInstalled = '760B34C50AB2B82EC26F386CF20B9BAB5F50F85813D414CB00280D37839E8BD5'
$expectedIni = '3E44B99853A4850FE1BC812A6FCE4796F0F3BB62126BD596F913181E3746C0B6'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Close Tomb Raider before replacing its loaded mod.' }
if ((Get-FileHash -LiteralPath $sourceDll).Hash -ne $expectedSource) { throw 'Build changed after verification.' }
if ((Get-FileHash -LiteralPath $destinationDll).Hash -ne $expectedInstalled) { throw 'Installed DLL changed since preflight.' }
if ((Get-FileHash -LiteralPath $iniPath).Hash -ne $expectedIni) { throw 'Settings changed since preflight.' }
$backupDll = $destinationDll + '.pre-remaining-parity-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
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
    fixes = @('Mount/landing visibility recovery', 'TR3 crouch/crawl hands-only masking', 'Per-game water-exit camera/grips', 'TR3 sprint continuity', 'Ledge pull gesture robustness', 'Large ledge-catch feedback', 'Sustained holster proximity feedback', 'Blended costume cuff preservation', 'Ordinary-jump landing camera/torso stabilization')
    locomotionChecks = 1146402
    trackingChecks = 67387
    bodyChecks = 15654
    inventoryChecks = 1888
    physicsRenderChecks = 1704
    binaryAddressChecks = 3677
    negativeControls = '10 of 10 caught their intended regression'
    nativeAnimationBanks = 140
    coreTests = 'Pass (0 failures)'
    remainingAuditedGameplayParityDifferences = 0
    audit = 'C:\dev\TombRaider123VR\docs\remaining-parity-port.md'
    headsetVisualValidation = 'Pending'
}
$receipt | ConvertTo-Json | Set-Content -LiteralPath 'C:\dev\TombRaider123VR\build\remaining-parity\deployment.json' -Encoding UTF8
$receipt | ConvertTo-Json
