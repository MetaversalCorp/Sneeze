# Windows x64 build.
#
# Default: compile + link Sneeze only. Plain `cmake --build` against the
# Sneeze build tree. No dep checks, no configure step. Fails naturally if
# the tree or the dep libraries aren't there yet.
#
# The Sneeze src tree is a SINGLE multi-config tree at
#   builds/windows-x64/build/
# that emits Debug or Release into
#   builds/windows-x64/install/{debug,release}/{bin,lib}/
# depending on the -Config flag (which drives `cmake --build --config`).
# Opening builds/windows-x64/build/Sneeze.sln in Visual Studio and flipping
# the Debug/Release dropdown Just Works -- both configs build against their
# respective deps trees without any reconfigure.
#
# The DEPS trees stay per-config (deps/builds/windows-x64/{debug,release}/)
# and both must be built on disk before you can flip the VS dropdown to a
# config whose deps don't exist yet.
#
# Flags switch the script into deps mode, reconfigure mode, or deps+Sneeze mode:
#
#   -Deps         Build the third-party libs into deps/builds/windows-x64/<config>/libs/.
#   -Fresh        Reconfigure the Sneeze tree from scratch (cmake -S src --fresh).
#                 Wipes CMakeCache.txt + CMakeFiles/ so stale cached values
#                 (anari_DIR, compiler paths, toolchain tweaks, etc.) can't
#                 linger. Does NOT build -- just regenerates the project files.
#                 Deps tree is never touched. Requires CMake >= 3.24 (VS 2022
#                 ships 3.28+, so effectively everyone).
#   -All          Build deps, then configure + build Sneeze.
#   -Only <dep>   Build a single dep (implies deps-targeting).
#   -List         Show dep stamp cache.
#   -Rebuild      Modifier: force a full rebuild of whatever target(s) are
#                 selected by the other flags, regardless of prior build state.
#                 NEVER crosses the src <-> deps wall on its own. Matrix:
#                   -Rebuild                  scrub + rebuild Sneeze only
#                   -Rebuild -Deps            scrub + rebuild all deps
#                   -Rebuild -Only <dep>      scrub + rebuild one dep
#                   -Rebuild -All             scrub + rebuild deps, then Sneeze
#                 Source clones in deps/repos/ are never scrubbed.
#   -Verify       Read-only. Report every dependency's status against the
#                 manifest (deps/dependencies.json) in FRESHNESS mode -- for a
#                 branch ref this reaches the network (git ls-remote) to tell you
#                 if you are behind upstream. Statuses: OK / BEHIND / MISMATCH /
#                 STALE. STALE means the checkout is fine but the built lib is
#                 out of date, for either of two reasons, read straight from git
#                 state + the graph + build stamps (nothing extra recorded on
#                 disk): a dependency is out of date, OR the dep is "not built"
#                 in a config (no build stamp -- e.g. a -Sync that failed partway,
#                 or a config you never built). Modifies nothing. This is the
#                 "make sure our dependencies are up to date" step you run right
#                 after pulling latest Sneeze. Exits non-zero if anything is out
#                 of date or stale.
#   -Sync         Modifier (implies deps-targeting): bring the dep(s) in scope
#                 into line with the manifest, then REBUILD in BOTH Debug and
#                 Release every in-scope dep that moved, is missing a build stamp,
#                 or transitively depends on any dep being rebuilt (rebuilding a
#                 dep invalidates its dependents' cached libs). A tag/SHA ref is fetched and
#                 checked out; a branch ref is fetched and fast-forwarded (never a
#                 hard reset -- local commits worked ahead of the branch are
#                 preserved; a diverged branch is left as-is with a warning). This
#                 is the ONLY path that moves a checkout. With -Only <dep>, scope
#                 is that dep + its transitive dependents (not its dependencies).
#                 The rebuild set is scrubbed in both configs up front, so a build
#                 that fails partway leaves honest "not built" state -- just fix
#                 the error and re-run -Sync to RESUME (it rebuilds only what is
#                 still unbuilt). Without -Sync, a checkout that does not match the
#                 manifest is a hard error (read-only gate; nothing is moved).
#
# HARD RULE: the deps folder (deps/builds/<platform>/<config>/) may only be
# modified when -Deps, -Only, or -All is present on the command line. A
# Sneeze-only invocation (anything else, including -Fresh or -Rebuild alone)
# cannot touch a single bit inside deps/. This parallels the CMakeLists-level
# invariant: deps/CMakeLists.txt and src/CMakeLists.txt never include or
# reference each other's trees. The scripts are the only glue, and they obey
# the same wall.
#
# The deps tree (deps/CMakeLists.txt) and the Sneeze tree (src/CMakeLists.txt)
# are two completely independent CMake projects. They share nothing. This
# script is the only glue: in -All mode it builds deps, then configures +
# builds Sneeze in a separate CMake invocation.
#
# Debug and Release live in fully separate DEPS trees under
# deps/builds/windows-x64/{debug,release}/ but share a single Sneeze build
# tree at builds/windows-x64/build/ and distinct install trees at
# builds/windows-x64/install/{debug,release}/. Source clones in deps/repos/
# are shared across configs. The script auto-passes `cmake --fresh` when
# the cached SNEEZE_CONFIG in CMakeCache.txt differs from the requested
# -Config, to evict stale find_library entries that would otherwise pull in
# the previous config's lib variants (LNK2038 mismatches).
#
# Usage:
#   .\scripts\build-windows.ps1                        # Sneeze (Release)
#   .\scripts\build-windows.ps1 -Config Debug          # Sneeze (Debug)
#   .\scripts\build-windows.ps1 -Fresh                 # Reconfigure Sneeze (no build)
#   .\scripts\build-windows.ps1 -Deps                  # Deps only (cached ones skipped)
#   .\scripts\build-windows.ps1 -All                   # Deps, then Sneeze
#   .\scripts\build-windows.ps1 -Only filament         # Build one dep (cached = skip)
#   .\scripts\build-windows.ps1 -Only filament -Rebuild  # Full-scrub rebuild of one dep
#   .\scripts\build-windows.ps1 -Rebuild               # Full-scrub rebuild of Sneeze only
#   .\scripts\build-windows.ps1 -Deps -Rebuild         # Full-scrub rebuild of all deps
#   .\scripts\build-windows.ps1 -All -Rebuild          # Full-scrub rebuild of deps + Sneeze
#   .\scripts\build-windows.ps1 -Sync                  # Sync all + rebuild moved deps & dependents (Debug + Release)
#   .\scripts\build-windows.ps1 -Only rmap -Sync       # Sync rmap + its dependents, both configs
#   .\scripts\build-windows.ps1 -List                  # Stamp cache state

