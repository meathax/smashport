# Owned, isolated development exe peers. No shared TEMP cleanup or environment mutation.
[CmdletBinding()]
param(
    [ValidateRange(60,4400000)][int]$Frames = 3000,
    [ValidateRange(1024,65534)][int]$Port = 27015,
    [string]$BuildDirectory = '',
    [ValidateRange(0,4400000)][int]$Fault = 0,
    [string]$OutDirectory = '',
    [ValidateSet('reference','enhanced')][string]$Profile = 'reference',
    [ValidateRange(0,10000)][double]$LatencyMs = 0,
    [ValidateRange(0,10000)][double]$JitterMs = 0,
    [ValidateRange(0,100)][double]$LossPercent = 0,
    [int]$Seed = 1
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $taskRoot 'build/msvc' }
if (!$OutDirectory) {
    $OutDirectory = Join-Path $taskRoot ('build/net-peer-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
}
$taskArgs = @((Join-Path $PSScriptRoot 'net_exe_peer.py'), '--build-directory', $BuildDirectory,
    '--out', $OutDirectory, '--frames', [string]$Frames, '--port', [string]$Port,
    '--proxy-port', [string]($Port + 1), '--fault', [string]$Fault, '--profile', $Profile,
    '--latency-ms', $LatencyMs.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--jitter-ms', $JitterMs.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--loss-percent', $LossPercent.ToString([Globalization.CultureInfo]::InvariantCulture), '--seed', [string]$Seed)
& python @taskArgs
exit $LASTEXITCODE
