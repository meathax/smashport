[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Script,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$System = 'smashtv',
    [double]$Seconds = 80,
    [string]$Mame = 'D:\Arcade\AI\mameexe\mame.exe',
    [string]$RomPath = 'D:\Arcade\AI\MAME 0.289 ROMs (merged)',
    [string]$ExpectedSha256 = 'af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3',
    [string[]]$Extra = @()
)
# Development-only headless MAME 0.289 runner (PLAN.md P0). Each run gets a
# fresh cfg/nvram/diff/snap tree so no state leaks between runs. Never used by
# the shipping executable.
$ErrorActionPreference = 'Stop'
$Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Mame).Hash.ToLowerInvariant()
if ($Hash -ne $ExpectedSha256) { throw "Unpinned MAME executable: $Hash" }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$BuildRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) 'build'))
if (!$OutDir.StartsWith($BuildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "MAME output must be a child of $BuildRoot; refusing recursive cleanup of $OutDir"
}
if (Test-Path -LiteralPath $OutDir) { Remove-Item -Recurse -Force -LiteralPath $OutDir }
foreach ($Sub in 'cfg','nvram','diff','snap','inp','sta') { New-Item -ItemType Directory -Force -Path (Join-Path $OutDir $Sub) | Out-Null }
$ScriptPath = [IO.Path]::GetFullPath($Script)
$Log = Join-Path $OutDir 'mame.log'
$Arguments = @($System, '-rompath', $RomPath,
    '-cfg_directory', (Join-Path $OutDir 'cfg'), '-nvram_directory', (Join-Path $OutDir 'nvram'),
    '-diff_directory', (Join-Path $OutDir 'diff'), '-snapshot_directory', (Join-Path $OutDir 'snap'),
    '-input_directory', (Join-Path $OutDir 'inp'), '-state_directory', (Join-Path $OutDir 'sta'),
    '-noreadconfig', '-skip_gameinfo', '-video', 'none', '-sound', 'none', '-nothrottle',
    '-seconds_to_run', [string]$Seconds, '-autoboot_script', $ScriptPath) + $Extra
Push-Location $OutDir
try {
    # Byte-stable UTF-8 (no BOM) log under both Windows PowerShell and pwsh.
    $Lines = & $Mame @Arguments 2>&1 | ForEach-Object { "$_" }
    $Code = $LASTEXITCODE
    [IO.File]::WriteAllText($Log, (($Lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
} finally { Pop-Location }
[pscustomobject]@{ exit = $Code; log = $Log; mame_sha256 = $Hash; system = $System; script = $ScriptPath } | ConvertTo-Json -Compress
if ($Code -ne 0) { exit $Code }
