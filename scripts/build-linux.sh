#!/usr/bin/env bash
# Linux build. Auto-detects arch (x64 or arm64).
#
# Default: compile + link Sneeze only. Plain `cmake --build` against the
# Sneeze build tree. No dep checks, no configure step. Fails naturally if
# the tree or the dep libraries aren't there yet.
#
# The Sneeze src tree is a SINGLE multi-config tree at
#   builds/linux-<arch>/build/
# that emits Debug or Release into
#   builds/linux-<arch>/install/{debug,release}/{bin,lib}/
# depending on the --config flag (which drives `cmake --build --config`).
# Uses the Ninja Multi-Config generator so one build tree carries both
# configurations and --config just selects at build time.
#
# The DEPS trees stay per-config (deps/builds/linux-<arch>/{debug,release}/)
# and both must be built on disk before you can build a config whose deps
# don't exist yet.
#
# Flags switch the script into deps mode or deps+Sneeze mode:
#
#   --deps         Build the third-party libs into deps/builds/linux-<arch>/<config>/libs/.
#   --fresh        Reconfigure the Sneeze tree from scratch (cmake -S src --fresh).
#                  Wipes CMakeCache.txt + CMakeFiles/ so stale cached values
#                  (compiler paths, toolchain tweaks, find_package results, etc.)
#                  can't linger. Does NOT build -- just regenerates the project
#                  files. Deps tree is never touched. Requires CMake >= 3.24.
#   --all          Build deps, then configure + build Sneeze.
#   --only <dep>   Build a single dep (implies deps-targeting).
#   --list         Show dep stamp cache.
#   --verify       Read-only (implies deps-targeting): report each dependency's
#                  checkout vs deps/dependencies.json (FRESHNESS mode). Same
#                  role as build-windows.ps1 -Verify. Does not build Sneeze.
#   --sync         Modifier (implies deps-targeting): move the dep(s) in scope
#                  to the immutable tag their deps/<dep>.cmake pins, then force a
#                  rebuild. Without it, a tag mismatch is a hard error.
#   --rebuild      Modifier: force a full rebuild of whatever target(s) are
#                  selected by the other flags, regardless of prior build state.
#                  NEVER crosses the src <-> deps wall on its own. Matrix:
#                    --rebuild                  scrub + rebuild Sneeze only
#                    --rebuild --deps           scrub + rebuild all deps
#                    --rebuild --only <dep>     scrub + rebuild one dep
#                    --rebuild --all            scrub + rebuild deps, then Sneeze
#                  Source clones in deps/repos/ are never scrubbed.
#
# HARD RULE: the deps folder (deps/builds/<platform>/<config>/) may only be
# modified when --deps, --only, or --all is present on the command line. A
# Sneeze-only invocation (anything else, including --fresh or --rebuild alone)
# cannot touch a single bit inside deps/.
#
# The deps tree (deps/CMakeLists.txt) and the Sneeze tree (src/CMakeLists.txt)
# are two completely independent CMake projects. They share nothing. This
# script is the only glue: in --all mode it builds deps, then configures +
# builds Sneeze in a separate CMake invocation.
#
# Debug and Release live in fully separate DEPS trees under
# deps/builds/linux-<arch>/{debug,release}/ but share a single Sneeze build
# tree at builds/linux-<arch>/build/ and distinct install trees at
# builds/linux-<arch>/install/{debug,release}/. Source clones in deps/repos/
# are shared across configs.
#
# Usage:
#   ./scripts/build-linux.sh                      # Sneeze (Release)
#   ./scripts/build-linux.sh --config Debug       # Sneeze (Debug)
#   ./scripts/build-linux.sh --fresh              # Reconfigure Sneeze (no build)
#   ./scripts/build-linux.sh --deps               # Deps only
#   ./scripts/build-linux.sh --all                # Deps, then Sneeze
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SNEEZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CONFIG="Release"
DEPS=0
ALL=0
FRESH=0
REBUILD=0       # modifier; composes with other flags, does not imply deps mode
DEPS_FORWARD=0  # --only / --list set this (they target deps/)
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
   case "$1" in
      --deps)         DEPS=1 ;;
      --all)          ALL=1 ;;
      --fresh)        FRESH=1 ;;
      --rebuild)      REBUILD=1 ;;
      --config)       shift; CONFIG="$1" ;;
      --config=*)     CONFIG="${1#--config=}" ;;
      --only|--list)  DEPS_FORWARD=1; EXTRA_ARGS+=("$1") ;;
      --only=*)       DEPS_FORWARD=1; EXTRA_ARGS+=("$1") ;;
      --sync)         DEPS_FORWARD=1; EXTRA_ARGS+=("$1") ;;
      --verify)       DEPS_FORWARD=1; EXTRA_ARGS+=("$1") ;;
      *)              EXTRA_ARGS+=("$1") ;;
   esac
   shift