[CmdletBinding()]
param (
   [ValidateSet ('Debug', 'Release')]
   [string]   $Config = 'Release',
   [string]   $Platform = 'windows-x64',
   [string]   $Only,
   [switch]   $Rebuild,
   [switch]   $List,
   [switch]   $Deps,
   [switch]   $All,
   [switch]   $Fresh,
   [switch]   $Sync,
   [switch]   $Verify,
   [Parameter (ValueFromRemainingArguments = $true)]
   [string[]] $CMakeExtraArgs
)

$ErrorActionPreference = 'Stop'

$modeCount = @($Deps, $All, $Fresh) | Where-Object { $_ } | Measure-Object | Select-Object -ExpandProperty Count
if ($modeCount -gt 1) {
   Write-Error '-Deps, -All, and -Fresh are mutually exclusive'
   exit 1
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SneezeDir = Resolve-Path (Join-Path $ScriptDir '..')

$ConfigLower    = $Config.ToLower()
$DepsSourceDir  = Join-Path $SneezeDir 'deps'
$SrcSourceDir   = Join-Path $SneezeDir 'src'
$DepRepo        = Join-Path $DepsSourceDir 'repos'
$DepRoot        = Join-Path $DepsSourceDir "builds/$Platform/$ConfigLower"
$DepsBuildDir   = Join-Path $DepRoot 'build'
$LibsDir        = Join-Path $DepRoot 'libs'
# Single multi-config Sneeze tree. -Config only drives `cmake --build --config`.
$SneezeOutDir     = Join-Path $SneezeDir "builds/$Platform"
$SneezeBuildDir   = Join-Path $SneezeOutDir 'build'
$SneezeInstallDir = Join-Path $SneezeOutDir "install/$ConfigLower"

$StampDir = Join-Path $DepsBuildDir '.dep-stamps'

# Manifest tooling: dependency order + pins come from deps/dependencies.json via
# depgraph.py; the read-only checkout gate is deps/verify.cmake.
$DepGraphTool    = Join-Path $SneezeDir 'tools/DepGraph/depgraph.py'
$DepsVerifyCMake = Join-Path $DepsSourceDir 'verify.cmake'

# Only these flags => deps mode. -Rebuild is a modifier, not a mode: it
# composes with whatever target set is selected by the real mode flags.
# HARD RULE: if none of -Deps, -Only, or -All is set, the deps folder must
# never be touched -- regardless of what -Rebuild / -Fresh are doing.
# (-List is read-only and handled via its own early exit below.)
$DepsMode   = [bool]($Deps -or $All -or $Only -or $List -or $Sync -or $Verify)
$SneezeMode = [bool]((-not $DepsMode) -or $All)
# Reconfigure the Sneeze tree before building. Implied by -All and -Fresh.
# -Rebuild does NOT force reconfigure any more: it cleans via `cmake --build
# --target clean` which preserves the configured tree (CMakeCache, CMakeFiles,
# .sln/.vcxproj), so the IDE doesn't lose state. Exception: if -Rebuild targets
# Sneeze but the tree has never been configured, fall back to configuring it --
# otherwise the subsequent build would fail with a cryptic "CMakeCache.txt
# missing" error.
$Reconfigure = [bool]($All -or $Fresh)
if ($Rebuild -and $SneezeMode -and -not (Test-Path (Join-Path $SneezeBuildDir 'CMakeCache.txt'))) {
   $Reconfigure = $true
}

# ---------------------------------------------------------------------------
# Dependency build order comes from the manifest (deps/dependencies.json), topo-
# sorted (deps before dependents) by tools/DepGraph/depgraph.py -- the single
# source of truth. No hand-kept list to drift against CMake or build-deps.sh.
# ---------------------------------------------------------------------------

$DepsOrdered = @(& python $DepGraphTool order 2>$null | Where-Object { $_ -ne '' })
if ($LASTEXITCODE -ne 0 -or $DepsOrdered.Count -eq 0) {
   Write-Error "Could not read dependency order from $DepGraphTool (is python 3 on PATH?)"
   exit 1
}

# Private MetaversalCorp deps (sneeze-sdk, rmap, map, vox, …): -Verify/-Sync use
# git ls-remote / fetch against the manifest HTTPS URL. Without credentials that
# silently no-op'd and left Jenkins on a stale sneeze-sdk (missing ABI symbols).
# Set env DEP_GIT_TOKEN (PAT with contents:read) on the agent, or rely on an
# existing git credential helper that can reach github.com.
function Enable-PrivateDepGitAuth {
   $token = $env:DEP_GIT_TOKEN
   if (-not $token) { return }
   $prev = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try {
      # Only MetaversalCorp — a global github.com insteadOf with a fine-grained
      # PAT that cannot read Khronos/public repos makes anari-sdk etc. UNKNOWN.
      $existing = @(git config --global --get-regexp '^url\.https://x-access-token:.*@github\.com/\.insteadof$' 2>$null)
      foreach ($line in $existing) {
         if ($line -match '^(url\..+\.insteadof)\s') {
            git config --global --unset-all $Matches[1] 2>$null | Out-Null
         }
      }
      $existingMv = @(git config --global --get-regexp '^url\.https://x-access-token:.*@github\.com/MetaversalCorp/\.insteadof$' 2>$null)
      foreach ($line in $existingMv) {
         if ($line -match '^(url\..+\.insteadof)\s') {
            git config --global --unset-all $Matches[1] 2>$null | Out-Null
         }
      }
      git config --global "url.https://x-access-token:${token}@github.com/MetaversalCorp/.insteadOf" "https://github.com/MetaversalCorp/"
      Write-Host "  DEP_GIT_TOKEN set — MetaversalCorp github.com clones use token auth (len=$($token.Length))"
   }
   finally {
      $ErrorActionPreference = $prev
   }
}

if ($Verify -or $Sync) {
   Enable-PrivateDepGitAuth
}

# ---------------------------------------------------------------------------
# Stamp helpers
# ---------------------------------------------------------------------------

function Test-Stamped ([string] $Dep) {
   Test-Path (Join-Path $StampDir "$Dep.done")
}

function Set-Stamped ([string] $Dep) {
   New-Item -ItemType Directory -Force -Path $StampDir | Out-Null
   New-Item -ItemType File      -Force -Path (Join-Path $StampDir "$Dep.done") | Out-Null
}

function Clear-Stamped ([string] $Dep) {
   Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $StampDir "$Dep.done")
}

