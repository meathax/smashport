[CmdletBinding()]
param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
if (!$Destination) { $Destination = Join-Path (Split-Path $PSScriptRoot -Parent) 'original\smashtv' }
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) {
    throw 'Destination exists. It has not been modified. Audit an existing checkout in place; do not overwrite it.'
}
Get-Command git -ErrorAction Stop | Out-Null
& git clone --no-tags https://github.com/historicalsource/smashtv.git $Destination
if ($LASTEXITCODE -ne 0) { throw 'Source clone failed. Any partial directory is retained for inspection; no source pin has been claimed.' }
$Commit = (& git -C $Destination rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $Commit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot establish exact checkout commit.' }
$Record = [ordered]@{
    repository = 'https://github.com/historicalsource/smashtv'; commit = $Commit
    acquired_utc = [DateTime]::UtcNow.ToString('o'); source_configuration = 'UNVERIFIED'
    retail_revision_match = 'UNVERIFIED'; original_files_modified = $false
}
$Record | ConvertTo-Json | Set-Content -LiteralPath ($Destination + '.provenance.json') -Encoding UTF8
Write-Host "Original checkout preserved at $Destination; commit recorded as $Commit."
Write-Host 'A commit pin does not by itself establish the archive configuration or matching retail revision.'
