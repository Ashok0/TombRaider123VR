$ErrorActionPreference = 'Stop'
$iniPath = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered\TombRaiderVR.ini'
$encoding = [System.Text.Encoding]::GetEncoding(28591)
$bytes = [System.IO.File]::ReadAllBytes($iniPath)
$original = $encoding.GetString($bytes)
$pattern = '(?m)^FirstPersonMotionGunRaiseMetres=[^\r\n]*'
if ([regex]::Matches($original, $pattern).Count -ne 1) { throw 'Expected exactly one hand-height setting.' }
$updated = [regex]::Replace($original, $pattern, 'FirstPersonMotionGunRaiseMetres=-0.06985')
$backup = Join-Path $PSScriptRoot ('TombRaiderVR-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.ini')
[System.IO.File]::WriteAllBytes($backup, $bytes)
[System.IO.File]::WriteAllBytes($iniPath, $encoding.GetBytes($updated))
if ($encoding.GetString([System.IO.File]::ReadAllBytes($iniPath)) -cne $updated) { throw 'Verification failed.' }
Write-Output 'Installed and verified: FirstPersonMotionGunRaiseMetres=-0.06985'
