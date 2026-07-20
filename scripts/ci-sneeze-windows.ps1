# Copyright 2026 Metaversal Corporation. All rights reserved.
#
# Jenkins / CI entry point for standalone Sneeze Windows builds.
#
# Usage (Jenkins Execute Windows batch command):
#   pwsh -ExecutionPolicy Bypass -File scripts\ci-sneeze-windows.ps1 -Config Release
#
# What it does:
#   1. Fast-forward the workspace to origin/main (reset --hard).
#   2. Preflight: verify include/ matches src/ (console/network/storage API).
#   3. Delegate to build-windows.ps1 (pass -All for first-time deps, -Fresh -Rebuild for clean src).
#
# Prerequisites: VS 2022, CMake 3.24+, Git, Rust (for wasmtime), deps built or pass -All.

[CmdletBinding()]
param (
   [ValidateSet ('Debug', 'Release')]
   [string] $Config = 'Release',

   [switch] $SkipSync,
   [switch] $All,
   [switch] $Fresh,
   [switch] $Rebuild
)

$ErrorActionPreference = 'Stop'

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SneezeDir  = Resolve-Path (Join-Path $ScriptDir '..')

if (-not $SkipSync) {
   Write-Host ''
   Write-Host '============================================================'
   Write-Host '  Sync Sneeze (origin/main)'
   Write-Host '============================================================'
   Push-Location $SneezeDir
   try {
      git fetch origin main
      if ($LASTEXITCODE -ne 0) { throw 'git fetch failed' }
      git reset --hard origin/main
      if ($LASTEXITCODE -ne 0) { throw 'git reset --hard failed' }
      $sHead = git rev-parse --short HEAD
      Write-Host "  Sneeze HEAD = $sHead ($(git log -1 --format='%s'))"
   }
   finally {
      Pop-Location
   }
}

# Record HEAD after sync; wipe PCH when the commit changes (lighter than deleting
# the whole build tree every run).
$BuildDir = Join-Path $SneezeDir 'builds\windows-x64\build'
$HeadFile = Join-Path $BuildDir '.ci-sneeze-head'
$script:HeadChanged = $false
if (-not $SkipSync) {
   $sNewHead = (git -C $SneezeDir rev-parse HEAD).Trim()
   $sOldHead = ''
   if (Test-Path $HeadFile) {
      $sOldHead = (Get-Content -Raw $HeadFile).Trim()
   }
   if ($sOldHead -and ($sOldHead -ne $sNewHead)) {
      $script:HeadChanged = $true
      Write-Host "  Sneeze commit changed ($($sOldHead.Substring(0,7)) -> $($sNewHead.Substring(0,7))); clearing stale build state"
      if (Test-Path $BuildDir) {
         Remove-Item -Recurse -Force $BuildDir
      }
   }
   if (-not (Test-Path $BuildDir)) {
      New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
   }
   Set-Content -Path $HeadFile -Value $sNewHead -NoNewline
}

$buildArgs = @{
   Config  = $Config
}
if ($All)     { $buildArgs['All']     = $true }
if ($Fresh)   { $buildArgs['Fresh']   = $true }
if ($Rebuild) { $buildArgs['Rebuild'] = $true }

# Default CI path: configure fresh + full rebuild when caller did not specify mode.
if (-not ($All -or $Fresh -or $Rebuild)) {
   $buildArgs['Fresh']   = $true
   $buildArgs['Rebuild'] = $true
}

& "$ScriptDir\build-windows.ps1" @buildArgs
exit $LASTEXITCODE
