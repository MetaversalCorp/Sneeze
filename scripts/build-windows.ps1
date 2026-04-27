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
#   -Deps         Build the 15 third-party libs into deps/builds/windows-x64/<config>/libs/.
#   -Fresh        Reconfigure the Sneeze tree from scratch (cmake -S src --fresh),
#                 then build it. Wipes CMakeCache.txt + CMakeFiles/ so stale
#                 cached values (anari_DIR, compiler paths, toolchain tweaks,
#                 etc.) can't linger. Deps tree is never touched. Requires
#                 CMake >= 3.24 (VS 2022 ships 3.28+, so effectively everyone).
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
#   .\scripts\build-windows.ps1 -Fresh                 # Reconfigure + build Sneeze
#   .\scripts\build-windows.ps1 -Deps                  # Deps only (cached ones skipped)
#   .\scripts\build-windows.ps1 -All                   # Deps, then Sneeze
#   .\scripts\build-windows.ps1 -Only filament         # Build one dep (cached = skip)
#   .\scripts\build-windows.ps1 -Only filament -Rebuild  # Full-scrub rebuild of one dep
#   .\scripts\build-windows.ps1 -Rebuild               # Full-scrub rebuild of Sneeze only
#   .\scripts\build-windows.ps1 -Deps -Rebuild         # Full-scrub rebuild of all deps
#   .\scripts\build-windows.ps1 -All -Rebuild          # Full-scrub rebuild of deps + Sneeze
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

# Only these flags => deps mode. -Rebuild is a modifier, not a mode: it
# composes with whatever target set is selected by the real mode flags.
# HARD RULE: if none of -Deps, -Only, or -All is set, the deps folder must
# never be touched -- regardless of what -Rebuild / -Fresh are doing.
# (-List is read-only and handled via its own early exit below.)
$DepsMode   = [bool]($Deps -or $All -or $Only -or $List)
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
# Dependency graph -- order matters (deps before dependents).
# Keep in sync with scripts/build-deps.sh and deps/CMakeLists.txt.
# ---------------------------------------------------------------------------

$DepsOrdered = @(
   'spirv-headers'   # no deps
   'spirv-tools'     # -> spirv-headers
   'glslang'         # -> spirv-tools
   'anari-sdk'       # no deps (Debug-normal, consumed by Sneeze)
   'openxr-sdk'      # no deps (skipped if XR=OFF)
   'boringssl'       # no deps (src/jws/ crypto)
   'curl'            # -> boringssl (Android only; Schannel on Windows)
   'rmlui'           # no deps
   'nlohmann-json'   # no deps
   'jwt-cpp'         # header-only
   'spirv-cross'     # no deps (SPIR-V -> HLSL for Vox)
   'vox'             # -> spirv-cross
   'wasmtime'        # no deps (Cargo, slow)
   'filament'        # consumed only by halogen
)

$DepsOrdered += 'halogen'   # -> filament, anari-sdk

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
#   3. Per-dep build + install trees under libs/<dep>/.
function Remove-DepState ([string] $Dep) {
   Clear-Stamped $Dep
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $DepsBuildDir "$Dep-prefix")
   Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $LibsDir $Dep)
}

function Show-DepList {
   foreach ($dep in $DepsOrdered) {
      $status = if (Test-Stamped $dep) { 'cached' } else { 'pending' }
      "{0,-20} {1}" -f $dep, $status
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
# Deps mode -- configure + build deps/CMakeLists.txt
# ---------------------------------------------------------------------------

if ($DepsMode) {
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

   Write-Host ''
   Write-Host "==> Building Sneeze ($Platform, $Config)"
   & cmake --build $SneezeBuildDir --config $Config
   if ($LASTEXITCODE -ne 0) {
      Write-Error 'Sneeze build failed'
      exit 1
   }
   Write-Host "==> Sneeze Windows build complete ($Config)"
   Write-Host "    Sneeze.lib -> $SneezeInstallDir\lib"
   Write-Host "    test .exes -> $SneezeInstallDir\bin"
}
