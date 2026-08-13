#!/usr/bin/env bash
# Build Sneeze dependencies one at a time with caching.
#
# Each dep: configure once -> cmake --build --target <dep> -> stamp on success.
# Stamps live in $BUILD_DIR/.dep-stamps/<dep>.done
# Re-run = skip stamped deps. --rebuild to full-scrub rebuild (see below).
# --only <dep> to build a single dep.
# --list to show dep order and status.
# --rebuild (alone or with --only) wipes build outputs + all stamps.
#   Source clones in deps/repos/ are preserved. With --only: scrubs one dep
#   (script stamp + ExternalProject prefix + libs/<folder>/). Without --only:
#   scrubs the entire per-config dep root.
# --verify: read-only. Report every dependency's status against the manifest
#   (deps/dependencies.json) in FRESHNESS mode -- branch refs are checked over
#   the network (git ls-remote) so you learn if you are behind upstream.
#   Statuses: OK / BEHIND / MISMATCH / STALE. STALE means the checkout is fine
#   but the built lib is out of date, for either reason (read from git state +
#   the graph + build stamps): a dependency is out of date, OR the dep is "not
#   built" in a config (no build stamp -- e.g. a --sync that failed partway, or
#   a config you never built). Modifies nothing. Run right after pulling Sneeze.
# --sync: for the dep(s) in scope, bring the clone into line with the manifest,
#   then REBUILD in BOTH Debug and Release every in-scope dep that moved, whose
#   is missing a build stamp, or transitively depends on any dep being rebuilt
#   (rebuilding a dep invalidates its dependents' cached libs). This is the ONLY path that
#   moves a checkout. A tag/SHA ref is fetched and checked out; a branch ref is
#   fetched and fast-forwarded ONLY (never a hard reset, so local commits worked
#   ahead of the branch are preserved; a diverged branch is left as-is with a
#   warning). With --only <dep>, scope is that dep + its transitive dependents
#   (not its dependencies). The rebuild set is scrubbed in both configs up front,
#   so a build that fails partway leaves honest "not built" state -- fix the
#   error and re-run --sync to RESUME (rebuilds only what is still unbuilt).
#   Without --sync, a checkout that does not match the manifest is a hard error.
#
# Expected invocation is via build-linux.sh / build-macos.sh which pick the
# per-config directories. To invoke directly, pass --config, --platform,
# --build-dir, --libs-dir, and --dep-repo explicitly.
#
# Usage:
#   ./scripts/build-deps.sh [options]
#   ./scripts/build-deps.sh --only wasmtime
#   ./scripts/build-deps.sh --only filament --rebuild
#   ./scripts/build-deps.sh --rebuild
#   ./scripts/build-deps.sh --list

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SNEEZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CONFIG="${CONFIG:-Release}"
PLATFORM="${PLATFORM:-}"
BUILD_DIR="${BUILD_DIR:-}"
LIBS_DIR="${LIBS_DIR:-}"
DEP_REPO="${DEP_REPO:-$SNEEZE_DIR/deps/repos}"

REBUILD=0
ONLY=""
LIST_ONLY=0
SYNC=0
VERIFY=0
CMAKE_EXTRA_ARGS=()
SYNC_MOVED=()

# Manifest tooling: dependency order + pins come from deps/dependencies.json via
# depgraph.py; the read-only checkout gate is deps/verify.cmake.
PYTHON="${PYTHON:-python3}"
DEPGRAPH="$SNEEZE_DIR/tools/DepGraph/depgraph.py"
DEPS_VERIFY_CMAKE="$SNEEZE_DIR/deps/verify.cmake"

# ---------------------------------------------------------------------------
# Dependency build order comes from the manifest, topo-sorted (deps before
# dependents) by depgraph.py -- the single source of truth. No hand-kept list to
# drift against CMake or build-windows.ps1. (read loop, not mapfile, for the
# bash 3.2 that ships on macOS.)
# ---------------------------------------------------------------------------

DEPS_ORDERED=()
while IFS= read -r _line; do
   if [[ -n "$_line" ]]; then DEPS_ORDERED+=("$_line"); fi
