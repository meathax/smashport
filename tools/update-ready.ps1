[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\rom-player'),
    [string]$Destination = (Join-Path $PSScriptRoot '..\ready')
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$Required = @('SmashTV.exe', 'dxcompiler.dll', 'librashader.dll', 'librashader_d3d11.dll', 'shaders')
foreach ($Name in $Required) {
    if (!(Test-Path -LiteralPath (Join-Path $BuildDirectory $Name))) {
        throw "Build output is incomplete: missing $Name in $BuildDirectory"
    }
}

if (Test-Path -LiteralPath $Destination) {
    Remove-Item -LiteralPath $Destination -Recurse
}
New-Item -ItemType Directory -Path $Destination | Out-Null
foreach ($Name in @('SmashTV.exe', 'dxcompiler.dll', 'librashader.dll', 'librashader_d3d11.dll', 'EOSSDK-Win64-Shipping.dll', 'SDL3.dll')) {
    $Source = Join-Path $BuildDirectory $Name
    if (Test-Path -LiteralPath $Source) {
        Copy-Item -LiteralPath $Source -Destination $Destination
    }
}
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'shaders') -Destination $Destination -Recurse
Copy-Item -LiteralPath (Join-Path $Root 'THIRD_PARTY_NOTICES.md') -Destination $Destination

$Launcher = @'
@echo off
set "STV_HOST_ROM_ZIP=%~dp0..\build\rom-player\selected-l8.zip"
start "" /D "%~dp0" "%~dp0SmashTV.exe"
'@
Set-Content -LiteralPath (Join-Path $Destination 'Play Smash TV.cmd') -Value $Launcher -Encoding ascii

$Commit = (& git -C $Root rev-parse HEAD).Trim()
$Identity = [ordered]@{
    updated_utc = (Get-Date).ToUniversalTime().ToString('o')
    commit = $Commit
    executable_sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination 'SmashTV.exe')).Hash.ToLowerInvariant()
    build_directory = $BuildDirectory
    rom_location = (Join-Path $BuildDirectory 'selected-l8.zip')
    scope = 'Private local test staging; not a distributable package'
}
$Identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'BUILD_INFO.json') -Encoding utf8
Write-Host "Ready test folder updated: $Destination"
