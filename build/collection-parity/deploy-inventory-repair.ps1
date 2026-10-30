$ErrorActionPreference = 'Stop'
$gameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$sourceDll = 'C:\dev\TombRaider123VR\build\collection-parity\bin\TombRaiderVR.dll'
$destinationDll = Join-Path $gameDirectory 'TombRaiderVR.dll'
$expectedSource = 'F212ADCBE11C00D6B85B4F7ED5A6A66CCE4FA6FBB4AF7F667C0F1D0AE2B256E3'
$expectedInstalled = '4B7A0327157B1F2ADCBB4DBC44BB1658E0BED7EBF93AB51454CD437551BC9780'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Close Tomb Raider before replacing its loaded mod.' }
if ((Get-FileHash -LiteralPath $sourceDll).Hash -ne $expectedSource) { throw 'Build changed after verification.' }
if ((Get-FileHash -LiteralPath $destinationDll).Hash -ne $expectedInstalled) { throw 'Installed DLL changed since the deployment preflight.' }
$backupDll = $destinationDll + '.pre-inventory-repair-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
Copy-Item -LiteralPath $destinationDll -Destination $backupDll
try {
    Copy-Item -LiteralPath $sourceDll -Destination $destinationDll -Force
    if ((Get-FileHash -LiteralPath $destinationDll).Hash -ne $expectedSource) { throw 'Deployed DLL hash mismatch.' }
} catch {
    Copy-Item -LiteralPath $backupDll -Destination $destinationDll -Force
    throw
}
$receipt = [ordered]@{
    deployedAt = (Get-Date).ToString('o')
    source = $sourceDll
    destination = $destinationDll
    sha256 = $expectedSource
    previousSha256 = $expectedInstalled
    backup = $backupDll
    iniSha256 = (Get-FileHash -LiteralPath (Join-Path $gameDirectory 'TombRaiderVR.ini')).Hash
    fixes = @('Submit background without ending frame', 'Restore native inventory camera without clearing background', 'Shared ring and label spatial projection', 'Preserve gameplay background camera in both view modes')
    headsetVisualValidation = 'Pending'
}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath 'C:\dev\TombRaider123VR\build\collection-parity\deployment-inventory-repair.json' -Encoding UTF8
$receipt | ConvertTo-Json -Depth 4