# ExternalProject_Add keeps its own per-step stamps at
#   <DepsBuildDir>/<dep>-prefix/src/<dep>-stamp/<Config>/<dep>-configure
# and only re-runs configure if that file is missing. When a dep's configure
# succeeds but its build fails (e.g. link error), the configure stamp stays --
# so a later retry reuses cached CMAKE_ARGS even if deps/<dep>.cmake changed.
# Invalidate the configure stamp so the retry picks up our current args.
function Invalidate-DepConfigure ([string] $Dep) {
   $stamp = Join-Path $DepsBuildDir "$Dep-prefix/src/$Dep-stamp/$Config/$Dep-configure"
   Remove-Item -Force -ErrorAction SilentlyContinue $stamp
}

# -Rebuild: full scrub of a single dep's build state. Source clone in
# deps/repos/<dep>/ is preserved.
# Wipes:
#   1. Script-level .done stamp.
#   2. ExternalProject prefix dir: holds every EP stamp (download/update/
#      patch/configure/build/install), logs, tmp/. Nuking forces the full
#      EP chain to re-run top-to-bottom on next build.
#   3. Per-dep build + install trees under libs/<folder>/ (manifest folder, not key).
function Remove-DepState ([string] $Dep) {
   Clear-Stamped $Dep
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $DepsBuildDir "$Dep-prefix")
   # Install tree lives under the manifest FOLDER (e.g. sneeze-sdk -> SneezeSDK),
   # not the dep key -- scrub by folder so a full rebuild is truly from scratch.
   $pin = Get-DepPin $Dep
   $folder = if ($pin) { $pin.Folder } else { $Dep }
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $LibsDir $folder)
}

