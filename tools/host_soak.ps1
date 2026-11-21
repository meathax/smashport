# PLAN C7/P11: capture a bounded Windows host run without certifying the release.
# Fresh output directory: host.json, samples.csv and capture.json. Existing
# captures are never replaced. All STV automation settings are child-local.
[CmdletBinding()]
param(
    [ValidateRange(3,2147483647)][long]$Frames = 394000,
    [ValidateRange(0.01,3600)][double]$SampleSeconds = 60,
    [ValidateRange(0,1000000)][double]$WarmupSeconds = 120,
    [ValidateRange(0,1000000)][double]$MinimumSeconds = 0,
    [ValidateRange(0,2000000)][double]$DeadlineSeconds = 0,
    [string]$Inputs = '',
    [string]$Name = 'host_soak',
    [string]$OutputDirectory = '',
    [string]$Exe = '',
    [ValidateSet('reference','enhanced')][string]$Profile = 'reference',
    [ValidateRange(0,1000000)][int]$ExpectedMainFaults = 121,
    [string]$ExpectedPosition = '',
    [string]$FrozenCommit = '',
    [string]$FrozenExeSha256 = ''
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
function Pin-File([string]$Path) {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.PSIsContainer) { throw "Expected a file: $Path" }
    return [ordered]@{path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Repo-State {
    $head = & git -C $ProjectRoot rev-parse HEAD 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the project commit' }
    $changes = @(& git -C $ProjectRoot status --porcelain=v1 --untracked-files=all 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect project working-tree state' }
    return [ordered]@{commit=([string]$head).Trim();changes=$changes}
}
function Require-Int($Value,[string]$Label,[long]$Minimum=0,[long]$Maximum=[long]::MaxValue) {
    if (($Value -isnot [int] -and $Value -isnot [long]) -or $Value -lt $Minimum -or $Value -gt $Maximum) {
        throw "Invalid integer field: $Label"
    }
}
function Require-Number($Value,[string]$Label,[double]$Minimum=0) {
    if (($Value -isnot [double] -and $Value -isnot [single] -and $Value -isnot [decimal] -and $Value -isnot [int] -and $Value -isnot [long]) -or
        [double]::IsNaN([double]$Value) -or [double]::IsInfinity([double]$Value) -or [double]$Value -lt $Minimum) {
        throw "Invalid numeric field: $Label"
    }
}
function Require-Bool($Value,[string]$Label) { if ($Value -isnot [bool]) { throw "Invalid boolean field: $Label" } }
function Require-String($Value,[string]$Label) { if ($Value -isnot [string]) { throw "Invalid string field: $Label" } }
function Write-Receipt($Value,[string]$Path) {
    $temporary = $Path + '.tmp'
    [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}
if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw 'Name must be a simple filename' }
$Frozen = [bool]$FrozenCommit -or [bool]$FrozenExeSha256
if ([bool]$FrozenCommit -xor [bool]$FrozenExeSha256) { throw 'Frozen capture requires both full commit and executable SHA-256' }
if ($Frozen -and ($FrozenCommit -notmatch '^[0-9a-fA-F]{40}$' -or $FrozenExeSha256 -notmatch '^[0-9a-fA-F]{64}$')) {
    throw 'Frozen capture requires a full 40-character commit and 64-character executable SHA-256'
}
if ($Frozen -and (!$Inputs -or !$ExpectedPosition -or !$PSBoundParameters.ContainsKey('MinimumSeconds') -or $MinimumSeconds -le 0)) {
    throw 'Frozen capture requires explicit input, expected lockstep position and minimum duration'
}
if (!$Exe) { $Exe = Join-Path $ProjectRoot 'build\msvc\stage\Windows-x64\SmashTV.exe' }
$ExePin = Pin-File $Exe
$CollectorPin = Pin-File $PSCommandPath
# An empty, explicit scenario gives reproducible attract mode, disables live
# gameplay input and keeps the player's existing CMOS untouched.
if (!$Inputs) { $Inputs = Join-Path $ProjectRoot 'tests\scenarios\attract_soak.inputs' }
$InputPin = Pin-File $Inputs
$PositionPin = $null
$Position = $null
if ($ExpectedPosition) {
    $PositionPin = Pin-File $ExpectedPosition
    $Position = Get-Content -LiteralPath $PositionPin.path -Raw | ConvertFrom-Json
    foreach ($key in @('frames','circuit','wave','p1_lives')) { Require-Int $Position.$key "expected position.$key" }
    if ($Position.frames -ne $Frames) { throw 'Expected lockstep position has a different frame count' }
    if ($Position.PSObject.Properties['profile'] -and $Position.profile -ne $Profile -and
        $Position.profile -ne $(if ($Profile -eq 'enhanced') {'enhanced_l8_16_9'} else {'reference_l8'})) {
        throw 'Expected lockstep position has a different profile'
    }
}
$Before = Repo-State
if ($Frozen -and ($Before.commit -ne $FrozenCommit.ToLowerInvariant() -or $Before.changes.Count -ne 0)) {
    throw 'Frozen capture requires the exact clean project commit'
}
if ($Frozen -and $ExePin.sha256 -ne $FrozenExeSha256.ToLowerInvariant()) { throw 'Executable does not match frozen SHA-256' }
if ($MinimumSeconds -eq 0) { $MinimumSeconds = $Frames / 54.7068 * 0.98 }
if ($DeadlineSeconds -eq 0) { $DeadlineSeconds = [Math]::Max(30 + $Frames / 50.0,30 + $MinimumSeconds * 1.15) }
if ($DeadlineSeconds -le $MinimumSeconds) { throw 'External deadline must exceed the requested minimum duration' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot ('build\' + $Name) }
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw "Preserve existing capture directory: $OutputDirectory" }
$parentDirectory = Split-Path -Parent $OutputDirectory
New-Item -ItemType Directory -Path $parentDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$HostPath = Join-Path $OutputDirectory 'host.json'
$SamplesPath = Join-Path $OutputDirectory 'samples.csv'
$CapturePath = Join-Path $OutputDirectory 'capture.json'
# The host currently reads STV_HOST_REPORT and STV_HOST_INPUTS into MAX_PATH.
if ($HostPath.Length -ge 260 -or $InputPin.path.Length -ge 260) { throw 'Host capture/input path exceeds the current Windows host MAX_PATH contract' }
[IO.File]::WriteAllText($SamplesPath,'seconds,working_set_bytes,private_bytes,handles,cpu_seconds'+[Environment]::NewLine,[Text.Encoding]::ASCII)
$Capture = [ordered]@{
    schema='stv.host-capture.v1';result='STARTING';error=$null
    verified_at=$Before.commit;working_tree_changes=$Before.changes
    frozen_inputs_requested=$Frozen;release_gate_proof=$false
    started_at_utc=[DateTime]::UtcNow.ToString('o');completed_at_utc=$null
    owner_pid=$PID;process_id=$null;exit_code=$null;wall_seconds=$null
    collector=$CollectorPin;exe=$ExePin;inputs=$InputPin;expected_position=$PositionPin
    requested=[ordered]@{frames=$Frames;minimum_seconds=$MinimumSeconds;sample_seconds=$SampleSeconds
        warmup_seconds=$WarmupSeconds;profile=$Profile;expected_main_faults=$ExpectedMainFaults;deadline_seconds=$DeadlineSeconds}
    samples=0;trend=$null;host=$null;after=$null;artifacts=$null
    limitations=@('Capture completion is not C7 certification or a memory/handle leak assessment.',
        'The host histogram measures completed GDI draws (schema 2), or historical presented batches; it is not monitor scanout proof.',
        'Known POST faults are checked by count and first boundary; full fault identities need separate reference evidence.')
}
Write-Receipt $Capture $CapturePath
$Samples = [Collections.Generic.List[object]]::new()
$Watch = [Diagnostics.Stopwatch]::new()
$Process = [Diagnostics.Process]::new()
$Process.StartInfo = [Diagnostics.ProcessStartInfo]::new()
$Process.StartInfo.FileName = $ExePin.path
$Process.StartInfo.WorkingDirectory = Split-Path -Parent $ExePin.path
$Process.StartInfo.UseShellExecute = $false
$Process.StartInfo.CreateNoWindow = $true
$Process.StartInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
# Never mutate the parent environment and never delete/read its shared TEMP report.
foreach ($key in @($Process.StartInfo.EnvironmentVariables.Keys)) {
    if ($key -like 'STV_HOST_*' -or $key -like 'STV_NET*' -or $key -eq 'STV_PROFILE') {
        $Process.StartInfo.EnvironmentVariables.Remove($key)
    }
}
$Process.StartInfo.EnvironmentVariables['STV_HOST_FRAMES'] = $Frames.ToString([Globalization.CultureInfo]::InvariantCulture)
$Process.StartInfo.EnvironmentVariables['STV_HOST_NOPAUSE'] = '1'
$Process.StartInfo.EnvironmentVariables['STV_HOST_REPORT'] = $HostPath
$Process.StartInfo.EnvironmentVariables['STV_HOST_INPUTS'] = $InputPin.path
$Process.StartInfo.EnvironmentVariables['STV_PROFILE'] = $Profile
$Started = $false
$Validated = $false
$Failure = $null
try {
    $Watch.Start()
    if (!$Process.Start()) { throw 'Host process did not start' }
    $Started = $true
    $Capture.process_id = $Process.Id
    $Capture.result = 'RUNNING'
    Write-Receipt $Capture $CapturePath
    $NextSample = 0.0
    while (!$Process.HasExited) {
        $elapsed = $Watch.Elapsed.TotalSeconds
        if ($elapsed -ge $DeadlineSeconds) { throw "Host exceeded external deadline of $DeadlineSeconds seconds" }
        if ($elapsed -ge $NextSample) {
            try {
                $Process.Refresh()
                if (!$Process.HasExited) {
                    $row = [pscustomobject][ordered]@{
                        seconds=[Math]::Round($Watch.Elapsed.TotalSeconds,6)
                        working_set_bytes=[long]$Process.WorkingSet64
                        private_bytes=[long]$Process.PrivateMemorySize64
                        handles=[int]$Process.HandleCount
                        cpu_seconds=[Math]::Round($Process.TotalProcessorTime.TotalSeconds,6)
                    }
                    if ($row.private_bytes -le 0 -or $row.working_set_bytes -le 0 -or $row.handles -lt 0) {
                        throw 'Invalid live process resource sample'
                    }
                    $Samples.Add($row)
                    $row | Export-Csv -LiteralPath $SamplesPath -Append -NoTypeInformation -Encoding ASCII
                }
            } catch {
                if (!$Process.HasExited) { throw }
            }
            $NextSample = $Watch.Elapsed.TotalSeconds + $SampleSeconds
        }
        $waitMs = [Math]::Max(1,[Math]::Min(100,[Math]::Ceiling(($NextSample - $Watch.Elapsed.TotalSeconds)*1000)))
        $null = $Process.WaitForExit([int]$waitMs)
    }
    $Process.WaitForExit()
    $Capture.exit_code = $Process.ExitCode
    if ($Process.ExitCode -ne 0) { throw "Host exited with code $($Process.ExitCode)" }
    if (!(Test-Path -LiteralPath $HostPath -PathType Leaf)) { throw 'Host exited without its report' }
    $h = Get-Content -LiteralPath $HostPath -Raw | ConvertFrom-Json
    $Capture.host = $h
    foreach ($key in @('frames','frame_number','main_faults','audio_rate','samples','audio_errors','audio_underruns','visible_width','visible_height','circuit','wave','p1_lives')) {
        Require-Int $h.$key "host.$key"
    }
    foreach ($key in @('timed_out','audio_device','audio_initial_device')) { Require-Bool $h.$key "host.$key" }
    foreach ($key in @('profile','sound_fault','first_fault','first_fault_addr','audio_error_stage','audio_error_code')) { Require-String $h.$key "host.$key" }
    Require-Number $h.seconds 'host.seconds'
    Require-Number $h.present_max_ms 'host.present_max_ms'
    Require-Number $h.audio_min_queued_ms 'host.audio_min_queued_ms' -1
    if ($h.frames -ne $Frames -or $h.frame_number -ne $Frames - 1 -or $h.timed_out) { throw 'Host did not complete requested end frame' }
    if ($h.main_faults -ne $ExpectedMainFaults -or $h.sound_fault -ne '') { throw 'Unexpected main/sound fault count' }
    if ($ExpectedMainFaults -gt 0) {
        if ($h.first_fault -ne 'read from unmapped address' -or $h.first_fault_addr -ne '00200000') { throw 'Unexpected POST fault boundary' }
    } elseif ($h.first_fault -ne '' -or $h.first_fault_addr -ne '00000000') { throw 'Unexpected first fault in zero-fault capture' }
    $afterWarmup = @($Samples.ToArray() | Where-Object { $_.seconds -ge $WarmupSeconds })
    if ($afterWarmup.Count -lt 2 -or $afterWarmup[-1].seconds -le $afterWarmup[0].seconds) {
        throw 'Insufficient resource samples after warmup; no memory/handle trend can be claimed'
    }
    $first = $afterWarmup[0]
    $last = $afterWarmup[-1]
    $Capture.trend = [ordered]@{
        samples=$afterWarmup.Count;start_seconds=$first.seconds;end_seconds=$last.seconds
        private_bytes_start=$first.private_bytes;private_bytes_end=$last.private_bytes;private_bytes_delta=$last.private_bytes-$first.private_bytes
        handles_start=$first.handles;handles_end=$last.handles;handles_delta=$last.handles-$first.handles
        assessment='UNASSESSED: endpoints are measurements, not a leak-freedom claim'
    }
    if ($Position) {
        foreach ($key in @('circuit','wave','p1_lives')) { if ($h.$key -ne $Position.$key) { throw "Lockstep end position differs at $key" } }
        $Capture.position_matched = $true
    }
    if (!$h.audio_initial_device -or !$h.audio_device -or $h.audio_errors -ne 0 -or $h.audio_underruns -ne 0 -or
        $h.audio_rate -le 0 -or $h.samples -le 0 -or $h.audio_min_queued_ms -lt 0 -or $h.audio_error_stage -ne '' -or $h.audio_error_code -ne '00000000') {
        throw ('Host audio unavailable, underrunning or failed during capture (stage={0}, code={1}, underruns={2})' -f $h.audio_error_stage,$h.audio_error_code,$h.audio_underruns)
    }
    $actualProfile = if ($Profile -eq 'enhanced') {'enhanced_l8_16_9'} else {'reference_l8'}
    $width = if ($Profile -eq 'enhanced') {547} else {410}
    if ($h.profile -ne $actualProfile -or $h.visible_width -ne $width -or $h.visible_height -ne 256) {
        throw 'Actual host profile/geometry differs from requested capture'
    }
    $presentationV2 = [bool]$h.PSObject.Properties['presentation_schema']
    $maximumBucket = $Frames
    if ($presentationV2) {
        Require-Int $h.presentation_schema 'host.presentation_schema'
        if ($h.presentation_schema -ne 2) { throw 'Unsupported presentation report schema' }
        foreach ($key in @('presentation_frames','presentation_failures','frame_captures',
            'content_width','content_height','presentation_client_width','presentation_client_height',
            'simulation_rate_numerator','simulation_rate_denominator','simulation_clock_rebases','audio_output_samples',
            'audio_output_resyncs','audio_dropped_source_samples','audio_dropped_device_frames','audio_max_pending_samples')) {
            Require-Int $h.$key "host.$key"
        }
        Require-Int $h.audio_pending_limit_samples 'host.audio_pending_limit_samples' 2
        foreach ($key in @('presentation_fps','simulation_hz','simulation_seconds','simulation_max_late_ms','audio_output_ratio')) {
            Require-Number $h.$key "host.$key"
        }
        Require-Bool $h.presentation_window_visible 'host.presentation_window_visible'
        Require-String $h.presentation_mode 'host.presentation_mode'
        if ($h.presentation_mode -notin @('uncapped','compositor_synced')) { throw 'Unknown presentation mode' }
        if ($h.presentation_failures -ne 0) { throw 'Presentation reported a failed GDI draw' }
        if ($h.simulation_rate_numerator -ne 8000000 -or $h.simulation_rate_denominator -ne 146234 -or
            $h.simulation_clock_rebases -ne 0) { throw 'Simulation clock changed during the scripted capture' }
        if ([Math]::Abs($h.simulation_seconds - $Frames * 146234.0 / 8000000.0) -gt 0.000001) {
            throw 'Simulation duration does not match completed arcade frames'
        }
        if ($h.audio_output_samples -le 0 -or $h.audio_output_ratio -lt 0.994999999 -or
            $h.audio_output_ratio -gt 1.005000001) { throw 'Device output correction is absent or out of bounds' }
        if ($h.audio_output_resyncs -ne 0 -or $h.audio_dropped_source_samples -ne 0 -or
            $h.audio_dropped_device_frames -ne 0) { throw 'Host output backlog was dropped or resynchronized' }
        if ($h.audio_max_pending_samples -gt $h.audio_pending_limit_samples) {
            throw 'Host output backlog exceeded its bounded queue'
        }
        if ($h.frame_captures -le 0 -or $h.frame_captures -gt $Frames -or
            $h.content_width -notin @(410,$width) -or $h.content_height -ne 256) {
            throw 'Content viewport differs from the completed scene'
        }
        if ($h.content_rect -isnot [array] -or $h.content_rect.Count -ne 4) { throw 'Missing content destination rectangle' }
        foreach ($coordinate in $h.content_rect) { Require-Int $coordinate 'host.content_rect' }
        if ($h.presentation_window_visible -and $h.presentation_frames -lt 2) {
            throw 'Visible host did not complete enough actual presentations'
        }
        if ($h.presentation_frames -gt 0) {
            $rect = $h.content_rect
            if ($rect[2] -le $rect[0] -or $rect[3] -le $rect[1] -or
                $rect[2] -gt $h.presentation_client_width -or $rect[3] -gt $h.presentation_client_height -or
                [Math]::Abs($rect[0] + $rect[2] - $h.presentation_client_width) -gt 1 -or
                [Math]::Abs($rect[1] + $rect[3] - $h.presentation_client_height) -gt 1) {
                throw 'Content destination is not centered inside the window'
            }
        }
        $maximumBucket = $h.presentation_frames
    }
    if ($h.present_ms_hist -isnot [array] -or $h.present_ms_hist.Count -ne 41) { throw 'Missing or inconsistent present histogram' }
    [long]$intervals = 0
    $lastBucket = -1
    for ($i = 0; $i -lt 41; ++$i) {
        Require-Int $h.present_ms_hist[$i] "host.present_ms_hist[$i]" 0 $maximumBucket
        $intervals += $h.present_ms_hist[$i]
        if ($h.present_ms_hist[$i] -gt 0) { $lastBucket = $i }
    }
    if ($presentationV2) {
        # Count actual completed paints. Uncapped presentation can produce many
        # more images than simulation frames; hidden captures honestly report 0.
        if ($intervals -ne [Math]::Max(0,$h.presentation_frames - 1)) {
            throw 'Missing or inconsistent present histogram'
        }
    } elseif ($intervals -le 0 -or $intervals -gt $Frames - 1) {
        # Preserve the strict pre-uncapped contract for historical executables.
        throw 'Missing or inconsistent present histogram'
    }
    if (($intervals -eq 0 -and $h.present_max_ms -ne 0) -or
        ($intervals -gt 0 -and ($h.present_max_ms + 0.011 -lt $lastBucket -or
        ($lastBucket -lt 40 -and $h.present_max_ms -ge $lastBucket + 1.011)))) {
        throw 'Missing or inconsistent present histogram'
    }
    $Capture.presentation_intervals = $intervals
    $wall = $Watch.Elapsed.TotalSeconds
    if ($h.seconds -le 0 -or $h.seconds + 0.02 -lt $MinimumSeconds -or $h.seconds -gt $wall + 0.1 -or
        $wall - $h.seconds -gt [Math]::Max(30,$h.seconds * 0.02)) { throw 'Host duration does not meet requested duration or wall-clock check' }
    $Validated = $true
} catch {
    $Failure = $_.Exception.Message
} finally {
    if (!$Validated -and !$Failure) { $Failure = 'Capture interrupted before validation completed' }
    if ($Started) {
        try {
            if (!$Process.HasExited) {
                $Process.Kill()
                if (!$Process.WaitForExit(5000)) { throw "Owned host process $($Process.Id) did not terminate" }
            }
            $Capture.exit_code = $Process.ExitCode
        } catch {
            $cleanupError = $_.Exception.Message
            $Capture.cleanup_error = $cleanupError
            $Failure = if ($Failure) { "$Failure; cleanup: $cleanupError" } else { $cleanupError }
        }
    }
    $Watch.Stop()
    $Capture.wall_seconds = $Watch.Elapsed.TotalSeconds
    $Capture.samples = $Samples.Count
    try {
        $afterCollector = Pin-File $CollectorPin.path
        $Capture.collector.after_sha256 = $afterCollector.sha256
        if ($afterCollector.sha256 -ne $CollectorPin.sha256) { throw 'Capture collector changed during capture' }
        $afterExe = Pin-File $ExePin.path
        $afterInput = Pin-File $InputPin.path
        $Capture.exe.after_sha256 = $afterExe.sha256
        $Capture.inputs.after_sha256 = $afterInput.sha256
        $Capture.after = Repo-State
        if ($afterExe.sha256 -ne $ExePin.sha256) { throw 'Executable changed during capture' }
        if ($afterInput.sha256 -ne $InputPin.sha256) { throw 'Input changed during capture' }
        if ($PositionPin) {
            $afterPosition = Pin-File $PositionPin.path
            $Capture.expected_position.after_sha256 = $afterPosition.sha256
            if ($afterPosition.sha256 -ne $PositionPin.sha256) { throw 'Expected lockstep position changed during capture' }
        }
        if ($Capture.after.commit -ne $Before.commit) { throw 'Project commit changed during capture' }
        if ($Frozen -and $Capture.after.changes.Count -ne 0) { throw 'Frozen project working tree changed during capture' }
    } catch {
        $pinError = $_.Exception.Message
        $Failure = if ($Failure) { "$Failure; final pins: $pinError" } else { $pinError }
    }
    $Capture.artifacts = [ordered]@{samples=(Pin-File $SamplesPath)}
    if (Test-Path -LiteralPath $HostPath -PathType Leaf) { $Capture.artifacts.host = Pin-File $HostPath }
    $Capture.completed_at_utc = [DateTime]::UtcNow.ToString('o')
    $Capture.result = if ($Failure) {'CAPTURE_FAILED'} else {'CAPTURE_COMPLETE'}
    $Capture.error = $Failure
    Write-Receipt $Capture $CapturePath
    $Process.Dispose()
}
if ($Failure) { throw "$Failure (receipt: $CapturePath)" }
[pscustomobject]@{result=$Capture.result;capture=$CapturePath;frames=$Capture.host.frames;seconds=$Capture.host.seconds;samples=$Capture.samples;release_gate_proof=$false}
