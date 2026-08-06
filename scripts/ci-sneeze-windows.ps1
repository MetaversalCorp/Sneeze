# Copyright 2026 Metaversal Corporation. All rights reserved.
#
# Jenkins / CI entry point for standalone Sneeze Windows builds.
#
# Usage (Jenkins Execute Windows batch command):
#   pwsh -ExecutionPolicy Bypass -File scripts\ci-sneeze-windows.ps1 -Config Release
#
# What it does:
#   1. Fast-forward the workspace to origin/main (reset --hard).
#   2. Rewrite MetaversalCorp HTTPS git URLs to SSH so -Verify/-Sync use the
#      agent's deploy key (manifest pins are https://github.com/MetaversalCorp/...).
#   3. -Verify deps against deps/dependencies.json (network freshness).
#   4. If anything is out of date / stale / unreachable, -Sync (moves checkouts
#      + rebuilds affected deps in Debug and Release).
#   5. ABI canary: if installed sneeze_abi.h is missing current symbols, force
#      -Only sneeze-sdk -Sync -Rebuild (catches "Verify OK but install stale").
#   6. Build Sneeze (default: -Fresh -Rebuild).
#
# Prerequisites: VS 2022, CMake 3.24+, Git, Rust (for wasmtime), Python 3
# (depgraph / verify scripts). Agent SSH key must read MetaversalCorp private
# deps (SneezeSDK, RMAP, Map, Vox). No DEP_GIT_TOKEN required.

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
# Private MetaversalCorp deps: manifest URLs are HTTPS; Jenkins auth is SSH.
# Rewrite so build-windows -Verify/-Sync ls-remote/fetch use the deploy key.
# Scoped to MetaversalCorp/ only — public HTTPS remotes stay HTTPS.
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '============================================================'
Write-Host '  Git auth: MetaversalCorp HTTPS -> SSH'
Write-Host '============================================================'
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
   # Drop leftover PAT insteadOf entries from earlier experiments.
   $patInstead = @(git config --global --get-regexp '^url\.https://x-access-token:.*\.insteadof$' 2>$null)
   foreach ($line in $patInstead) {
      if ($line -match '^(url\..+\.insteadof)\s') {
         git config --global --unset-all $Matches[1] 2>$null | Out-Null
      }
   }

   # scp-style and ssh:// — both show up depending on git/ssh version.
   foreach ($key in @(
         'url.git@github.com:MetaversalCorp/.insteadOf',
         'url.ssh://git@github.com/MetaversalCorp/.insteadOf'
      )) {
      git config --global --unset-all $key 2>$null | Out-Null
   }
   git config --global 'url.git@github.com:MetaversalCorp/.insteadOf' 'https://github.com/MetaversalCorp/'
   git config --global 'url.ssh://git@github.com/MetaversalCorp/.insteadOf' 'https://github.com/MetaversalCorp/'
   Write-Host '  insteadOf: https://github.com/MetaversalCorp/ -> git@github.com:MetaversalCorp/ (and ssh://)'

   # Point existing MetaversalCorp clone remotes at SSH so `git fetch origin` works
   # even when a recipe uses the local origin rather than the manifest URL.
   $mvRepos = @{
      'SneezeSDK' = 'SneezeSDK'
      'RMAP'      = 'RMAP'
      'Map'       = 'Map'
      'Vox'       = 'Vox'
      'filament'  = 'filament'
      'Halogen'   = 'Halogen'
   }
   $reposRoot = Join-Path $SneezeDir 'deps\repos'
   foreach ($folder in $mvRepos.Keys) {
      $repo = Join-Path $reposRoot $folder
      if (-not (Test-Path (Join-Path $repo '.git'))) { continue }
      $sshUrl = "git@github.com:MetaversalCorp/$($mvRepos[$folder]).git"
      git -C $repo remote set-url origin $sshUrl 2>$null | Out-Null
      Write-Host "  origin -> $sshUrl ($folder)"
   }

   function Invoke-GitLsRemote ([string] $Url) {
      $out = & git ls-remote --heads $Url 'refs/heads/main' 2>&1
      $code = $LASTEXITCODE
      $text = ($out | Out-String).Trim()
      $tip = $null
      if ($code -eq 0) {
         foreach ($line in @($out)) {
            if ("$line" -match '^([0-9a-f]{7,40})\s+') {
               $tip = $Matches[1]
               break
            }
         }
      }
      return @{ Code = $code; Tip = $tip; Text = $text }
   }

   # 1) Direct SSH — proves the agent key can read SneezeSDK.
   Write-Host '  Smoke: git ls-remote git@github.com:MetaversalCorp/SneezeSDK.git ...'
   $direct = Invoke-GitLsRemote 'git@github.com:MetaversalCorp/SneezeSDK.git'
   if (-not $direct.Tip) {
      Write-Host $direct.Text
      Write-Error @"
Direct SSH ls-remote to MetaversalCorp/SneezeSDK failed (exit $($direct.Code)).
The Jenkins agent SSH key is not authenticating to that repo (missing deploy key /
wrong user / ssh-agent not loaded in the service session).
Fix access, then re-run. Do not set DEP_GIT_TOKEN — this job uses SSH only.
"@
      exit 1
   }
   Write-Host "  Direct SSH OK: $($direct.Tip.Substring(0,10))"

   # 2) HTTPS URL (what Verify/Sync pass) must hit the same tip via insteadOf.
   Write-Host '  Smoke: git ls-remote https://github.com/MetaversalCorp/SneezeSDK.git (via insteadOf) ...'
   $viaHttp = Invoke-GitLsRemote 'https://github.com/MetaversalCorp/SneezeSDK.git'
   if (-not $viaHttp.Tip) {
      Write-Host $viaHttp.Text
      Write-Error @"
HTTPS ls-remote failed after SSH insteadOf (exit $($viaHttp.Code)).
Direct SSH worked, so the rewrite is wrong — check git config --global --get-regexp insteadOf.
"@
      exit 1
   }
   Write-Host "  insteadOf HTTPS OK: $($viaHttp.Tip.Substring(0,10))"
}
finally {
   $ErrorActionPreference = $prev
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
# Catches "checkout OK / stamp OK but install/include is stale".
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
   & $BuildScript -Only sneeze-sdk -Sync -Rebuild
   if ($LASTEXITCODE -ne 0) {
      Write-Error 'sneeze-sdk -Sync -Rebuild failed'
      exit $LASTEXITCODE
   }
   if (-not (Test-SneezeSdkAbiInstalled)) {
      Write-Error @"
Installed sneeze_abi.h is still missing kSNEEZE_ABI_TYPE_SERVICES after Sync.
On the agent (SSH deploy key must reach MetaversalCorp/SneezeSDK):

  cd deps\repos\SneezeSDK
  git fetch origin main
  git reset --hard origin/main
  cd ..\..\..
  .\scripts\build-windows.ps1 -Only sneeze-sdk -Rebuild
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