# Per-config path bundle. -Sync builds BOTH configs in one run, so it can't use
# the single-config module-level $DepsBuildDir/$LibsDir/$StampDir -- it resolves
# each config's tree explicitly through this.
function Get-DepConfigPaths ([string] $Cfg) {
   $cl   = $Cfg.ToLower()
   $root = Join-Path $DepsSourceDir "builds/$Platform/$cl"
   $bld  = Join-Path $root 'build'
   return [pscustomobject] @{
      Config = $Cfg
      Build  = $bld
      Libs   = Join-Path $root 'libs'
      Stamp  = Join-Path $bld '.dep-stamps'
   }
}

# Full-scrub one dep's build state in a specific config's tree (config-aware
# sibling of Remove-DepState). Source clone in deps/repos/ is never touched.
function Remove-DepStateAt ([string] $Dep, $Paths) {
   Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Paths.Stamp "$Dep.done")
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Paths.Build "$Dep-prefix")
   # The install tree lives under the manifest FOLDER, not the dep key (e.g.
   # sneeze-sdk -> SneezeSDK). Scrub by folder so a full rebuild is truly from
   # scratch -- overwriting on reinstall would leave files a new version removed.
   $pin = Get-DepPin $Dep
   $folder = if ($pin) { $pin.Folder } else { $Dep }
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Paths.Libs $folder)
}

# Transitive dependents of $Dep (the deps that must rebuild when it moves), topo-
# ordered, straight from the manifest via depgraph.py.
function Get-DepDependents ([string] $Dep) {
   return @(& python $DepGraphTool dependents $Dep 2>$null | Where-Object { $_ -ne '' })
}

function Show-DepList {
   foreach ($dep in $DepsOrdered) {
      $status = if (Test-Stamped $dep) { 'cached' } else { 'pending' }
      "{0,-20} {1}" -f $dep, $status
   }
}

# Pin (folder / ref / url) for a dep, straight from the manifest via
# depgraph.py. Returns $null if the dep is unknown.
function Get-DepPin ([string] $Dep) {
   $line = (& python $DepGraphTool pin $Dep 2>$null)
   if ($LASTEXITCODE -ne 0 -or -not $line) { return $null }
   $parts = ($line -split "`t")
   if ($parts.Count -lt 3) { return $null }
   return [pscustomobject] @{
      Folder = $parts[0]
      Ref    = $parts[1]
      Url    = $parts[2]
      Repo   = Join-Path $DepRepo $parts[0]
   }
}

# Run the shared read-only checkout gate (deps/verify.cmake). $Mode is 'offline'
# or 'freshness'; an optional $Target restricts to that dep + its transitive
# closure. $StampDirs (optional) is the set of per-config .dep-stamps dirs -- when
# given, a checkout-OK dep missing a build stamp is reported STALE "not built"
# (the -Sync-failed-partway signal). Returns cmake's exit code. NEVER modifies a
# clone. NOTE: cmake -P requires every -D to precede -P.
function Invoke-DepVerify ([string] $Mode, [string] $Target, [string[]] $StampDirs) {
   $cmakeArgs = @("-DMODE=$Mode", "-DSNEEZE_DEP_REPO=$DepRepo")
   if ($Target) { $cmakeArgs += "-DTARGET=$Target" }
   if ($StampDirs) { $cmakeArgs += "-DSNEEZE_STAMP_DIRS=$($StampDirs -join ';')" }
   $cmakeArgs += @('-P', $DepsVerifyCMake)
   # cmake -P emits its report (and, when out of date, a FATAL_ERROR) on stderr.
   # Locally relax ErrorActionPreference so the non-zero exit + stderr render as
   # plain report lines instead of a NativeCommandError that swallows the summary.
   $prev = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try {
      & cmake @cmakeArgs 2>&1 | ForEach-Object { Write-Host $_ }
   }
   finally {
      $ErrorActionPreference = $prev
   }
   return $LASTEXITCODE
}

