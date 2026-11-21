# Windows process-boundary tests for tools/host_soak.ps1. Run with Windows
# PowerShell 5.1 (the synthetic fixture uses the installed .NET Framework).
# Every capture stays under a fresh ignored build directory; no game is run.
[CmdletBinding()]
param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Wrapper = Join-Path $ProjectRoot 'tools\host_soak.ps1'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot ('build\test-host-soak-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw "Preserve existing regression directory: $OutputDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$FixtureExe = Join-Path $OutputDirectory 'host-soak-fixture.exe'
Add-Type -Path (Join-Path $PSScriptRoot 'host_soak_fixture.cs') -OutputAssembly $FixtureExe -OutputType ConsoleApplication -ReferencedAssemblies @('System.dll','System.Core.dll','System.Web.Extensions.dll')
$Results = [Collections.Generic.List[object]]::new()
$SuiteError = $null
$EnvironmentKeys = @('STV_HOST_FRAMES','STV_HOST_REPORT','STV_HOST_INPUTS','STV_HOST_NOPAUSE','STV_HOST_UNRELATED',
    'STV_PROFILE','STV_NET','STV_NET_INPUTS','STV_NET_FRAMES','STV_NET_FAULT','STV_NET_SHAPE',
    'STV_TEST_HOST_CASE','STV_TEST_POSITION','STV_TEST_SOURCE')
$SavedEnvironment = @{}
foreach ($key in $EnvironmentKeys) { $SavedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key) }
function Run-Case([string]$Name,[bool]$Success,[string]$ExpectedError='', [hashtable]$Overrides=@{},[string]$Mode='', [string]$Script=$Wrapper) {
    $inputPath = Join-Path $OutputDirectory ($Name + '.inputs')
    $positionPath = Join-Path $OutputDirectory ($Name + '-position.json')
    [IO.File]::WriteAllText($inputPath,'0 0 0'+[Environment]::NewLine)
    [IO.File]::WriteAllText($positionPath,'{"frames":10,"circuit":0,"wave":1,"p1_lives":3}')
    $arguments = @{
        Frames=10;SampleSeconds=0.05;WarmupSeconds=0.05;MinimumSeconds=0.2;DeadlineSeconds=3
        Inputs=$inputPath;Name=$Name;OutputDirectory=(Join-Path $OutputDirectory $Name);Exe=$FixtureExe
        Profile='reference';ExpectedPosition=$positionPath
    }
    foreach ($key in $Overrides.Keys) { $arguments[$key] = $Overrides[$key] }
    $env:STV_TEST_HOST_CASE = if ($Mode) { $Mode } else { $Name }
    $env:STV_TEST_POSITION = $arguments.ExpectedPosition
    $errorText = $null
    try { $null = & $Script @arguments } catch { $errorText = $_.Exception.Message }
    $passed = ($Success -eq ($null -eq $errorText))
    if (!$Success -and $ExpectedError -and $errorText -notlike ('*' + $ExpectedError + '*')) { $passed = $false }
    foreach ($key in $EnvironmentKeys | Where-Object { $_ -notlike 'STV_TEST_*' }) {
        if ([Environment]::GetEnvironmentVariable($key) -ne ('parent-sentinel-' + $key)) {
            $passed = $false
            $errorText = "$errorText; wrapper changed parent environment $key"
        }
    }
    $receiptPath = Join-Path $arguments.OutputDirectory 'capture.json'
    $receipt = $null
    if (Test-Path -LiteralPath $receiptPath) {
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
        if ($receipt.release_gate_proof -ne $false -or $receipt.result -ne $(if ($Success) {'CAPTURE_COMPLETE'} else {'CAPTURE_FAILED'})) { $passed = $false }
        if ($receipt.process_id -and (Get-Process -Id $receipt.process_id -ErrorAction SilentlyContinue)) {
            $passed = $false
            $errorText = "$errorText; owned child still running"
        }
        if ($Success -and ($receipt.trend.samples -lt 2 -or $receipt.host.fixture_environment_clean -ne $true)) { $passed = $false }
        if ($Name -eq 'audio_underrun' -and ($receipt.position_matched -ne $true -or $receipt.trend.samples -lt 2)) {
            $passed = $false
            $errorText += '; missing independent endpoint or resource observations'
        }
    } elseif ($Success) { $passed = $false; $errorText = 'Successful invocation omitted its terminal receipt' }
    $Results.Add([pscustomobject][ordered]@{case=$Name;expected_success=$Success;result=$(if ($passed) {'PASS'} else {'FAIL'});error=$errorText;capture=$(if ($receipt) {$receiptPath} else {$null})})
    Write-Host ("{0}: {1}" -f $(if ($passed) {'PASS'} else {'FAIL'}),$Name)
}
try {
    foreach ($key in $EnvironmentKeys | Where-Object { $_ -notlike 'STV_TEST_*' }) {
        [Environment]::SetEnvironmentVariable($key,('parent-sentinel-' + $key),'Process')
    }
    Run-Case 'healthy' $true
    Run-Case 'default_attract' $true '' @{Inputs=''} 'healthy'
    Run-Case 'enhanced' $true '' @{Profile='enhanced'} 'healthy'
    Run-Case 'batched' $true
    Run-Case 'rounded_max' $true
    Run-Case 'uncapped' $true
    Run-Case 'uncapped_hidden' $true

    # Explicit relative paths follow the PowerShell provider location even
    # when the process cwd differs (as it does in remote shells). Keep a
    # protected capture at the wrong destination to detect old GetFullPath.
    $providerLocation = Join-Path $OutputDirectory 'provider-location'
    $processLocation = Join-Path $OutputDirectory 'process-location'
    $wrongDestination = Join-Path $processLocation 'relative-capture'
    New-Item -ItemType Directory -Path $providerLocation | Out-Null
    New-Item -ItemType Directory -Path $wrongDestination -Force | Out-Null
    $wrongSentinel = Join-Path $wrongDestination 'preserve.txt'
    [IO.File]::WriteAllText($wrongSentinel,'wrong-location capture must survive')
    $savedProcessLocation = [Environment]::CurrentDirectory
    Push-Location -LiteralPath $providerLocation
    try {
        [Environment]::CurrentDirectory = $processLocation
        Run-Case 'relative_output_provider_location' $true '' @{OutputDirectory='relative-capture'} 'healthy'
        $relativeResult = $Results[$Results.Count - 1]
        $relativeResult | Add-Member -NotePropertyName path_resolution -NotePropertyValue ([ordered]@{
            powershell_location=(Get-Location).Path;process_current_directory=[Environment]::CurrentDirectory
            relative_argument='relative-capture';expected_capture=(Join-Path $providerLocation 'relative-capture\capture.json')
            protected_wrong_destination=$wrongSentinel
        })
        if ([IO.File]::ReadAllText($wrongSentinel) -ne 'wrong-location capture must survive' -or
            (Test-Path -LiteralPath (Join-Path $wrongDestination 'capture.json')) -or
            (Get-Location).Path -eq [Environment]::CurrentDirectory) {
            $relativeResult.result = 'FAIL'
            $relativeResult.error = 'Relative capture used or modified the process cwd instead of the PowerShell location'
        }
        if ($relativeResult.capture) { $relativeResult.capture = Join-Path $providerLocation 'relative-capture\capture.json' }
    } finally {
        [Environment]::CurrentDirectory = $savedProcessLocation
        Pop-Location
    }
    $negative = @(
        @('missing','Host exited without its report'),
        @('malformed',''),
        @('missing_field','Invalid integer field: host.frame_number'),
        @('wrong_type','Invalid integer field: host.frames'),
        @('null_field','Invalid integer field: host.frames'),
        @('wrong_bool','Invalid boolean field: host.audio_device'),
        @('exit','Host exited with code 2'),
        @('frames','Host did not complete requested end frame'),
        @('extra_frames','Host did not complete requested end frame'),
        @('frame_number','Host did not complete requested end frame'),
        @('timed_out','Host did not complete requested end frame'),
        @('faults','Unexpected main/sound fault count'),
        @('sound','Unexpected main/sound fault count'),
        @('fault_address','Unexpected POST fault boundary'),
        @('fault_boundary','Unexpected POST fault boundary'),
        @('no_audio','Host audio unavailable'),
        @('initial_audio','Host audio unavailable'),
        @('queue_unmeasured','Host audio unavailable'),
        @('audio_error','Host audio unavailable'),
        @('audio_stage','Host audio unavailable'),
        @('audio_underrun','Host audio unavailable'),
        @('audio_samples','Host audio unavailable'),
        @('audio_rate','Host audio unavailable'),
        @('profile','Actual host profile/geometry differs'),
        @('width','Actual host profile/geometry differs'),
        @('height','Actual host profile/geometry differs'),
        @('histogram','Missing or inconsistent present histogram'),
        @('histogram_zero','Missing or inconsistent present histogram'),
        @('histogram_count','Invalid integer field: host.present_ms_hist'),
        @('histogram_negative','Invalid integer field: host.present_ms_hist'),
        @('histogram_max','Missing or inconsistent present histogram'),
        @('uncapped_schema','Unsupported presentation report schema'),
        @('uncapped_missing_count','Invalid integer field: host.presentation_frames'),
        @('uncapped_count','Missing or inconsistent present histogram'),
        @('uncapped_zero_visible','Visible host did not complete enough actual presentations'),
        @('uncapped_mode','Unknown presentation mode'),
        @('uncapped_paint_error','Presentation reported a failed GDI draw'),
        @('uncapped_clock','Simulation clock changed'),
        @('uncapped_rebased','Simulation clock changed'),
        @('uncapped_simulation_duration','Simulation duration does not match'),
        @('uncapped_audio_ratio','Device output correction is absent or out of bounds'),
        @('uncapped_no_output','Device output correction is absent or out of bounds'),
        @('uncapped_missing_resync','Invalid integer field: host.audio_output_resyncs'),
        @('uncapped_resync','Host output backlog was dropped or resynchronized'),
        @('uncapped_source_drop','Host output backlog was dropped or resynchronized'),
        @('uncapped_device_drop','Host output backlog was dropped or resynchronized'),
        @('uncapped_pending_limit','Host output backlog exceeded its bounded queue'),
        @('uncapped_content','Content viewport differs'),
        @('uncapped_no_capture','Content viewport differs'),
        @('uncapped_offcenter','Content destination is not centered'),
        @('duration','Host duration does not meet'),
        @('wall_clock','Host duration does not meet'),
        @('position','Lockstep end position differs at wave'),
        @('mutated_input','Input changed during capture'),
        @('mutated_position','Expected lockstep position changed during capture')
    )
    foreach ($case in $negative) { Run-Case $case[0] $false $case[1] }
    Run-Case 'deadline' $false 'Host exceeded external deadline' @{DeadlineSeconds=0.9}
    Run-Case 'warmup' $false 'Insufficient resource samples after warmup' @{WarmupSeconds=2} 'healthy'
    Run-Case 'name_preflight' $false 'Name must be a simple filename' @{Name='../escape'} 'healthy'
    $existing = Join-Path $OutputDirectory 'existing_output'
    New-Item -ItemType Directory -Path $existing | Out-Null
    [IO.File]::WriteAllText((Join-Path $existing 'preserve.txt'),'original capture')
    Run-Case 'existing_output' $false 'Preserve existing capture directory' @{} 'healthy'
    if ([IO.File]::ReadAllText((Join-Path $existing 'preserve.txt')) -ne 'original capture') { throw 'Existing capture was modified' }

    # A tiny isolated repository tests the real frozen-commit contract without
    # changing, cleaning or committing the user's project.
    $fixtureRepository = Join-Path $OutputDirectory 'fixture-repository'
    New-Item -ItemType Directory -Path (Join-Path $fixtureRepository 'tools') | Out-Null
    $frozenWrapper = Join-Path $fixtureRepository 'tools\host_soak.ps1'
    Copy-Item -LiteralPath $Wrapper -Destination $frozenWrapper
    & git init -q $fixtureRepository
    if ($LASTEXITCODE) { throw 'Cannot initialize isolated fixture repository' }
    & git -C $fixtureRepository add -- tools/host_soak.ps1
    & git -C $fixtureRepository -c user.name=HostSoakFixture -c user.email=host-soak-fixture@example.invalid commit -qm 'Synthetic capture contract fixture'
    if ($LASTEXITCODE) { throw 'Cannot commit isolated fixture repository' }
    $fixtureCommit = (& git -C $fixtureRepository rev-parse HEAD).Trim()
    $fixtureHash = (Get-FileHash -LiteralPath $FixtureExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $frozen = @{FrozenCommit=$fixtureCommit;FrozenExeSha256=$fixtureHash}
    Run-Case 'frozen_healthy' $true '' $frozen 'healthy' $frozenWrapper
    Run-Case 'freeze_pair' $false 'Frozen capture requires both full commit' @{FrozenCommit=$fixtureCommit} 'healthy' $frozenWrapper
    Run-Case 'sha_preflight' $false 'Executable does not match frozen SHA-256' @{FrozenCommit=$fixtureCommit;FrozenExeSha256=('f'*64)} 'healthy' $frozenWrapper
    Run-Case 'commit_preflight' $false 'Frozen capture requires the exact clean project commit' @{FrozenCommit=('f'*40);FrozenExeSha256=$fixtureHash} 'healthy' $frozenWrapper
    Run-Case 'freeze_input' $false 'Frozen capture requires explicit input' @{FrozenCommit=$fixtureCommit;FrozenExeSha256=$fixtureHash;Inputs=''} 'healthy' $frozenWrapper
    Run-Case 'freeze_position' $false 'Frozen capture requires explicit input' @{FrozenCommit=$fixtureCommit;FrozenExeSha256=$fixtureHash;ExpectedPosition=''} 'healthy' $frozenWrapper
    Run-Case 'freeze_duration' $false 'Frozen capture requires explicit input' @{FrozenCommit=$fixtureCommit;FrozenExeSha256=$fixtureHash;MinimumSeconds=0} 'healthy' $frozenWrapper
    $env:STV_TEST_SOURCE = Join-Path $fixtureRepository 'untracked-during-capture.txt'
    Run-Case 'frozen_dirty_during' $false 'Frozen project working tree changed during capture' $frozen 'mutated_source' $frozenWrapper
    Run-Case 'frozen_dirty_before' $false 'Frozen capture requires the exact clean project commit' $frozen 'healthy' $frozenWrapper
    $env:STV_TEST_SOURCE = $frozenWrapper
    Run-Case 'collector_mutation' $false 'Capture collector changed during capture' @{} 'mutated_collector' $frozenWrapper
} catch {
    $SuiteError = $_.Exception.Message
} finally {
    foreach ($key in $EnvironmentKeys) { [Environment]::SetEnvironmentVariable($key,$SavedEnvironment[$key],'Process') }
    $failed = @($Results.ToArray() | Where-Object { $_.result -eq 'FAIL' })
    if ($Results.Count -ne 77 -and !$SuiteError) { $SuiteError = 'Incomplete fixture regression suite' }
    $summary = [ordered]@{schema='stv.host-capture-tests.v1';result=$(if ($failed.Count -eq 0 -and !$SuiteError) {'PASS'} else {'FAIL'})
        fixture_only=$true;release_gate_proof=$false;tests=$Results.Count;failures=$failed.Count;suite_error=$SuiteError
        wrapper_sha256=(Get-FileHash -LiteralPath $Wrapper -Algorithm SHA256).Hash.ToLowerInvariant()
        fixture_source_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'host_soak_fixture.cs') -Algorithm SHA256).Hash.ToLowerInvariant()
        cases=$Results.ToArray()}
    $summary | ConvertTo-Json -Depth 8 | Out-File (Join-Path $OutputDirectory 'tests.json') -Encoding utf8
}
Write-Host "$($Results.Count) capture checks; $($failed.Count) failed. Report: $OutputDirectory\tests.json"
if ($SuiteError) { throw "Host capture regression suite failed: $SuiteError" }
if ($failed.Count) { throw "$($failed.Count) host capture regressions failed" }