done < <("$PYTHON" "$DEPGRAPH" order)
if [[ ${#DEPS_ORDERED[@]} -eq 0 ]]; then
   echo "Could not read dependency order from $DEPGRAPH (is python 3 on PATH?)" >&2
   exit 1
fi

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
   case "$1" in
      --rebuild)      REBUILD=1 ;;
      --only)         shift; ONLY="$1" ;;
      --list)         LIST_ONLY=1 ;;
      --sync)         SYNC=1 ;;
      --verify)       VERIFY=1 ;;
      --config)       shift; CONFIG="$1" ;;
      --platform)     shift; PLATFORM="$1" ;;
      --build-dir)    shift; BUILD_DIR="$1" ;;
      --libs-dir)     shift; LIBS_DIR="$1" ;;
      --dep-repo)     shift; DEP_REPO="$1" ;;
      -D*)            CMAKE_EXTRA_ARGS+=("$1") ;;
      *)              echo "Unknown: $1" >&2; exit 1 ;;
   esac
   shift
done

# Validate config
case "$CONFIG" in
   Debug|Release) : ;;
   *) echo "--config must be Debug or Release (got '$CONFIG')" >&2; exit 1 ;;
esac

# Auto-detect platform if not provided.
if [[ -z "$PLATFORM" ]]; then
   case "$(uname -s)" in
      Linux)
         case "$(uname -m)" in
            aarch64|arm64) PLATFORM="linux-arm64" ;;
            *)             PLATFORM="linux-x64" ;;
         esac ;;
      Darwin)
         case "$(uname -m)" in
            arm64)  PLATFORM="macos-arm64" ;;
            x86_64) PLATFORM="macos-x64" ;;
         esac ;;
      *) echo "Could not auto-detect platform; pass --platform explicitly." >&2; exit 1 ;;
   esac
fi

CFG_LOWER="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"

# Derive per-config dirs if not explicitly set.
if [[ -z "$BUILD_DIR" ]]; then
   BUILD_DIR="$SNEEZE_DIR/deps/builds/$PLATFORM/$CFG_LOWER/build"
fi
if [[ -z "$LIBS_DIR" ]]; then
   LIBS_DIR="$SNEEZE_DIR/deps/builds/$PLATFORM/$CFG_LOWER/libs"
fi

STAMP_DIR="$BUILD_DIR/.dep-stamps"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

is_stamped() { [[ -f "$STAMP_DIR/$1.done" ]]; }
stamp()      { mkdir -p "$STAMP_DIR"; touch "$STAMP_DIR/$1.done"; }
unstamp()    { rm -f "$STAMP_DIR/$1.done"; }

# ExternalProject_Add keeps its own per-step stamps at
#   $BUILD_DIR/<dep>-prefix/src/<dep>-stamp/<CONFIG>/<dep>-configure
# and only re-runs configure if that file is missing. When a dep's configure
# succeeds but its build fails (e.g. link error), the configure stamp stays --
# so a later retry reuses cached CMAKE_ARGS even if deps/<dep>.cmake changed.
# Invalidate the configure stamp so the retry picks up our current args.
invalidate_dep_configure() {
   rm -f "$BUILD_DIR/$1-prefix/src/$1-stamp/$CONFIG/$1-configure"
}

# --rebuild: full scrub of a single dep's build state. Source clone in
# deps/repos/<dep>/ is preserved.
# Wipes:
#   1. Script-level .done stamp.
#   2. ExternalProject prefix dir: holds every EP stamp (download/update/
#      patch/configure/build/install), logs, tmp/. Nuking forces the full
#      EP chain to re-run top-to-bottom on next build.
#   3. Per-dep build + install trees under libs/<folder>/ (manifest folder, not key).
remove_dep_state() {
   unstamp "$1"
   rm -rf "$BUILD_DIR/$1-prefix"
   rm -rf "$LIBS_DIR/$(dep_folder "$1")"
}

list_deps() {
   for dep in "${DEPS_ORDERED[@]}"; do
      local status="pending"
      is_stamped "$dep" && status="cached"
      printf "  %-20s %s\n" "$dep" "$status"
   done
}

# Echoes "<folder>\t<ref>\t<url>" for a dep, straight from the manifest via
# depgraph.py. Echoes nothing for an unknown dep.
dep_pin() {
   "$PYTHON" "$DEPGRAPH" pin "$1" 2>/dev/null || true
}

# The install tree for a dep lives under its manifest FOLDER, not its key (e.g.
# sneeze-sdk -> SneezeSDK). Echoes the folder; falls back to the key if unknown.
dep_folder() {
   local f
   f="$(dep_pin "$1" | cut -f1)"
   [[ -n "$f" ]] && echo "$f" || echo "$1"
}

