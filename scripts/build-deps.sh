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
#   (script stamp + ExternalProject prefix + libs/<dep>/). Without --only:
#   scrubs the entire per-config dep root.
# --sync: for the dep(s) in scope, if a clone is checked out at the wrong commit
#   for the immutable tag its deps/<dep>.cmake pins, fetch that tag, check it
#   out, and force a rebuild. Without --sync, a tag mismatch is a hard error
#   (the build refuses to silently compile stale source after a GIT_TAG bump).
#   Branch pins (main/master/next_release) are track-latest and never enforced.
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
CMAKE_EXTRA_ARGS=()

# ---------------------------------------------------------------------------
# Dependency graph -- order matters (deps before dependents)
# ---------------------------------------------------------------------------

DEPS_ORDERED=(
   spirv-headers         # no deps
   spirv-tools           # -> spirv-headers
   glslang               # -> spirv-tools
   anari-sdk             # no deps (Debug-normal, consumed by Sneeze)
   openxr-sdk            # no deps (skipped if XR=OFF)
   boringssl             # no deps (src/jws/ crypto + Android curl TLS)
   curl                  # -> boringssl (Android only; native TLS elsewhere)
   freetype              # no deps (RmlUi + FindSneezeFreeType)
   rmlui                 # -> freetype
   nlohmann-json         # no deps
   fastgltf              # no deps (vendors simdjson; glTF loader for src/deps/gltf)
   basisu                # no deps (transcoder-only, vendors zstd; KTX2/basis texture decode)
   jwt-cpp               # header-only (JWS library used by src/jws/)
   spirv-cross           # no deps (SPIR-V -> HLSL / MSL for Vox)
   vox                   # -> spirv-cross (GPU compute dispatch)
   wasmtime              # no deps (Cargo, slow)
   filament              # always Release (consumed only by halogen)
)

DEPS_ORDERED+=(halogen)   # -> filament, anari-sdk

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
   case "$1" in
      --rebuild)      REBUILD=1 ;;
      --only)         shift; ONLY="$1" ;;
      --list)         LIST_ONLY=1 ;;
      --sync)         SYNC=1 ;;
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
#   3. Per-dep build + install trees under libs/<dep>/.
remove_dep_state() {
   unstamp "$1"
   rm -rf "$BUILD_DIR/$1-prefix"
   rm -rf "$LIBS_DIR/$1"
}

list_deps() {
   for dep in "${DEPS_ORDERED[@]}"; do
      local status="pending"
      is_stamped "$dep" && status="cached"
      printf "  %-20s %s\n" "$dep" "$status"
   done
}

# Parse deps/<dep>.cmake for the pinned ref (GIT_TAG). Matches the directive
# only, never the word GIT_TAG inside a comment: the comment is stripped (from
# '#' to end of line) before the line is tested. Echoes nothing when the recipe
# has no git pin (header-only deps).
dep_pin_ref() {
   local f="$SNEEZE_DIR/deps/$1.cmake"
   [[ -f "$f" ]] || return 0
   awk '{ gsub(/\r$/, ""); sub(/#.*/, ""); if (match($0, /GIT_TAG[ \t]+[^ \t]+/)) { s = substr($0, RSTART, RLENGTH); sub(/^GIT_TAG[ \t]+/, "", s); print s; exit } }' "$f"
}

# Parse the source-clone folder from set (_repo "${SNEEZE_DEP_REPO}/<folder>").
# Falls back to the dep name. Echoes the absolute clone path.
dep_pin_repo() {
   local f="$SNEEZE_DIR/deps/$1.cmake"
   local folder=""
   [[ -f "$f" ]] && folder="$(awk 'match($0, /SNEEZE_DEP_REPO\}\/[^"]+/) { s = substr($0, RSTART, RLENGTH); sub(/.*SNEEZE_DEP_REPO\}\//, "", s); print s; exit }' "$f")"
   [[ -n "$folder" ]] || folder="$1"
   echo "$DEP_REPO/$folder"
}

# Guard against the silent-stale-version trap: deps/<dep>.cmake's GIT_TAG only
# governs the FIRST clone; bumping it never moves an existing clone, so a build
# would otherwise recompile the old source with no warning. For deps pinned to
# an immutable tag, verify the clone is on that tag. Branch pins (main/master/
# next_release) are intentionally track-latest and are skipped. On mismatch:
# hard error by default, or auto-correct (fetch + checkout + force rebuild)
# when --sync is given.
assert_dep_checkout() {
   local dep="$1"
   local ref repo head resolved short

   ref="$(dep_pin_ref "$dep")"
   [[ -n "$ref" ]] || return 0
   repo="$(dep_pin_repo "$dep")"
   [[ -d "$repo/.git" ]] || return 0          # not cloned yet; first clone honors the pin

   head="$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)"
   [[ -n "$head" ]] || return 0

   # Cheap offline test: does the pinned ref resolve locally to HEAD? Covers
   # both a tag checked out detached and a branch sitting on its tip.
   resolved="$(git -C "$repo" rev-parse --verify --quiet "${ref}^{commit}" 2>/dev/null || true)"
   [[ -n "$resolved"  &&  "$resolved" == "$head" ]] && return 0

   # Not on the pin. Classify the ref against the remote (only reached on a
   # potential mismatch, so this network call is rare). Branch pins are exempt.
   if [[ -z "$(git -C "$repo" ls-remote --tags origin "refs/tags/$ref" 2>/dev/null || true)" ]]; then
      if [[ -n "$(git -C "$repo" ls-remote --heads origin "refs/heads/$ref" 2>/dev/null || true)" ]]; then
         return 0   # branch pin -- track-latest by design, not enforced
      fi
      echo "WARNING: $dep: could not classify pinned ref '$ref' (offline?); skipping checkout verification." >&2
      return 0
   fi

   short="${head:0:8}"
   if [[ $SYNC -eq 1 ]]; then
      echo "  [sync] $dep at $short -> pinned tag '$ref' (fetch + checkout + rebuild)"
      git -C "$repo" fetch --depth 1 origin tag "$ref"
      git -C "$repo" checkout --detach "$ref"
      remove_dep_state "$dep"
      echo "  [sync] $dep now at $ref"
   else
      cat >&2 <<EOF
$dep is not checked out at the tag its recipe pins.
  pinned (deps/$dep.cmake): $ref
  checked out:              $short   ($repo)

GIT_TAG only governs the first clone; an existing clone is never moved when you
bump it. Re-run with --sync to fetch and check out the pin automatically:
  ./scripts/build-deps.sh --only $dep --sync
or fix it by hand:
  git -C "$repo" fetch --depth 1 origin tag $ref
  git -C "$repo" checkout --detach $ref
EOF
      exit 1
   fi
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
# Verify each targeted clone matches the immutable tag its recipe pins before
# anything configures or builds. Catches the case where GIT_TAG was bumped but
# the existing clone was never moved -- which would otherwise rebuild stale
# source silently. --sync auto-corrects; otherwise this hard-errors. Branch
# pins are track-latest by design and are skipped.
# ---------------------------------------------------------------------------

if [[ -n "$ONLY" ]]; then
   DEPS_TO_CHECK=("$ONLY")
else
   DEPS_TO_CHECK=("${DEPS_ORDERED[@]}")
fi

for dep in "${DEPS_TO_CHECK[@]}"; do
   assert_dep_checkout "$dep"
done

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