# -Sync: bring one dep's clone into line with the manifest -- the ONLY code that
# moves a checkout. A tag/SHA ref is fetched and checked out detached. A branch
# ref is fetched and fast-forwarded ONLY (never a hard reset): commits worked
# ahead of the branch are preserved, and a diverged branch is left untouched
# with a warning. This function ONLY moves the (config-independent) checkout and
# records which deps it moved into $script:SyncMoved -- the per-config rebuild is
# driven separately (both Debug and Release) from the recorded set + dependents.
$script:SyncMoved = @()
function Sync-Dep ([string] $Dep) {
   $pin = Get-DepPin $Dep
   if (-not $pin) { return }
   if (-not (Test-Path (Join-Path $pin.Repo '.git'))) { return }   # not cloned yet; first clone honors the pin

   # git writes progress/warnings to stderr; under the script's Stop preference a
   # native command's stderr becomes a TERMINATING error. Relax locally so these
   # git calls are driven by $LASTEXITCODE, not by whether they printed to stderr.
   $prevEAP = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try {
      $head = (& git -C $pin.Repo rev-parse HEAD 2>$null)
      if ($LASTEXITCODE -ne 0) { return }
      $head = $head.Trim()
      $resolved = (& git -C $pin.Repo rev-parse --verify --quiet "$($pin.Ref)^{commit}" 2>$null)

      # Prefer the manifest URL for ls-remote/fetch — clone `origin` is often
      # unreachable for private MetaversalCorp deps on Jenkins (no creds on that
      # remote), which previously made branch pins look like tags and no-op'd Sync.
      $remote = $pin.Url
      $isBranch = [bool] (& git ls-remote --heads $remote "refs/heads/$($pin.Ref)" 2>$null)
      if (-not $isBranch) {
         $isBranch = [bool] (& git -C $pin.Repo ls-remote --heads origin "refs/heads/$($pin.Ref)" 2>$null)
         if ($isBranch) { $remote = 'origin' }
      }

      if ($isBranch) {
         & git -C $pin.Repo fetch $remote $pin.Ref 2>&1 | Write-Host
         if ($LASTEXITCODE -ne 0) { Write-Warning "${Dep}: could not fetch branch '$($pin.Ref)' from $remote; left as-is"; return }
         & git -C $pin.Repo merge --ff-only FETCH_HEAD 2>&1 | Write-Host
         if ($LASTEXITCODE -ne 0) {
            Write-Warning "${Dep}: branch '$($pin.Ref)' cannot fast-forward (diverged, or you are ahead with local commits); left as-is."
         } else {
            $new = (& git -C $pin.Repo rev-parse HEAD).Trim()
            if ($new -ne $head) {
               Write-Host "  [sync] $Dep branch '$($pin.Ref)' fast-forwarded -> $($new.Substring(0, [Math]::Min(10, $new.Length)))"
               $script:SyncMoved += $Dep
            }
         }
         return
      }

      # Not a branch: it is a tag or a raw SHA. If the clone already resolves to
      # the pin, nothing to do.
      if ($resolved -and $resolved.Trim() -eq $head) { return }
      $short = $head.Substring(0, [Math]::Min(10, $head.Length))
      Write-Host "  [sync] $Dep at $short -> '$($pin.Ref)' (fetch + checkout)"

      # Tag vs SHA: only a real tag can be fetched with `fetch tag <name>`; a raw
      # SHA must be fetched directly (GitHub allows fetching a reachable commit).
      $isTag = [bool] (& git -C $pin.Repo ls-remote --tags origin "refs/tags/$($pin.Ref)" 2>$null)
      if ($isTag) {
         & git -C $pin.Repo fetch --depth 1 origin tag $pin.Ref 2>&1 | Write-Host
      } else {
         & git -C $pin.Repo fetch origin $pin.Ref 2>&1 | Write-Host
      }
      if ($LASTEXITCODE -ne 0) { Write-Error "Could not fetch '$($pin.Ref)' for ${Dep}"; exit 1 }

      & git -C $pin.Repo checkout --detach $pin.Ref 2>&1 | Write-Host
      if ($LASTEXITCODE -ne 0) { Write-Error "Could not check out '$($pin.Ref)' for ${Dep}"; exit 1 }
      $script:SyncMoved += $Dep
      Write-Host "  [sync] $Dep now at $($pin.Ref)"
   }
   finally {
      $ErrorActionPreference = $prevEAP
   }
}

