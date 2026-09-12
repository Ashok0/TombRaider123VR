# Fetches the OpenVR headers this project builds against.
#
# Only the headers are needed: openvr_api.dll is resolved with LoadLibrary at
# runtime (see VRSystem.cpp), so there is no .lib to link and no import to
# satisfy when the runtime is absent.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\fetch_openvr.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'third_party\openvr\headers'
New-Item -ItemType Directory -Force $dest | Out-Null

# openvr.h is the only header the project includes. If you already have an
# OpenVR SDK checkout, just copy its headers\openvr.h into $dest instead --
# matching the SDK you will ship openvr_api.dll from avoids any chance of an
# interface-version mismatch (the header's IVRSystem_Version string has to be
# one the runtime supports).
$url = 'https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr.h'
$out = Join-Path $dest 'openvr.h'

Write-Host "fetching openvr.h ..."
Invoke-WebRequest -Uri $url -OutFile $out

$ver = Select-String -Path $out -Pattern 'IVRSystem_Version\s*=\s*"([^"]+)"' |
       ForEach-Object { $_.Matches[0].Groups[1].Value }

Write-Host ""
Write-Host "OpenVR header in $dest  (interface $ver)"
Write-Host "At runtime you also need openvr_api.dll (x64) next to tomb123.exe;"
Write-Host "SteamVR ships one at steamapps\common\SteamVR\bin\win64\openvr_api.dll."
