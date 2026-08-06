# Copyright 2026 Metaversal Corporation. All rights reserved.
#
# Jenkins / CI entry point for standalone Sneeze Windows builds.
#
# Usage (Jenkins Execute Windows batch command):
#   pwsh -ExecutionPolicy Bypass -File scripts\ci-sneeze-windows.ps1 -Config Release
#
# What it does:
#   1. Fast-forward the workspace to origin/main (reset --hard).
#   2. Configure DEP_GIT_TOKEN (if set) for private MetaversalCorp dep fetch.
#   3. -Verify deps against deps/dependencies.json (network freshness).
#   4. If anything is out of date / stale / unreachable, -Sync (moves checkouts
#      + rebuilds affected deps in Debug and Release).
#   5. ABI canary: if installed sneeze_abi.h is missing current symbols, force
#      -Only sneeze-sdk -Sync -Rebuild (catches "Verify OK but install stale").
#   6. Build Sneeze (default: -Fresh -Rebuild).
#
# Prerequisites: VS 2022, CMake 3.24+, Git, Rust (for wasmtime), Python 3
# (depgraph / verify scripts). Private deps need agent git auth OR env
# DEP_GIT_TOKEN (PAT with contents:read on MetaversalCorp private repos).

[CmdletBinding()]
param (
   [ValidateSet ('Debug', 'Release')]
   [string] $Config = 'Release',

   [switch] $SkipSync,
   [switch] $SkipDepVerify,
   [switch] $All,
   [switch] $Fresh,
   [switch] $Rebuild
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$SneezeDir   = Resolve-Path (Join-Path $ScriptDir '..')
$BuildScript = Join-Path $ScriptDir 'build-windows.ps1'
$ConfigLower = $Config.ToLowerInvariant()

# ---------------------------------------------------------------------------
# Private dep auth (sneeze-sdk / rmap / map / vox). Same idea as GHA.
# ---------------------------------------------------------------------------
if ($env:DEP_GIT_TOKEN) {
   $token = $env:DEP_GIT_TOKEN
   $prev = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try {
      $existing = @(git config --global --get-regexp '^url\.https://x-access-token:.*@github\.com/(MetaversalCorp/)?\.insteadof$' 2>$null)
      foreach ($line in $existing) {
         if ($line -match '^(url\..+\.insteadof)\s') {
            git config --global --unset-all $Matches[1] 2>$null | Out-Null
         }
      }
      # Scope to MetaversalCorp only so public remotes (anari-sdk, etc.) stay readable.
      git config --global "url.https://x-access-token:${token}@github.com/MetaversalCorp/.insteadOf" "https://github.com/MetaversalCorp/"
      Write-Host "DEP_GIT_TOKEN configured for MetaversalCorp deps (len=$($token.Length))"
   }
   finally {
      $ErrorActionPreference = $prev
   }
}
else {
   Write-Host "DEP_GIT_TOKEN not set — relying on agent git credentials for private deps"
}

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

# Record HEAD after sync; wipe build tree when the commit changes (lighter than
# deleting every run).
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

# ---------------------------------------------------------------------------
# Deps: Verify first; Sync only when something is out of date / stale.
# ---------------------------------------------------------------------------

if (-not $SkipDepVerify) {
   Write-Host ''
   Write-Host '============================================================'
   Write-Host '  Verify dependencies (deps/dependencies.json)'
   Write-Host '============================================================'
   & $BuildScript -Verify
   $verifyRc = $LASTEXITCODE
   if ($verifyRc -ne 0) {
      Write-Host ''
      Write-Host '============================================================'
      Write-Host '  Deps out of date — Sync (checkouts + Debug/Release rebuild)'
      Write-Host '============================================================'
      & $BuildScript -Sync
      if ($LASTEXITCODE -ne 0) {
         Write-Error 'Dependency -Sync failed'
         exit $LASTEXITCODE
      }
   }
   else {
      Write-Host '  Dependencies OK — skipping full -Sync'
   }
}

# ---------------------------------------------------------------------------
# SneezeSDK ABI canary — installed headers must match HostFunctions.cpp.
# Catches "checkout OK / stamp OK but install/include is stale" and the case
# where Verify could not see a private branch tip and skipped Sync.
# ---------------------------------------------------------------------------
function Test-SneezeSdkAbiInstalled {
   $abi = Join-Path $SneezeDir "deps\builds\windows-x64\$ConfigLower\libs\SneezeSDK\install\include\sneeze_abi.h"
   if (-not (Test-Path $abi)) {
      Write-Host "  sneeze_abi.h missing: $abi"
      return $false
   }
   $ok = [bool] (Select-String -Path $abi -Pattern 'kSNEEZE_ABI_TYPE_SERVICES' -SimpleMatch -Quiet)
   if (-not $ok) {
      Write-Host "  sneeze_abi.h lacks kSNEEZE_ABI_TYPE_SERVICES (stale install at $abi)"
   }
   return $ok
}

if (-not (Test-SneezeSdkAbiInstalled)) {
   Write-Host ''
   Write-Host '============================================================'
   Write-Host '  SneezeSDK ABI canary failed — force Sync + Rebuild'
   Write-Host '============================================================'
   if (-not $env:DEP_GIT_TOKEN) {
      Write-Warning 'DEP_GIT_TOKEN is not set. If Sync cannot reach MetaversalCorp/SneezeSDK, set a PAT with contents:read on that repo (and RMAP/Map/Vox) as a Jenkins secret/env var.'
   }
   & $BuildScript -Only sneeze-sdk -Sync -Rebuild
   if ($LASTEXITCODE -ne 0) {
      Write-Error 'sneeze-sdk -Sync -Rebuild failed'
      exit $LASTEXITCODE
   }
   if (-not (Test-SneezeSdkAbiInstalled)) {
      Write-Error @"
Installed sneeze_abi.h is still missing kSNEEZE_ABI_TYPE_SERVICES after Sync.
Checkout/install is stale. On the agent, with git auth to MetaversalCorp:

  cd deps\repos\SneezeSDK
  git fetch origin main
  git reset --hard origin/main
  cd ..\..\..
  .\scripts\build-windows.ps1 -Only sneeze-sdk -Rebuild

Or set DEP_GIT_TOKEN and re-run this job.
"@
      exit 1
   }
   Write-Host '  SneezeSDK ABI canary OK after rebuild'
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

Write-Host ''
Write-Host '============================================================'
Write-Host "  Build Sneeze ($Config)"
Write-Host '============================================================'
& $BuildScript @buildArgs
exit $LASTEXITCODE