# -Sync executor: move the targeted checkouts, then rebuild -- in BOTH Debug and
# Release -- every dep in scope that needs it. Scope ("candidates") is the
# targets plus their transitive dependents; with -Only it is that one dep and its
# dependents (never its dependencies -- an out-of-date dependency of the target is
# reported by -Verify, not touched here). Within scope a dep is rebuilt when:
#   * -Rebuild forces it, or
#   * it (or an ancestor) moved this run, or
#   * it is MISSING a build stamp in either config.
# That last clause makes -Sync RESUMABLE: a run that failed partway left the
# unbuilt deps unstamped in both configs (they are scrubbed up front, below), so
# re-running -Sync picks up exactly where it stopped. It also means the first
# -Sync builds any config you have never built.
function Invoke-Sync {
   $targets = if ($Only) { @($Only) } else { $DepsOrdered }

   Write-Host '==> Sync: bringing checkouts into line with the manifest (deps/dependencies.json)'
   $script:SyncMoved = @()
   # Move every clone, not just -Only targets. Deps configure runs
   # sneeze_verify_all (OFFLINE) over the whole manifest; a scoped sync must not
   # leave an unrelated MISMATCH (e.g. halogen) blocking another dep's rebuild.
   foreach ($dep in $DepsOrdered) { Sync-Dep $dep }
   $moved = @($script:SyncMoved | Select-Object -Unique)

   # candidates = the blast radius of the scope: targets + their dependents.
   $candidates = @()
   foreach ($dep in $targets) { $candidates += $dep; $candidates += Get-DepDependents $dep }
   $candidates = $candidates | Select-Object -Unique

   # movedClosure = everything downstream of (and including) what moved this run.
   $movedClosure = @()
   foreach ($dep in $moved) { $movedClosure += $dep; $movedClosure += Get-DepDependents $dep }

   $rel = Get-DepConfigPaths 'Release'
   $dbg = Get-DepConfigPaths 'Debug'

   $need = @()
   foreach ($dep in $candidates) {
      $r = $false
      if ($Rebuild) { $r = $true }
      elseif ($movedClosure -contains $dep) { $r = $true }
      elseif (-not (Test-Path (Join-Path $rel.Stamp "$dep.done"))) { $r = $true }
      elseif (-not (Test-Path (Join-Path $dbg.Stamp "$dep.done"))) { $r = $true }
      if ($r) { $need += $dep }
   }

   foreach ($dep in $moved) {
      if ($dep -and ($need -notcontains $dep)) { $need += $dep }
   }

   # Cascade: rebuilding ANY dep invalidates the cached libs of everything that
   # depends on it (they linked the previous build), so pull in the transitive
   # dependents of the WHOLE need set -- not just of what moved this run. This
   # closes the failed-resume hole where a dep moved in an earlier crashed run
   # (so it is not "moved" now) but is still being rebuilt here (missing stamp),
   # while its already-stamped dependent would otherwise be skipped. Dependents of
   # an in-scope dep are themselves in scope, so this never escapes -Only.
   $expanded = @()
   foreach ($dep in $need) { $expanded += $dep; $expanded += Get-DepDependents $dep }
   $need = @($expanded | Select-Object -Unique | Where-Object { ($candidates -contains $_) -or ($moved -contains $_) })

   $rebuild = @($DepsOrdered | Where-Object { $need -contains $_ })   # topo order

   if (-not $rebuild) {
      Write-Host '  Everything already up to date and built (Debug + Release); nothing to do.'
      return
   }

   Write-Host ("  Rebuild set (Debug + Release): " + ($rebuild -join ', '))

   # Scrub the rebuild set in BOTH configs up front. If a build fails partway, the
   # not-yet-built deps stay unstamped in both configs -> -Verify shows them STALE
   # (not built) and the next -Sync resumes them (missing-stamp clause above).
   foreach ($paths in @($rel, $dbg)) {
      foreach ($dep in $rebuild) { Remove-DepStateAt $dep $paths }
   }

   foreach ($paths in @($rel, $dbg)) {
      $cfg = $paths.Config
      Write-Host ''
      Write-Host "==> Rebuilding deps ($cfg): $($rebuild -join ', ')"

      $cfgArgs = @(
         '-S', $DepsSourceDir
         '-B', $paths.Build
         "-DSNEEZE_CONFIG=$cfg"
         "-DSNEEZE_PLATFORM=$Platform"
         "-DSNEEZE_DEP_REPO=$DepRepo"
         "-DLIBS_DIR=$($paths.Libs)"
      )
      & cmake @cfgArgs
      if ($LASTEXITCODE -ne 0) { Write-Error "Deps CMake configure failed ($cfg)"; exit 1 }

      New-Item -ItemType Directory -Force -Path $paths.Stamp | Out-Null
      foreach ($dep in $rebuild) {
         Write-Host "==> Building ($cfg): $dep"
         Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $paths.Build "$dep-prefix/src/$dep-stamp/$cfg/$dep-configure")
         & cmake --build $paths.Build --target $dep --config $cfg
         if ($LASTEXITCODE -ne 0) { Write-Error "  [FAIL] $dep ($cfg). Fix, then re-run -Sync to resume (it rebuilds only what is still unbuilt)."; exit 1 }
         New-Item -ItemType File -Force -Path (Join-Path $paths.Stamp "$dep.done") | Out-Null
         Write-Host "    [ok] $dep ($cfg)"
      }
   }

   Write-Host ''
   Write-Host "==> Sync complete. Rebuilt in Debug + Release: $($rebuild -join ', ')"
}


function Clear-SneezePrecompiledHeaders {
   param ([string] $BuildRoot)

   if (-not (Test-Path $BuildRoot)) {
      return
   }

   # Only compiled PCH outputs — never cmake_pch.cxx / cmake_pch.hxx (CMake generates
   # those at configure; deleting them causes C1083 on the next build).
   $nRemoved = 0
   foreach ($name in @('cmake_pch.cxx.obj', 'cmake_pch.hxx.pch', 'cmake_pch.pch')) {
      Get-ChildItem -Path $BuildRoot -Recurse -Filter $name -ErrorAction SilentlyContinue |
         ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
            $nRemoved++
         }
   }
   if ($nRemoved -gt 0) {
      Write-Host "  Cleared $nRemoved stale CMake PCH output(s) under $BuildRoot (will recompile cmake_pch.cxx)"
   }
}

