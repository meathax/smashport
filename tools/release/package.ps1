# Retain the packaging CLI so existing callers receive an explicit failure.
[CmdletBinding()]
param([string]$OutDir = '', [ValidatePattern('^\d+\.\d+(?:\.\d+)?(?:-[A-Za-z0-9.-]+)?$')][string]$Version = '1.0',
      [string]$StageDirectory = '', [switch]$DevelopmentRelease)
# The installed player statically links the programs generated from the selected ROM.
# A filename allowlist or a PE import audit cannot make that image distributable.
throw 'Packaging blocked: the locally generated SmashTV.exe contains retail game code. Only the clean installer may be distributed once standalone setup and its payload audit are ready.'
