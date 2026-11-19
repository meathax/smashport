[CmdletBinding()]
param(
    [ValidateSet('ClangCross','MSVC')][string]$Mode = 'MSVC',
    [string]$BuildDirectory = '',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$PlayerRelease, # compatibility alias for the sole player build
    [string]$Rom,
    [switch]$Play,
    [switch]$Lab   # also build the retired v0.5 hand-written lab code (STV_LAB)
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
if ($Mode -eq 'ClangCross') {
    throw 'ClangCross targets the retired source lab, not the translated L8 game. Use -Mode MSVC for SmashTV.exe; tools/cross_windows.py is historical diagnostic tooling only.'
}
if (!$BuildDirectory) { $BuildDirectory = Join-Path $Root $(if ($Lab) { 'build/msvc-lab' } else { 'build/rom-player' }) }
# Resolve relative paths against PowerShell's location, even when the process cwd differs.
$BuildDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$Exe = Join-Path $BuildDirectory 'SmashTV.exe'
$Cache = Join-Path $BuildDirectory 'selected-l8.zip'
$Selection = Join-Path $BuildDirectory 'rom-selection.json'
$InteractiveSelection = $false
if (!$Lab -and !$Rom) {
    if ((Test-Path -LiteralPath $Selection) -and (Test-Path -LiteralPath $Cache)) {
        try { $Saved = Get-Content -Raw -LiteralPath $Selection | ConvertFrom-Json } catch { $Saved = $null }
        if ($Saved -and $Saved.rom -eq (Get-FileHash -LiteralPath $Cache).Hash) {
            $Rom = $Cache
            if ($Play -and (Test-Path -LiteralPath $Exe) -and $Saved.player -eq (Get-FileHash -LiteralPath $Exe).Hash) {
                $env:STV_HOST_ROM_ZIP = $Cache
                Start-Process -FilePath $Exe -WorkingDirectory $BuildDirectory
                return
            }
        }
    }
    if (!$Rom) {
        Add-Type -AssemblyName System.Windows.Forms
        $Picker = New-Object System.Windows.Forms.OpenFileDialog
        $Picker.Title = 'Set up Smash TV - choose your retail L8 ROM ZIP'
        $Picker.Filter = 'ROM archives (*.zip)|*.zip'
        try {
            if ($Picker.ShowDialog() -ne 'OK') { return }
            $Rom = $Picker.FileName
            $InteractiveSelection = $true
        } finally { $Picker.Dispose() }
    }
}
if ($Mode -eq 'MSVC') {
    # Configure and build in the same x64 developer environment. Ninja avoids
    # MSBuild's duplicate Path/PATH environment failure seen on this host.
    $CMake = (Get-Command cmake -ErrorAction Stop).Source
    Get-Command ninja -ErrorAction Stop | Out-Null
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $VsWhere -PathType Leaf)) { throw 'Visual Studio installer/vswhere.exe not found.' }
    $Install = (& $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (!$Install) { throw 'Visual Studio C++ x64 tools not found.' }
    $VsDevCmd = Join-Path $Install 'Common7\Tools\VsDevCmd.bat'
    if (!(Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) { throw 'VsDevCmd.bat not found.' }

    $PlayerInputs = ''
    if (!$Lab) {
        New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
        $Result = Join-Path $BuildDirectory ('import-' + [Guid]::NewGuid().ToString('N') + '.txt')
        try {
            & python (Join-Path $Root 'tools/player/prepare.py') --rom $Rom --cache $Cache --generated (Join-Path $Root 'build/player-inputs') --result $Result
            if ($LASTEXITCODE -ne 0) { throw 'ROM setup failed. Select a complete retail L8 set.' }
            $PlayerInputs = Get-Content -Encoding utf8 -Raw -LiteralPath $Result
        } finally { if (Test-Path -LiteralPath $Result) { Remove-Item -LiteralPath $Result } }
    } else {
        New-Item -ItemType Directory -Force -Path (Join-Path $Root 'build\p1') | Out-Null
        & python (Join-Path $Root 'tools\gsp2cpp\asm.py') link --mem ROM2=FFFF7000 `
            --out (Join-Path $Root 'build\p1\robo.bin') `
            --map (Join-Path $Root 'build\p1\robo.map.json') `
            --listing (Join-Path $Root 'build\p1\listing.json') `
            --report (Join-Path $Root 'build\p1\report.json')
        if ($LASTEXITCODE -ne 0) { throw 'P1 source-alignment prerequisites failed.' }
        New-Item -ItemType Directory -Force -Path (Join-Path $Root 'generated\private') | Out-Null
        & python (Join-Path $Root 'tools\gsp2cpp\codemap.py') `
            --listing (Join-Path $Root 'build\p1\listing.json') `
            --map (Join-Path $Root 'build\p1\robo.map.json') `
            --out (Join-Path $Root 'generated\private\l8_codemap.json')
        if ($LASTEXITCODE -ne 0) { throw 'Oracle code-map generation failed.' }
        & python (Join-Path $Root 'tools/gsp2cpp/ensure_emission.py') --refresh
        if ($LASTEXITCODE -ne 0) { throw 'Oracle emission failed.' }
        & python (Join-Path $Root 'tools/m6809_aot/emit6809.py') `
            (Join-Path $Root 'original\user-supplied-roms\smashtv.zip')
        if ($LASTEXITCODE -ne 0) { throw 'Oracle sound emission failed.' }
    }
    if (!$Lab -and
        (!(Test-Path -LiteralPath (Join-Path $Root 'build\p1\listing.json')) -or
         !(Test-Path -LiteralPath (Join-Path $Root 'build\p1\robo.map.json')))) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Root 'build\p1') | Out-Null
        & python (Join-Path $Root 'tools\gsp2cpp\asm.py') link --mem ROM2=FFFF7000 `
            --out (Join-Path $Root 'build\p1\robo.bin') `
            --map (Join-Path $Root 'build\p1\robo.map.json') `
            --listing (Join-Path $Root 'build\p1\listing.json') `
            --report (Join-Path $Root 'build\p1\report.json')
        if ($LASTEXITCODE -ne 0) { throw 'P1 source-alignment prerequisites failed.' }
    }
    $Configure = 'call "' + $VsDevCmd + '" -no_logo -arch=x64 -host_arch=x64 && "' + $CMake + '" -S "' + $Root + '" -B "' + $BuildDirectory + '" -G Ninja -DCMAKE_BUILD_TYPE=' + $Configuration + ' -DCMAKE_TRY_COMPILE_CONFIGURATION=' + $Configuration + ' -DSTV_PLAYER_INPUTS="' + $PlayerInputs + '" -DSTV_PLAYER_RELEASE=' + $(if ($Lab) { 'OFF' } else { 'ON' }) + ' -DSTV_LAB=' + $(if ($Lab) { 'ON' } else { 'OFF' })
    $Target = if ($Play -and !$Lab) { ' --target SmashTV' } else { '' }
    & $env:ComSpec /d /c ($Configure + ' && "' + $CMake + '" --build "' + $BuildDirectory + '"' + $Target + ' --parallel 4')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    if (!$Lab) {
        @{rom=(Get-FileHash -LiteralPath $Cache).Hash; player=(Get-FileHash -LiteralPath $Exe).Hash} |
            ConvertTo-Json | Set-Content -LiteralPath $Selection -Encoding utf8
        Write-Host "Local game built from your ROM: $Exe"
        if ($Play) {
            $env:STV_HOST_ROM_ZIP = $Cache
            Start-Process -FilePath $Exe -WorkingDirectory $BuildDirectory
        }
    }
}