# ---------------------------------------------------------------------------
# -List is read-only, handle it first and exit.
# ---------------------------------------------------------------------------

if ($List) {
   Write-Host "Dependencies ($StampDir):"
   Show-DepList
   exit 0
}

# ---------------------------------------------------------------------------
# -Verify is read-only: report each dep's checkout vs the manifest (FRESHNESS,
# so branch refs are checked against upstream over the network). Run this right
# after pulling latest Sneeze, before a normal build. Modifies nothing.
# ---------------------------------------------------------------------------

if ($Verify) {
   Write-Host 'Verifying dependency checkouts against the manifest (freshness)...'
   # Pass both configs' stamp dirs so a checkout-OK-but-unbuilt dep (e.g. a -Sync
   # that failed partway, or a config you never built) surfaces as STALE "not built"
   # rather than misleadingly OK.
   $stampDirs = @((Get-DepConfigPaths 'Release').Stamp, (Get-DepConfigPaths 'Debug').Stamp)
   $rc = Invoke-DepVerify -Mode freshness -Target $Only -StampDirs $stampDirs
   exit $rc
}

# ---------------------------------------------------------------------------
# Deps mode -- configure + build deps/CMakeLists.txt
# ---------------------------------------------------------------------------

if ($DepsMode) {
   . (Join-Path $PSScriptRoot 'dep-sync.ps1')

   # -Sync is self-contained: it moves checkouts, then rebuilds the moved deps +
   # their dependents in BOTH configs, and returns. It does not fall through to
   # the single-config build path below.
   if ($Sync) {
      Invoke-Sync
      exit 0
   }

   if ($Rebuild) {
      if ($Only) {
         Remove-DepState $Only
         Write-Host "Scrubbed: $Only (stamp, EP prefix, build/, install/)"
      } else {
         # Nuke the entire per-config dep root: outer deps CMake build tree
         # + every dep's libs/<dep>/ + all stamps. Source clones untouched.
         Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $DepRoot
         Write-Host "Scrubbed: $DepRoot"
      }
   }

   # OFFLINE read-only gate: halt the build if any targeted clone doesn't match
   # the manifest -- catching a bumped ref whose clone was never moved, which
   # would otherwise rebuild stale source silently. -Sync (the only clone-moving
   # step) is handled above and never reaches here.
   $rc = Invoke-DepVerify -Mode offline -Target $Only
   if ($rc -ne 0) {
      Write-Error 'A dependency checkout does not match the manifest. Re-run with -Sync to correct it, or -Verify to inspect.'
      exit 1
   }

   Update-DepStamps -DepsSourceDir $DepsSourceDir -Deps @($DepsOrdered) -StampDir $StampDir

   Write-Host "==> Sneeze Windows deps build"
   Write-Host "    Platform       = $Platform"
   Write-Host "    Config         = $Config"
   Write-Host "    Dep repo (src) = $DepRepo"
   Write-Host "    Dep build dir  = $DepsBuildDir"
   Write-Host "    Libs dir       = $LibsDir"

   $depsConfigureArgs = @(
      '-S', $DepsSourceDir
      '-B', $DepsBuildDir
      "-DSNEEZE_CONFIG=$Config"
      "-DSNEEZE_PLATFORM=$Platform"
      "-DSNEEZE_DEP_REPO=$DepRepo"
      "-DLIBS_DIR=$LibsDir"
   )
   if ($CMakeExtraArgs) { $depsConfigureArgs += $CMakeExtraArgs }

   & cmake @depsConfigureArgs
   if ($LASTEXITCODE -ne 0) {
      Write-Error 'Deps CMake configure failed'
      exit 1
   }

   $depsToBuild = if ($Only) { @($Only) } else { $DepsOrdered }

   $built   = @()
   $skipped = @()
   $failed  = @()

   foreach ($dep in $depsToBuild) {
      if (Test-Stamped $dep) {
         $skipped += $dep
         continue
      }

      Write-Host ''
      Write-Host "==> Building: $dep"

      # Force ExternalProject to re-run configure so arg changes take effect.
      Invalidate-DepConfigure $dep

      & cmake --build $DepsBuildDir --target $dep --config $Config
      if ($LASTEXITCODE -eq 0) {
         Set-Stamped $dep
         $built += $dep
         Write-Host "    [ok] $dep"
      } else {
         $failed += $dep
         Write-Host "    [FAIL] $dep"
         Write-Host ''
         Write-Host "Re-run with: .\scripts\build-windows.ps1 -Deps -Config $Config -Only $dep"
      }
   }

   Write-Host ''
   Write-Host '=== Summary ==='
   if ($skipped.Count) { Write-Host "Cached:  $($skipped -join ', ')" }
   if ($built.Count)   { Write-Host "Built:   $($built   -join ', ')" }
   if ($failed.Count)  { Write-Host "FAILED:  $($failed  -join ', ')" }
   Write-Host ''

   if ($failed.Count) {
      Write-Host 'Fix failures, then re-run. Only failed deps rebuild.'
      exit 1
   }
}