# Transitive dependents of $1 (deps that must rebuild when it moves), topo order.
dep_dependents() {
   "$PYTHON" "$DEPGRAPH" dependents "$1" 2>/dev/null || true
}

# Run the shared read-only checkout gate (deps/verify.cmake). $1=mode
# (offline|freshness), $2=optional target (dep + its transitive closure),
# $3=optional ';'-separated list of per-config .dep-stamps dirs (when given, a
# checkout-OK dep missing a build stamp is reported STALE "not built").
# Returns cmake's exit status. NEVER modifies a clone. NOTE: -D must precede -P.
invoke_dep_verify() {
   local mode="$1" target="${2:-}" stampdirs="${3:-}"
   local args=(-DMODE="$mode" -DSNEEZE_DEP_REPO="$DEP_REPO")
   [[ -n "$target" ]] && args+=(-DTARGET="$target")
   [[ -n "$stampdirs" ]] && args+=(-DSNEEZE_STAMP_DIRS="$stampdirs")
   args+=(-P "$DEPS_VERIFY_CMAKE")
   cmake "${args[@]}"
}

# --sync: bring one dep's clone into line with the manifest -- the ONLY code
# that moves a checkout. A tag/SHA ref is fetched and checked out detached. A
# branch ref is fetched and fast-forwarded when possible. If the clone is ahead
# of the remote (e.g. after a force-push rewrote main), it is reset --hard to
# FETCH_HEAD so the checkout matches the manifest. True divergence (unique commits
# on both sides) is left untouched with a warning. This ONLY moves the
# (config-independent) checkout and
# appends moved deps to SYNC_MOVED -- the per-config rebuild (both Debug and
# Release) is driven separately from the recorded set + dependents.
sync_dep() {
   local dep="$1"
   local pin folder ref url repo head resolved new short

   pin="$(dep_pin "$dep")"
   [[ -n "$pin" ]] || return 0
   IFS=$'\t' read -r folder ref url <<< "$pin"
   repo="$DEP_REPO/$folder"
   [[ -d "$repo/.git" ]] || return 0          # not cloned yet; first clone honors the pin

   head="$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)"
   [[ -n "$head" ]] || return 0
   resolved="$(git -C "$repo" rev-parse --verify --quiet "${ref}^{commit}" 2>/dev/null || true)"

   # Prefer the manifest URL for ls-remote/fetch (clone origin may lack auth for
   # private MetaversalCorp deps). Fall back to origin.
   local remote="$url"
   if [[ -z "$(git ls-remote --heads "$url" "refs/heads/$ref" 2>/dev/null || true)" ]]; then
      if [[ -n "$(git -C "$repo" ls-remote --heads origin "refs/heads/$ref" 2>/dev/null || true)" ]]; then
         remote="origin"
      else
         remote=""
      fi
   fi

   if [[ -n "$remote" ]]; then
      # branch: fetch + fast-forward only (preserves any local ahead commits).
      # Do not recurse submodules - SneezeSDK header sync must not fail on a
      # missing private submodule SHA ("not our ref").
      if ! git -c fetch.recurseSubmodules=false -C "$repo" fetch "$remote" "$ref"; then
         echo "WARNING: $dep: could not fetch branch '$ref' from $remote; left as-is" >&2
         return 0
      fi
      if git -C "$repo" merge --ff-only FETCH_HEAD >/dev/null 2>&1; then
         new="$(git -C "$repo" rev-parse HEAD)"
         if [[ "$new" != "$head" ]]; then
            echo "  [sync] $dep branch '$ref' fast-forwarded -> ${new:0:10}"
            SYNC_MOVED+=("$dep")
         fi
      elif git -C "$repo" merge-base --is-ancestor FETCH_HEAD HEAD 2>/dev/null; then
         git -C "$repo" reset --hard FETCH_HEAD >/dev/null
         new="$(git -C "$repo" rev-parse HEAD)"
         echo "  [sync] $dep branch '$ref' reset to remote -> ${new:0:10} (local was ahead)"
         SYNC_MOVED+=("$dep")
      else
         echo "WARNING: $dep: branch '$ref' cannot fast-forward (diverged); left as-is." >&2
      fi
      return 0
   fi

   # Not a branch: it is a tag or a raw SHA. If the clone already resolves to the
   # pin, nothing to do.
   [[ -n "$resolved"  &&  "$resolved" == "$head" ]] && return 0
   short="${head:0:10}"
   echo "  [sync] $dep at $short -> '$ref' (fetch + checkout)"
   # Only a real tag can be fetched with `fetch tag <name>`; a raw SHA must be
   # fetched directly (GitHub allows fetching a reachable commit).
   if [[ -n "$(git -C "$repo" ls-remote --tags origin "refs/tags/$ref" 2>/dev/null || true)" ]]; then
      git -c fetch.recurseSubmodules=false -C "$repo" fetch --depth 1 origin tag "$ref"
   else
      git -c fetch.recurseSubmodules=false -C "$repo" fetch origin "$ref"
   fi
   git -C "$repo" checkout --detach "$ref"
   SYNC_MOVED+=("$dep")
   echo "  [sync] $dep now at $ref"
}

