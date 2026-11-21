[CmdletBinding()]
param(
    [switch]$PrepareOnly,
    [switch]$RenewC1Native,
    [string]$BuildDirectory = 'build/msvc',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateRange(1,3)][int]$Workers = 3,
    [string]$OutDir = '',
    [string]$ResumeReport = '',
    [string]$C1Evidence = 'evidence/c1-sound-full-runs-2026-10-01.json'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Python = (Get-Command python -ErrorAction Stop).Source
$RunnerArgs = @('--build-directory', $BuildDirectory, '--configuration', $Configuration, '--workers', $Workers, '--c1-evidence', $C1Evidence)
if ($PrepareOnly) { $RunnerArgs += '--prepare-only' }
if ($RenewC1Native) { $RunnerArgs += '--renew-c1-native' }
if ($OutDir) { $RunnerArgs += @('--out-dir', $OutDir) }
if ($ResumeReport) { $RunnerArgs += @('--resume-report', $ResumeReport) }
Push-Location $Root
try {
    & $Python (Join-Path $PSScriptRoot 'rebaseline.py') @RunnerArgs
    $RunnerCode = $LASTEXITCODE
} finally { Pop-Location }
exit $RunnerCode
