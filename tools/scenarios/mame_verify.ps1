# Development: record a MAME 0.289 oracle stream for an input file (PLAN P5), for
# comparison with the native replay of the same inputs (lsfile.py diff).
# Summary mode (STV_LS_SUMMARY) keeps multi-hour scenarios small. Run detached:
#   Start-Process pwsh -WindowStyle Hidden -ArgumentList '-File','tools/scenarios/mame_verify.ps1','-Inputs','x.inputs','-OutDir','build/x','-Frames','N'
param(
    [Parameter(Mandatory)][string]$Inputs,
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][int]$Frames,
    [int]$DumpEvery = 60,
    [string]$Snaps = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$Out = [IO.Path]::GetFullPath($OutDir)
# Out-File opens the pipeline log before mame_run creates its output directory.
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Out)) | Out-Null
$env:STV_LS_OUT = Join-Path $Out 'ls.bin'
$env:STV_LS_FRAMES = [string]$Frames
$env:STV_LS_COMPACT = '1'
$env:STV_LS_SUMMARY = '1'
$env:STV_LS_DUMP_EVERY = [string]$DumpEvery
$env:STV_LS_INPUTS = (Resolve-Path $Inputs).Path
if ($Snaps) { $env:STV_LS_SNAPS = $Snaps } else { Remove-Item Env:STV_LS_SNAPS -ErrorAction SilentlyContinue }
Remove-Item Env:STV_LS_BOT -ErrorAction SilentlyContinue
# emulated seconds: frames at about 54.7 Hz, with margin
$Seconds = [math]::Ceiling($Frames / 50.0) + 600
& (Join-Path $Root 'tools\mame_run.ps1') -Script tools/lockstep/record.lua -OutDir $OutDir -Seconds $Seconds *>&1 |
    Out-File -FilePath ($Out + '.log')