done

MODE_COUNT=$((DEPS + ALL + FRESH))
if [[ $MODE_COUNT -gt 1 ]]; then
   echo "--deps, --all, and --fresh are mutually exclusive" >&2
   exit 1
fi

case "$CONFIG" in
   Debug|Release) : ;;
   *) echo "--config must be Debug or Release (got '$CONFIG')" >&2; exit 1 ;;
esac

case "$(uname -m)" in
   aarch64|arm64) PLATFORM="linux-arm64" ;;
   *)             PLATFORM="linux-x64" ;;
esac

CFG_LOWER="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
DEPS_BUILD_DIR="$SNEEZE_DIR/deps/builds/$PLATFORM/$CFG_LOWER/build"
LIBS_DIR="$SNEEZE_DIR/deps/builds/$PLATFORM/$CFG_LOWER/libs"
# Single multi-config Sneeze tree. --config only drives `cmake --build --config`.
SNEEZE_OUT_DIR="$SNEEZE_DIR/builds/$PLATFORM"
SNEEZE_BUILD_DIR="$SNEEZE_OUT_DIR/build"
SNEEZE_INSTALL_DIR="$SNEEZE_OUT_DIR/install/$CFG_LOWER"
TOOLCHAIN="$SNEEZE_DIR/cmake/toolchain-linux-clang.cmake"

# --rebuild is a modifier, not a mode: it does NOT imply deps-targeting.
# HARD RULE: if none of --deps, --only, or --all is set, the deps folder
# must not be touched -- regardless of what --rebuild / --fresh are doing.
# (--list is read-only and handled inside build-deps.sh.)
DEPS_MODE=0
if [[ $DEPS -eq 1 || $ALL -eq 1 || $DEPS_FORWARD -eq 1 ]]; then
   DEPS_MODE=1
fi

SNEEZE_MODE=0
if [[ $DEPS_MODE -eq 0 || $ALL -eq 1 ]]; then
   SNEEZE_MODE=1
fi

# --rebuild forwards to build-deps.sh only when deps are actually in scope.
# When Sneeze-only, --rebuild is handled entirely below (wipe Sneeze tree).
if [[ $DEPS_MODE -eq 1 && $REBUILD -eq 1 ]]; then
   EXTRA_ARGS+=(--rebuild)
fi

# Reconfigure the Sneeze tree before building. Implied by --all and --fresh.
# --rebuild does NOT force reconfigure any more: it cleans via `cmake --build
# --target clean` which preserves the configured tree (CMakeCache, CMakeFiles,
# generated project files), so the IDE doesn't lose state. Exception: if
# --rebuild targets Sneeze but the tree has never been configured, fall back to
# configuring it -- otherwise the subsequent build would fail with a cryptic
# "CMakeCache.txt missing" error.
RECONFIGURE=0
if [[ $ALL -eq 1 || $FRESH -eq 1 ]]; then
   RECONFIGURE=1
