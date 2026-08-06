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
# (depgraph / verify scripts). No DEP_GIT_TOKEN required.
#
# Jenkins SSH note:
#   The SCM "Credentials" dropdown (e.g. LA2-JENKINSOS) is used ONLY by the
#   Git plugin to clone Sneeze. It is NOT in the environment for this script's
#   git ls-remote/fetch of deps. Bind the SAME credential into the build step:
#
#   Build Environment → Use secret text(s) or file(s) → SSH User Private Key
#     Credentials: LA2-JENKINSOS (or whatever SCM uses)
#     Key File Variable: SNEEZE_CI_SSH_KEY
#
#   Or: Build Environment → SSH Agent → Credentials: same key.
#
#   That key must be authorized on every private MetaversalCorp dep
#   (SneezeSDK, RMAP, Map, Vox) — a deploy key attached only to Sneeze.git
#   will still get Permission denied for the others.

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

# SCM credentials never reach this process. Prefer an explicitly bound key
# (Credentials Binding → SNEEZE_CI_SSH_KEY) so git/ssh use the same identity
# as the Sneeze checkout. SSH Agent plugin also works if it set SSH_AUTH_SOCK.
if ($env:SNEEZE_CI_SSH_KEY) {
   $keyPath = $env:SNEEZE_CI_SSH_KEY.Trim()
   if (-not (Test-Path -LiteralPath $keyPath)) {
      Write-Error "SNEEZE_CI_SSH_KEY is set but file not found: $keyPath"
      exit 1
   }
   # IdentitiesOnly: do not also try agent/default keys (avoids confusing failures).
   $env:GIT_SSH_COMMAND = "ssh -i `"$keyPath`" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
   Write-Host "  GIT_SSH_COMMAND: ssh -i <SNEEZE_CI_SSH_KEY> -o IdentitiesOnly=yes"
}
elseif ($env:SSH_AUTH_SOCK -or $env:GIT_SSH_COMMAND) {
   Write-Host '  Using existing SSH agent / GIT_SSH_COMMAND from the job environment'
}
else {
   Write-Host '  WARNING: no SNEEZE_CI_SSH_KEY / SSH_AUTH_SOCK — git will use the OS default key (often none under Jenkins).'
   Write-Host '  Bind LA2-JENKINSOS (or your SCM credential) as SNEEZE_CI_SSH_KEY — see script header.'
}

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
The SCM Credentials dropdown does not apply here — bind the same key into the
build step as SNEEZE_CI_SSH_KEY (Credentials Binding) or enable SSH Agent.
Also ensure that key is authorized on SneezeSDK (and RMAP/Map/Vox), not only Sneeze.
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