# --sync executor: move the targeted checkouts, then rebuild -- in BOTH Debug and
# Release -- every dep in scope that needs it. Scope ("candidates") is the
# targets plus their transitive dependents; with --only it is that one dep and
# its dependents (never its dependencies). Within scope a dep is rebuilt when
# --rebuild forces it, it (or an ancestor) moved this run, or it is MISSING a
# build stamp in either config. That last clause makes --sync RESUMABLE: a run
# that failed partway left the unbuilt deps unstamped in both configs (they are
# scrubbed up front, below), so re-running --sync resumes where it stopped. It
# also means the first --sync builds any config you have never built.
invoke_sync() {
   local targets=()
   if [[ -n "$ONLY" ]]; then targets=("$ONLY"); else targets=("${DEPS_ORDERED[@]}"); fi

   echo "==> Sync: bringing checkouts into line with the manifest (deps/dependencies.json)"
   SYNC_MOVED=()
   local dep d o
   # Move every clone, not just --only targets. Deps configure runs
   # sneeze_verify_all (OFFLINE) over the whole manifest; a scoped sync must not
   # leave an unrelated MISMATCH (e.g. halogen) blocking boringssl's rebuild.
   for dep in "${DEPS_ORDERED[@]}"; do sync_dep "$dep"; done

   # candidates = the blast radius of the scope: targets + their dependents.
   local candidates=()
   for dep in "${targets[@]}"; do
      candidates+=("$dep")
      while IFS= read -r d; do [[ -n "$d" ]] && candidates+=("$d"); done < <(dep_dependents "$dep")
   done

   # moved_closure = everything downstream of (and including) what moved this run.
   local moved_closure=()
   for dep in "${SYNC_MOVED[@]:-}"; do
      [[ -n "$dep" ]] || continue
      moved_closure+=("$dep")
      while IFS= read -r d; do [[ -n "$d" ]] && moved_closure+=("$d"); done < <(dep_dependents "$dep")
   done

   local rel_stamp="$SNEEZE_DIR/deps/builds/$PLATFORM/release/build/.dep-stamps"
   local dbg_stamp="$SNEEZE_DIR/deps/builds/$PLATFORM/debug/build/.dep-stamps"

   # A candidate needs rebuilding if --rebuild forces it, it/an ancestor moved,
   # or it is missing a build stamp in either config (resume signal).
   local wanted=()
   local c m
   for c in "${candidates[@]}"; do
      local need=0
      if [[ $REBUILD -eq 1 ]]; then
         need=1
      else
         for m in "${moved_closure[@]:-}"; do
            [[ "$m" == "$c" ]] && { need=1; break; }
         done
         if [[ $need -eq 0 ]]; then
            [[ -f "$rel_stamp/$c.done" ]] || need=1
            [[ -f "$dbg_stamp/$c.done" ]] || need=1
         fi
      fi
      [[ $need -eq 1 ]] && wanted+=("$c")
   done

   # Anything whose checkout moved this run must rebuild (even outside --only
   # scope), or installed libs would not match HEAD.
   for m in "${SYNC_MOVED[@]:-}"; do
      [[ -n "$m" ]] && wanted+=("$m")
   done

   # Cascade: rebuilding ANY dep invalidates the cached libs of everything that
   # depends on it, so pull in the transitive dependents of the WHOLE need set --
   # not just of what moved this run. Closes the failed-resume hole where a dep
   # moved in an earlier crashed run (not "moved" now) but is rebuilt here
   # (missing stamp), while its already-stamped dependent would be skipped.
   # Dependents of an in-scope dep are themselves in scope, so this cannot escape
   # --only.
   local expanded=()
   for c in "${wanted[@]:-}"; do
      [[ -n "$c" ]] || continue
      expanded+=("$c")
      while IFS= read -r d; do [[ -n "$d" ]] && expanded+=("$d"); done < <(dep_dependents "$c")
   done

   # Unique + topo order via DEPS_ORDERED.
   local rebuild=()
   for o in "${DEPS_ORDERED[@]}"; do
      for dep in "${expanded[@]:-}"; do
         if [[ "$o" == "$dep" ]]; then rebuild+=("$o"); break; fi
      done
   done

   if [[ ${#rebuild[@]} -eq 0 ]]; then
      echo "  Everything already up to date and built (Debug + Release); nothing to do."
      return 0
   fi
   echo "  Rebuild set (Debug + Release): ${rebuild[*]}"

   # Scrub the rebuild set in BOTH configs up front. If a build fails partway, the
   # not-yet-built deps stay unstamped in both configs -> --verify shows them STALE
   # (not built) and the next --sync resumes them (missing-stamp clause above).
   local cfg cfg_lower cfg_build cfg_libs cfg_stamp
   for cfg in Release Debug; do
      cfg_lower="$(echo "$cfg" | tr '[:upper:]' '[:lower:]')"
      cfg_build="$SNEEZE_DIR/deps/builds/$PLATFORM/$cfg_lower/build"
      cfg_libs="$SNEEZE_DIR/deps/builds/$PLATFORM/$cfg_lower/libs"
      cfg_stamp="$cfg_build/.dep-stamps"
      for dep in "${rebuild[@]}"; do
         rm -f  "$cfg_stamp/$dep.done"
         rm -rf "$cfg_build/$dep-prefix"
         rm -rf "$cfg_libs/$(dep_folder "$dep")"   # install tree is under the manifest folder, not the key
      done
   done

   for cfg in Release Debug; do
      cfg_lower="$(echo "$cfg" | tr '[:upper:]' '[:lower:]')"
      cfg_build="$SNEEZE_DIR/deps/builds/$PLATFORM/$cfg_lower/build"
      cfg_libs="$SNEEZE_DIR/deps/builds/$PLATFORM/$cfg_lower/libs"
      cfg_stamp="$cfg_build/.dep-stamps"
      echo ""
      echo "==> Rebuilding deps ($cfg): ${rebuild[*]}"

      cmake -S "$SNEEZE_DIR/deps" -B "$cfg_build" \
         -DSNEEZE_CONFIG="$cfg" \
         -DSNEEZE_PLATFORM="$PLATFORM" \
         -DSNEEZE_DEP_REPO="$DEP_REPO" \
         -DLIBS_DIR="$cfg_libs" \
         "${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}" \
         2>&1 | tail -5

      mkdir -p "$cfg_stamp"
      for dep in "${rebuild[@]}"; do
         echo "==> Building ($cfg): $dep"
         rm -f "$cfg_build/$dep-prefix/src/$dep-stamp/$cfg/$dep-configure"
         if cmake --build "$cfg_build" --target "$dep" --config "$cfg"; then
            touch "$cfg_stamp/$dep.done"
            echo "    [ok] $dep ($cfg)"
         else
            echo "    [FAIL] $dep ($cfg). Fix, then re-run --sync to resume (it rebuilds only what is still unbuilt)." >&2
            exit 1
         fi
      done
   done

   echo ""
   echo "==> Sync complete. Rebuilt in Debug + Release: ${rebuild[*]}"
}

# ---------------------------------------------------------------------------
# List mode
# ---------------------------------------------------------------------------

if [[ $LIST_ONLY -eq 1 ]]; then
   echo "Dependencies ($STAMP_DIR):"
   list_deps
   exit 0
fi

# ---------------------------------------------------------------------------
# --verify is read-only: report each dep's checkout vs the manifest (FRESHNESS,
# so branch refs are checked against upstream over the network). Run right after
# pulling latest Sneeze, before a normal build. Modifies nothing.
# ---------------------------------------------------------------------------

if [[ $VERIFY -eq 1 ]]; then
   echo "Verifying dependency checkouts against the manifest (freshness)..."
   # Pass both configs' stamp dirs so a checkout-OK-but-unbuilt dep (a --sync that
   # failed partway, or a config you never built) surfaces as STALE "not built".
   _rel_stamp="$SNEEZE_DIR/deps/builds/$PLATFORM/release/build/.dep-stamps"
   _dbg_stamp="$SNEEZE_DIR/deps/builds/$PLATFORM/debug/build/.dep-stamps"
   if invoke_dep_verify freshness "$ONLY" "$_rel_stamp;$_dbg_stamp"; then exit 0; else exit 1; fi
fi

# ---------------------------------------------------------------------------
# --sync is self-contained: move checkouts, then rebuild the moved deps + their
# dependents in BOTH configs, and exit. Does not fall through to the single-
# config build path below.
# ---------------------------------------------------------------------------

if [[ $SYNC -eq 1 ]]; then
   invoke_sync
   exit 0
fi

# ---------------------------------------------------------------------------
# Rebuild (scrub build state before configure)
# ---------------------------------------------------------------------------

if [[ $REBUILD -eq 1 ]]; then
   if [[ -n "$ONLY" ]]; then
      remove_dep_state "$ONLY"
      echo "Scrubbed: $ONLY (stamp, EP prefix, build/, install/)"
   else
      # Nuke the entire per-config dep root: outer deps CMake build tree
      # + every dep's libs/<dep>/ + all stamps. Source clones untouched.
      # DEP_ROOT is the shared parent of BUILD_DIR and LIBS_DIR.
      DEP_ROOT="$(dirname "$BUILD_DIR")"
      rm -rf "$DEP_ROOT"
      echo "Scrubbed: $DEP_ROOT"
   fi
fi

# ---------------------------------------------------------------------------
# OFFLINE read-only gate: halt the build if any targeted clone doesn't match the
# manifest -- catching a bumped ref whose clone was never moved, which would
# otherwise rebuild stale source silently. --sync (the only clone-moving step)
# is handled above and never reaches here.
# ---------------------------------------------------------------------------

if ! invoke_dep_verify offline "$ONLY"; then
   echo "A dependency checkout does not match the manifest. Re-run with --sync to correct it, or --verify to inspect." >&2
   exit 1
fi

# ---------------------------------------------------------------------------
# Configure (once -- idempotent via CMakeCache)
# ---------------------------------------------------------------------------

echo "==> Configuring deps"
echo "    PLATFORM  = $PLATFORM"
echo "    CONFIG    = $CONFIG"
echo "    BUILD_DIR = $BUILD_DIR"
echo "    LIBS_DIR  = $LIBS_DIR"
echo "    DEP_REPO  = $DEP_REPO"

cmake -S "$SNEEZE_DIR/deps" -B "$BUILD_DIR" \
   -DSNEEZE_CONFIG="$CONFIG" \
   -DSNEEZE_PLATFORM="$PLATFORM" \
   -DSNEEZE_DEP_REPO="$DEP_REPO" \
   -DLIBS_DIR="$LIBS_DIR" \
   "${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}" \
   2>&1 | tail -5

# ---------------------------------------------------------------------------
# Build deps
# ---------------------------------------------------------------------------

if [[ -n "$ONLY" ]]; then
   DEPS_TO_BUILD=("$ONLY")
else
   DEPS_TO_BUILD=("${DEPS_ORDERED[@]}")
fi

FAILED=()
SKIPPED=()
BUILT=()

for dep in "${DEPS_TO_BUILD[@]}"; do
   if is_stamped "$dep"; then
      SKIPPED+=("$dep")
      continue
   fi

   echo ""
   echo "==> Building: $dep"

   # Force ExternalProject to re-run configure so arg changes take effect.
   invalidate_dep_configure "$dep"

   if cmake --build "$BUILD_DIR" --target "$dep" --config "$CONFIG" 2>&1; then
      stamp "$dep"
      BUILT+=("$dep")
      echo "    [ok] $dep"
   else
      FAILED+=("$dep")
      echo "    [FAIL] $dep"
      echo ""
      echo "Re-run with: $0 --config $CONFIG --only $dep"
      # Don't exit -- continue to build independent deps
   fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "=== Summary ==="
[[ ${#SKIPPED[@]} -gt 0 ]] && echo "Cached:  ${SKIPPED[*]}"
[[ ${#BUILT[@]}   -gt 0 ]] && echo "Built:   ${BUILT[*]}"
[[ ${#FAILED[@]}  -gt 0 ]] && echo "FAILED:  ${FAILED[*]}"
echo ""

if [[ ${#FAILED[@]} -gt 0 ]]; then
   echo "Fix failures, then re-run. Only failed deps rebuild."
   exit 1
fi

echo "All deps ready."