# ---------------------------------------------------------------------------
# Sneeze mode -- configure (if -All) + plain `cmake --build`, no dep checks.
# ---------------------------------------------------------------------------

if ($Fresh -or $SneezeMode) {
   # -Rebuild with Sneeze in scope: clean only the CURRENT config's compiled
   # artifacts via `cmake --build --target clean --config <cfg>`. This preserves
   # the configured CMake tree (CMakeCache.txt, CMakeFiles/, generated .sln and
   # .vcxproj) so Visual Studio doesn't lose IDE state, and it preserves the
   # OTHER config's intermediates and install tree. The selected config's
   # install/<cfg>/ is also wiped so stale binaries don't survive the rebuild.
   if ($Rebuild -and $SneezeMode) {
      if (Test-Path (Join-Path $SneezeBuildDir 'CMakeCache.txt')) {
         Write-Host ''
         Write-Host "==> Cleaning Sneeze $Config build artifacts"
         & cmake --build $SneezeBuildDir --target clean --config $Config
         if ($LASTEXITCODE -ne 0) {
            Write-Error 'Sneeze clean failed'
            exit 1
         }
      }
      Write-Host "==> Scrubbing Sneeze $Config install: $SneezeInstallDir"
      Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $SneezeInstallDir
   }

   # -Fresh or -All: reconfigure the Sneeze tree from src/CMakeLists.txt
   # before building. Default (no -All, no -Fresh): skip configure; rely
   # on an already-configured tree. If the tree doesn't exist, `cmake --build`
   # will fail with a clear "CMakeCache.txt is missing" error -- the user
   # should re-run with -Fresh (or -All if deps are also missing).
   if ($Reconfigure) {
      Write-Host ''
      Write-Host "==> Configuring Sneeze tree at $SneezeBuildDir"

      $sneezeConfigureArgs = @(
         '-S', $SrcSourceDir
         '-B', $SneezeBuildDir
         "-DLIBS_DIR=$LibsDir"
         "-DSNEEZE_CONFIG=$Config"
         "-DSNEEZE_PLATFORM=$Platform"
         "-DSNEEZE_BUILD_ROOT=$SneezeOutDir"
      )

      # --fresh (CMake 3.24+): wipes CMakeCache.txt + CMakeFiles/ before
      # reconfiguring. Triggered explicitly by -Fresh, and automatically when
      # the cached SNEEZE_CONFIG in CMakeCache.txt differs from the requested
      # $Config -- find_library caches absolute paths, so reconfiguring with
      # a different LIBS_DIR does not update entries that were already resolved
      # under the previous config (causes LNK2038 _ITERATOR_DEBUG_LEVEL /
      # RuntimeLibrary mismatches when Release tries to link Debug-suffix libs
      # like spirv-cross-cored.lib that the cache still points at).
      $autoFresh = $false
      $cachePath = Join-Path $SneezeBuildDir 'CMakeCache.txt'
      if (Test-Path $cachePath) {
         $cachedLine = Select-String -Path $cachePath -Pattern '^SNEEZE_CONFIG:[^=]*=(.+)$' | Select-Object -First 1
         if ($cachedLine) {
            $cachedConfig = $cachedLine.Matches[0].Groups[1].Value.Trim()
            if ($cachedConfig -and ($cachedConfig -ne $Config)) {
               Write-Host "==> Cached SNEEZE_CONFIG=$cachedConfig differs from requested $Config; forcing --fresh"
               $autoFresh = $true
            }
         }
      }
      if ($Fresh -or $autoFresh) { $sneezeConfigureArgs += '--fresh' }

      & cmake @sneezeConfigureArgs
      if ($LASTEXITCODE -ne 0) {
         Write-Error 'Sneeze CMake configure failed'
         exit 1
      }
   }

   if ($Fresh -and -not $Rebuild) {
      Write-Host "==> Sneeze reconfigure complete (no build)"
   } else {
      Write-Host ''
      Write-Host "==> Building Sneeze ($Platform, $Config)"
      if ($Fresh -or $Rebuild) {
         Clear-SneezePrecompiledHeaders $SneezeBuildDir
      }
      & cmake --build $SneezeBuildDir --config $Config
      if ($LASTEXITCODE -ne 0) {
         Write-Error 'Sneeze build failed'
         exit 1
      }
      Write-Host "==> Sneeze Windows build complete ($Config)"
      Write-Host "    Sneeze.lib -> $SneezeInstallDir\lib"
      Write-Host "    test .exes -> $SneezeInstallDir\bin"
   }
}
