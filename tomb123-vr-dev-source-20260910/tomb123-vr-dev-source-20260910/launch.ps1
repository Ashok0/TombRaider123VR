param([string]$GameDirectory='C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered')
$ErrorActionPreference='Stop'
$expected=@{'tomb123.exe'='1ced64b0cff648976ff9b54624ed5e651889141639ac516a14d74e0bfb97cae3';'1\tomb1.dll'='553e4cf7e15d59909f4c7f119de4494b25dc98278ee325fc3b1e6e4e0e863418';'2\tomb2.dll'='e35fdd8d98b9332f0da4699194fa5558247e5eea01cc8f4456626bd01313256a';'3\tomb3.dll'='3f5c838a52c3eb00df500fdaa6d0f76bff944f96617a4d279d9a2b1dadb74a74'}
foreach($entry in $expected.GetEnumerator()){if((Get-FileHash -LiteralPath (Join-Path $GameDirectory $entry.Key) -Algorithm SHA256).Hash -ne $entry.Value){throw "Unsupported game build: $($entry.Key). No hooks applied."}}
$p=Get-Process tomb123 -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq (Join-Path $GameDirectory 'tomb123.exe')} | Select-Object -First 1
if(!$p){Start-Process -FilePath (Join-Path $GameDirectory 'tomb123.exe') -WorkingDirectory $GameDirectory;for($i=0;$i -lt 30;$i++){Start-Sleep -Milliseconds 1000;$p=Get-Process tomb123 -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq (Join-Path $GameDirectory 'tomb123.exe')} | Select-Object -First 1;if($p -and $p.MainWindowHandle -ne 0){break}}}
if(!$p){throw 'Game did not start.'}
& (Join-Path $PSScriptRoot 'build\inject.exe') $p.Id (Join-Path $PSScriptRoot 'build\tombvr.dll')
if($LASTEXITCODE -ne 0){throw "Injection failed: $LASTEXITCODE"}
Write-Output 'Prototype attached. Select TR1, TR2 or TR3 and load a level; hooks install when that game''s DLL loads. OpenXR starts automatically. F7 recenters tracking; F8 toggles OpenXR; F6 toggles monitor-only SBS. Close the game to remove all in-memory hooks.'
