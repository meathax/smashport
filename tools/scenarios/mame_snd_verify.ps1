# Development: record MAME 0.289's sound-board access stream (tools/lockstep/snd_record.lua)
# for an input file (PLAN P5 gate: sound-access hash equal on the soak scenarios), for
# comparison with stv_l8_lockstep --snd over the same inputs (sndfile.py diff). Run detached:
#   Start-Process pwsh -WindowStyle Hidden -ArgumentList '-File','tools/scenarios/mame_snd_verify.ps1','-Inputs','x.inputs','-OutDir','build/x','-Frames','N'
param(
    [Parameter(Mandatory)][string]$Inputs,
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][int]$Frames,
    [int]$From = -1, [int]$To = -2   # frame window for full access records
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$Out = [IO.Path]::GetFullPath($OutDir)
# Out-File opens the pipeline log before mame_run creates its output directory.
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Out)) | Out-Null
$env:STV_SND_OUT = Join-Path $Out 'snd.bin'
$env:STV_LS_FRAMES = [string]$Frames
$env:STV_LS_INPUTS = (Resolve-Path $Inputs).Path
$env:STV_SND_FROM = [string]$From
$env:STV_SND_TO = [string]$To
# emulated seconds: frames at about 54.7 Hz, with margin
$Seconds = [math]::Ceiling($Frames / 50.0) + 600
& (Join-Path $Root 'tools\mame_run.ps1') -Script tools/lockstep/snd_record.lua -OutDir $OutDir -Seconds $Seconds *>&1 |
    Out-File -FilePath ($Out + '.log')
