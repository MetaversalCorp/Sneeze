#!/usr/bin/env bash
# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build every Sneeze dependency on one macOS runner, in tier order.
# Each dep keeps the same actions/cache key the matrix jobs use
# (macos-<dep>-v2-univ3-<fingerprint>). A hit is restored and not rebuilt.
# A miss is compiled, slimmed, and saved under that key.
#
# CACHE_ACTION must be a checkout of actions/cache @ v4 (the dist/ tree).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PLATFORM="${PLATFORM:-macos}"
LIBS="libs-${PLATFORM}"
CACHE_ACTION="${CACHE_ACTION:?CACHE_ACTION must point at a checkout of actions/cache v4}"
JOBS="$(sysctl -n hw.ncpu)"

if [ ! -f "$CACHE_ACTION/dist/restore-only/index.js" ]; then
   echo "ci-macos-deps: missing $CACHE_ACTION/dist/restore-only/index.js"
   exit 1
fi

folder_of() {
   python3 -c "import json,sys; print(json.load(open('deps/dependencies.json'))['versions'][sys.argv[1]]['folder'])" "$1"
}

# Drop the ExternalProject build tree. Filament keeps build/tools (matc and
# friends) because a cross-compile of halogen looks there. The install tree stays.
slim_dep() {
   local dep="$1"
   local dir="$2"
   if [ ! -d "$dir/build" ]; then
      return 0
   fi
   if [ "$dep" = "filament" ] && [ -d "$dir/build/tools" ]; then
      mkdir -p "$dir/.tools-keep"
      cp -a "$dir/build/tools" "$dir/.tools-keep/"
      rm -rf "$dir/build"
      mkdir -p "$dir/build"
      mv "$dir/.tools-keep/tools" "$dir/build/tools"
      rm -rf "$dir/.tools-keep"
   else
      rm -rf "$dir/build"
   fi
}

# Run one actions/cache v4 entrypoint. Prints cache-hit=true/false to stdout.
cache_js() {
   local entry="$1"
   local path="$2"
   local key="$3"
   local out state envf
   out="$(mktemp)"
   state="$(mktemp)"
   envf="$(mktemp)"
   set +e
   env \
      "GITHUB_OUTPUT=$out" \
      "GITHUB_STATE=$state" \
      "GITHUB_ENV=$envf" \
      "INPUT_PATH=$path" \
      "INPUT_KEY=$key" \
      "INPUT_RESTORE-KEYS=" \
      "INPUT_ENABLECROSSOSARCHIVE=false" \
      "INPUT_FAIL-ON-CACHE-MISS=false" \
      "INPUT_LOOKUP-ONLY=false" \
      "INPUT_UPLOAD-CHUNK-SIZE=33554432" \
      node "$CACHE_ACTION/dist/$entry"
   local rc=$?
   set -e
   cat "$out"
   rm -f "$out" "$state" "$envf"
   return "$rc"
}

output_is_true() {
   # actions/core writes either "cache-hit=true" or a delimited block:
   #   cache-hit<<DELIMITER
   #   true
   #   DELIMITER
   printf '%s\n' "$1" | awk '
      /^cache-hit=true$/ { found=1 }
      /^cache-hit<</ { take=1; next }
      take { if ($0 == "true") found=1; take=0 }
      END { exit found ? 0 : 1 }
   '
}

build_dep() {
   local dep="$1"
   local build_dir="build-deps/$dep"
   rm -rf "$build_dir"
   cmake -S deps -B "$build_dir" -G Ninja \
      -DDEP="$dep" \
      -DLIBS_DIR="${ROOT}/${LIBS}" \
      -DSNEEZE_DEP_REPO="${ROOT}/deps/repos" \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
   cmake --build "$build_dir" --parallel "$JOBS"
   rm -rf "$build_dir"
}

mkdir -p "$LIBS"

while IFS= read -r dep; do
   [ -n "$dep" ] || continue
   folder="$(folder_of "$dep")"
   fp="$(scripts/ci-dep-fingerprint.sh "$dep")"
   key="${PLATFORM}-${dep}-v2-univ3-${fp}"
   path="${LIBS}/${folder}"
   echo "=== $dep ($folder) key=$key ==="

   set +e
   hit_out="$(cache_js restore-only/index.js "$path" "$key")"
   rc=$?
   set -e
   if [ "$rc" -ne 0 ]; then
      echo "cache restore failed for $dep"
      printf '%s\n' "$hit_out"
      exit 1
   fi
   printf '%s\n' "$hit_out"
   if output_is_true "$hit_out"; then
      echo "cache hit: $dep"
      slim_dep "$dep" "$path"
      continue
   fi

   echo "cache miss: $dep"
   build_dep "$dep"
   if [ ! -d "$path/install" ]; then
      echo "ci-macos-deps: $dep produced no install tree at $path/install"
      exit 1
   fi
   bash scripts/verify-macos-universal-libs.sh "$path/install"
   slim_dep "$dep" "$path"
   set +e
   cache_js save-only/index.js "$path" "$key" >/dev/null
   rc=$?
   set -e
   if [ "$rc" -ne 0 ]; then
      echo "cache save failed for $dep"
      exit 1
   fi
   echo "cache saved: $dep"
done < <(python3 scripts/ci-tier-matrix.py --ordered)

echo "ci-macos-deps: done"
