$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered'
$destination = Join-Path $game 'TombRaiderVR.dll'
$ini = Join-Path $game 'TombRaiderVR.ini'
$bin = Join-Path $PSScriptRoot 'bin'
$expected = @{
    'TombRaiderVR.dll' = '9047434EA980E5CD5C9E64E654B706A8E6603C6DE8D719CBCD93AAC09AA49F59'
    'TombRaiderVRBindings\actions.json' = '2A85BDEA9D7B3E76580B17621E0DEC29721FAC8752AB09521F1D944509AC66E7'
    'TombRaiderVRBindings\bindings_index.json' = '1C23C56951F393E4F933932625F20C03264F7F9971ACF54736F68405FC50189F'
}
if (Get-Process tomb123 -ErrorAction SilentlyContinue) { throw 'Quit TR1-3 before replacing its DLL.' }
foreach ($relative in $expected.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $bin $relative)).Hash -ne $expected[$relative]) { throw "Build artifact changed: $relative" }
}
$oldHash = (Get-FileHash -LiteralPath $destination).Hash
if ($oldHash -ne 'D05D41758CDB8FB358484AA062E8B97E18F2BF0890EEE92201D509C84D8EBA01') { throw 'Installed DLL changed since inspection.' }
$iniHash = (Get-FileHash -LiteralPath $ini).Hash
if ($iniHash -ne 'F937F9666DFBABC49826DBBA5538DC8293549708B37E68BEBEC2626EEED72930') { throw 'Installed INI changed since inspection.' }
$bytes = [IO.File]::ReadAllBytes($ini)
$text = [Text.Encoding]::UTF8.GetString($bytes)
if ($text -match '(?im)^ValveIndex') { throw 'Index settings already exist; preserve them for review.' }
$newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$addition = @(
    '; Set to 1 for Valve Index controllers, then restart. Keep TombRaiderVRBindings beside the DLL.'
    'ValveIndexControllers=0'
    '; Grip force: lower Press for a lighter squeeze; keep Release below Press.'
    'ValveIndexGripPress=0.55'
    'ValveIndexGripRelease=0.35'
    ''
) -join $newline
$section = [regex]'(?im)^\[VR\]\r?\n'
if ($section.Matches($text).Count -ne 1) { throw 'Expected exactly one VR section.' }
$updated = $section.Replace($text, [System.Text.RegularExpressions.MatchEvaluator]{ param($match) $match.Value + $addition }, 1)
if ($updated.Replace($addition, '') -cne $text) { throw 'INI update changed existing settings.' }
$backup = Join-Path $PSScriptRoot ('before-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $ini -Destination $backup
foreach ($relative in $expected.Keys) {
    $target = Join-Path $game $relative
    if (Test-Path -LiteralPath $target) {
        $save = Join-Path $backup $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $save) | Out-Null
        Copy-Item -LiteralPath $target -Destination $save
    }
}
New-Item -ItemType Directory -Force -Path (Join-Path $game 'TombRaiderVRBindings') | Out-Null
foreach ($relative in $expected.Keys) {
    $target = Join-Path $game $relative
    Copy-Item -LiteralPath (Join-Path $bin $relative) -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $expected[$relative]) { throw "Installed hash mismatch: $relative" }
}
[IO.File]::WriteAllBytes($ini, [Text.Encoding]::UTF8.GetBytes($updated))
if ([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($ini)) -cne $updated) { throw 'INI verification failed.' }
@{
    deployed_at = (Get-Date).ToString('o')
    backup = $backup
    old_sha256 = $oldHash
    files = $expected
    previous_ini_sha256 = $iniHash
    ini_sha256 = (Get-FileHash -LiteralPath $ini).Hash
    regression_checks = 1201164
    release_build = 'passed'
    bindings_validated_against_installed_valve_profile = $true
    index_hardware_verified = $false
    index_mode_enabled = $false
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding UTF8
Write-Output 'Installed Index input support and bindings. INI includes ValveIndexControllers=0; set to 1 for Index.'
Write-Output "Backup: $backup"
