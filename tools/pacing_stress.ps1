# PLAN P6/P8 pacing stress. Validation also works without launching a game/load:
#   .\tools\pacing_stress.ps1 -ValidateReport report.json -Frames 3300
param(
    [ValidateRange(1, 10000000)][int]$Frames = 3300,
    [ValidateRange(0, 1024)][int]$Burners = [Environment]::ProcessorCount,
    [string]$Exe = '',
    [string]$ValidateReport = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

function Test-FiniteNumber($Value) {
    return (($Value -is [int]) -or ($Value -is [long]) -or ($Value -is [double]) -or
            ($Value -is [float]) -or ($Value -is [decimal])) -and
           -not [double]::IsNaN([double]$Value) -and -not [double]::IsInfinity([double]$Value)
}

function Get-PacingVerdict($HostReport, [int]$RequestedFrames) {
    $expected = $RequestedFrames / 54.7068
    $reasons = [System.Collections.Generic.List[string]]::new()
    $required = @('frames','seconds','timed_out','audio_initial_device','audio_device','audio_rate',
                  'samples','audio_errors','audio_error_stage','audio_error_code','audio_underruns',
                  'audio_min_queued_ms','sound_fault')
    $complete = $true
    foreach ($name in $required) {
        if ($null -eq $HostReport -or $HostReport.PSObject.Properties.Name -notcontains $name) {
            $reasons.Add("Missing report field: $name")
            $complete = $false
        }
    }
    $timing = $false
    $audio = $false
    $unavailable = $false
    if ($complete) {
        $numbers = $true
        foreach ($name in @('frames','seconds','audio_rate','samples','audio_errors','audio_underruns','audio_min_queued_ms')) {
            if (-not (Test-FiniteNumber $HostReport.$name)) {
                $reasons.Add("Invalid numeric report field: $name")
                $numbers = $false
            }
        }
        $booleans = ($HostReport.timed_out -is [bool]) -and
                    ($HostReport.audio_initial_device -is [bool]) -and ($HostReport.audio_device -is [bool])
        $strings = ($HostReport.audio_error_stage -is [string]) -and
                   ($HostReport.audio_error_code -is [string]) -and ($HostReport.sound_fault -is [string])
        if (-not $booleans) { $reasons.Add('Device and timeout flags must be explicit booleans.') }
        if (-not $strings) { $reasons.Add('Audio error metadata and sound fault must be explicit strings.') }
        if ($numbers -and $booleans -and $strings) {
            $timing = -not $HostReport.timed_out -and $HostReport.frames -eq $RequestedFrames -and
                      [math]::Abs($HostReport.seconds - $expected) / $expected -lt 0.01
            if (-not $timing) { $reasons.Add('Requested frame count, deadline, or 1% pacing tolerance failed.') }
            $unavailable = -not $HostReport.audio_initial_device -and -not $HostReport.audio_device
            if ($unavailable) {
                $reasons.Add('No output endpoint opened; timing alone cannot satisfy the audio-required gate.')
            } elseif (-not $HostReport.audio_initial_device -or -not $HostReport.audio_device) {
                $reasons.Add('The output endpoint was unavailable at start or lost during the run.')
            } else {
                $audio = $HostReport.audio_rate -gt 0 -and $HostReport.samples -gt 0 -and
                         $HostReport.audio_errors -eq 0 -and $HostReport.audio_underruns -eq 0 -and
                         $HostReport.audio_min_queued_ms -gt 0 -and $HostReport.audio_error_stage -eq '' -and
                         $HostReport.audio_error_code -eq '00000000' -and $HostReport.sound_fault -eq ''
                if (-not $audio) { $reasons.Add('Audio generation, queue, error, underrun, or sound-board health failed.') }
            }
        }
    }
    $result = if ($timing -and $audio) { 'PASS' }
              elseif ($timing -and $unavailable -and $HostReport.sound_fault -eq '') { 'BLOCKED_AUDIO_UNAVAILABLE' }
              else { 'FAIL' }
    return [pscustomobject][ordered]@{
        result = $result
        timing_pass = $timing
        audio_required_pass = $audio
        reasons = $reasons.ToArray()
        frames_requested = $RequestedFrames
        frames = $HostReport.frames
        seconds = $HostReport.seconds
        expected_seconds = [math]::Round($expected, 6)
        audio_initial_device = $HostReport.audio_initial_device
        audio_device = $HostReport.audio_device
        audio_rate = $HostReport.audio_rate
        samples = $HostReport.samples
        audio_errors = $HostReport.audio_errors
        audio_error_stage = $HostReport.audio_error_stage
        audio_error_code = $HostReport.audio_error_code
        sound_fault = $HostReport.sound_fault
        audio_underruns = $HostReport.audio_underruns
        audio_min_queued_ms = $HostReport.audio_min_queued_ms
        present_max_ms = $HostReport.present_max_ms
        present_ms_hist = $HostReport.present_ms_hist
    }
}

if ($ValidateReport) {
    $h = Get-Content -LiteralPath $ValidateReport -Raw | ConvertFrom-Json
    $verdict = Get-PacingVerdict $h $Frames
    $verdict | ConvertTo-Json -Depth 5
    if ($verdict.result -ne 'PASS') { exit 1 }
    exit 0
}

if (!$Exe) { $Exe = Join-Path $Root 'build\msvc\stage\Windows-x64\SmashTV.exe' }
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $Root ('build\pacing-stress-' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh pacing output directory; existing evidence is preserved.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$Report = Join-Path $OutputDirectory 'host.json'
$seconds = [int]($Frames / 54.7) + 30
# Keep child variables literal inside the command string.
$burn = '$end = (Get-Date).AddSeconds(' + $seconds + '); while ((Get-Date) -lt $end) { $x = 0; for ($i = 0; $i -lt 100000; $i++) { $x += $i } }'
$procs = @()
$game = $null
$oldEnv = @{}
foreach ($name in @('STV_HOST_FRAMES','STV_HOST_NOPAUSE','STV_HOST_REPORT','STV_PROFILE')) {
    $oldEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    for ($i = 0; $i -lt $Burners; ++$i) {
        $procs += Start-Process pwsh -ArgumentList '-NoProfile', '-Command', $burn -WindowStyle Hidden -PassThru
    }
    Start-Sleep 3
    $env:STV_HOST_FRAMES = [string]$Frames
    $env:STV_HOST_NOPAUSE = '1'
    $env:STV_HOST_REPORT = $Report
    $env:STV_PROFILE = 'reference'
    $game = Start-Process $Exe -WorkingDirectory (Split-Path $Exe) -WindowStyle Hidden -PassThru
    if (-not $game.WaitForExit(($seconds + 30) * 1000)) {
        throw 'Player exceeded pacing-test deadline.'
    }
} finally {
    if ($null -ne $game -and -not $game.HasExited) { $game.Kill(); $game.WaitForExit() }
    foreach ($name in $oldEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $oldEnv[$name], 'Process')
    }
    $procs | ForEach-Object { if (-not $_.HasExited) { $_.Kill(); $_.WaitForExit() } }
}
$h = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
$verdict = Get-PacingVerdict $h $Frames
$verdict | Add-Member -NotePropertyName burners -NotePropertyValue $Burners
$verdict | Add-Member -NotePropertyName process_exit_code -NotePropertyValue $game.ExitCode
if ($game.ExitCode -ne 0) {
    $verdict.result = 'FAIL'
    $verdict.reasons += 'Game process returned a nonzero exit code.'
}
$verdict | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $OutputDirectory 'verdict.json')
if ($verdict.result -ne 'PASS') { exit 1 }