fi
if [[ $REBUILD -eq 1 && $SNEEZE_MODE -eq 1 && ! -f "$SNEEZE_BUILD_DIR/CMakeCache.txt" ]]; then
   RECONFIGURE=1
fi

# ---------------------------------------------------------------------------
# Deps mode
# ---------------------------------------------------------------------------

if [[ $DEPS_MODE -eq 1 ]]; then
   echo "==> Linux $PLATFORM deps build ($CONFIG, clang + libc++)"

   "$SCRIPT_DIR/build-deps.sh" \
      --config "$CONFIG" \
      --platform "$PLATFORM" \
      --build-dir "$DEPS_BUILD_DIR" \
      --libs-dir "$LIBS_DIR" \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
      "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
fi

# ---------------------------------------------------------------------------
# Sneeze mode -- configure (if --all) + plain `cmake --build`, no dep checks.
# ---------------------------------------------------------------------------

if [[ $FRESH -eq 1 || $SNEEZE_MODE -eq 1 ]]; then
   # --rebuild with Sneeze in scope: clean only the CURRENT config's compiled
   # artifacts via `cmake --build --target clean --config <cfg>`. This preserves
   # the configured CMake tree (CMakeCache.txt, CMakeFiles/, generated project
   # files) so the IDE doesn't lose state, and it preserves the OTHER config's
   # intermediates and install tree. The selected config's install/<cfg>/ is
   # also wiped so stale binaries don't survive the rebuild.
   if [[ $REBUILD -eq 1 && $SNEEZE_MODE -eq 1 ]]; then
      if [[ -f "$SNEEZE_BUILD_DIR/CMakeCache.txt" ]]; then
         echo ""
         echo "==> Cleaning Sneeze $CONFIG build artifacts"
         cmake --build "$SNEEZE_BUILD_DIR" --target clean --config "$CONFIG"
      fi
      echo "==> Scrubbing Sneeze $CONFIG install: $SNEEZE_INSTALL_DIR"
      rm -rf "$SNEEZE_INSTALL_DIR"
   fi

   if [[ $RECONFIGURE -eq 1 ]]; then
      # --fresh (CMake 3.24+) wipes CMakeCache.txt + CMakeFiles/ before
      # reconfiguring -- makes --fresh the explicit "start over" path while
      # --all keeps the idempotent cache update for normal reconfigures.
      FRESH_ARG=()
      if [[ $FRESH -eq 1 ]]; then FRESH_ARG=(--fresh); fi

      echo ""
      echo "==> Configuring Sneeze tree at $SNEEZE_BUILD_DIR"
      # Ninja Multi-Config: one build tree, both Debug and Release selectable
      # via `cmake --build --config`. --config here seeds SNEEZE_CONFIG so
      # find_package resolves against the right deps tree at configure time;
      # actual emission per invocation is decided by --build --config below.
      cmake -S "$SNEEZE_DIR/src" -B "$SNEEZE_BUILD_DIR" \
         -G "Ninja Multi-Config" \
         "${FRESH_ARG[@]+"${FRESH_ARG[@]}"}" \
         -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
         -DLIBS_DIR="$LIBS_DIR" \
         -DSNEEZE_CONFIG="$CONFIG" \
         -DSNEEZE_PLATFORM="$PLATFORM" \
         -DSNEEZE_BUILD_ROOT="$SNEEZE_OUT_DIR"
   fi

   if [[ $FRESH -eq 1 && $REBUILD -eq 0 ]]; then
      echo "==> Sneeze reconfigure complete (no build)"
   else
      echo ""
      echo "==> Building Sneeze ($PLATFORM, $CONFIG)"
      cmake --build "$SNEEZE_BUILD_DIR" --config "$CONFIG"
      echo "==> Sneeze Linux build complete ($CONFIG)"
      echo "    libSneeze.a -> $SNEEZE_INSTALL_DIR/lib"
      echo "    test bins   -> $SNEEZE_INSTALL_DIR/bin"
   fi
fi
