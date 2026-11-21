# Stage the clean, ROM-free installer with the RecompLauncher profile.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$')][string]$Version = '1.0.0'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$Installer = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Installer)
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
& python (Join-Path $Root 'tools\release\launcher_package.py') `
    --installer $Installer --out-dir $OutDir --version $Version
if ($LASTEXITCODE -ne 0) {
    throw 'RecompLauncher package staging failed.'
}
