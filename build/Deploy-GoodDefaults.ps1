$ErrorActionPreference = 'Stop'
$gameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$workspace = 'C:\dev\TombRaider123VR'
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Close Tomb Raider before installing the DLL and INI.' }
$expected = Get-Content -LiteralPath (Join-Path $workspace 'build\good-defaults-source-hashes.json') -Raw | ConvertFrom-Json
foreach ($name in @('TombRaiderVR.ini','TombRaiderVR.ini.good')) {
    if ((Get-FileHash -LiteralPath (Join-Path $gameDir $name)).Hash -ne $expected.$name) {
        throw ($name + ' changed since comparison; refresh the prepared settings before deployment.')
    }
}
$backupDir = Join-Path $workspace ('build\pre-good-defaults-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backupDir | Out-Null
foreach ($name in @('TombRaiderVR.dll','TombRaiderVR.ini','TombRaiderVR.log','TombRaiderVR.ini.good')) {
    $installed = Join-Path $gameDir $name
    if (Test-Path -LiteralPath $installed) { Copy-Item -LiteralPath $installed -Destination $backupDir }
}
$builtDll = Join-Path $workspace 'build\x64\Release\TombRaiderVR.dll'
$preparedIni = Join-Path $workspace 'build\TombRaiderVR.good-defaults.ini'
Copy-Item -LiteralPath $builtDll -Destination (Join-Path $gameDir 'TombRaiderVR.dll') -Force
Copy-Item -LiteralPath $preparedIni -Destination (Join-Path $gameDir 'TombRaiderVR.ini') -Force
if ((Get-FileHash -LiteralPath $builtDll).Hash -ne (Get-FileHash -LiteralPath (Join-Path $gameDir 'TombRaiderVR.dll')).Hash) { throw 'DLL verification failed.' }
if ((Get-FileHash -LiteralPath $preparedIni).Hash -ne (Get-FileHash -LiteralPath (Join-Path $gameDir 'TombRaiderVR.ini')).Hash) { throw 'INI verification failed.' }
if ((Get-FileHash -LiteralPath (Join-Path $gameDir 'TombRaiderVR.ini.good')).Hash -ne $expected.'TombRaiderVR.ini.good') { throw 'Reference file changed.' }
Write-Output ('Installed DLL: ' + (Get-FileHash -LiteralPath $builtDll).Hash)
Write-Output 'Installed INI verified; .good reference unchanged.'
Write-Output ('Backup: ' + $backupDir)
