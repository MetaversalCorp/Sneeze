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
# ci-dep-fingerprint.sh <dep>
#
# Print a hex digest that changes whenever ANYTHING that could alter <dep>'s
# built output changes -- for use as a CI cache key. It folds together, for
# <dep> and its full transitive closure (from deps/dependencies.json):
#   * the pinned ref of each dep;
#   * for a BRANCH ref, the current upstream tip (git ls-remote) so the cache
#     invalidates when the branch advances -- best effort: an unreachable remote
#     falls back to the branch name (private branch deps live in later, always-
#     rebuilt tiers, so this matters only for the public branch leaves in tier0);
#   * the contents of each dep's deps/<name>.cmake recipe;
# plus the shared deps CMake (CMakeLists.txt, DepGraph.cmake, dependencies.json).
# This fixes the transitive-invalidation gap: a dep's cache now tracks its deps'
# refs, not just its own recipe file.

set -euo pipefail

DEP="${1:?usage: ci-dep-fingerprint.sh <dep>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PYTHON:-python3}"
DG="$ROOT/tools/DepGraph/depgraph.py"

sha() {
   if command -v sha256sum >/dev/null 2>&1; then
      sha256sum
   else
      shasum -a 256
   fi
}

# <dep> plus its transitive closure, de-duplicated and sorted for a stable order.
# tr -d '\r' guards against a CRLF-emitting python (e.g. Windows local testing).
DEPS="$( { echo "$DEP"; "$PY" "$DG" closure "$DEP"; } | tr -d '\r' | sort -u )"

acc=""
for d in $DEPS; do
   pin="$("$PY" "$DG" pin "$d" | tr -d '\r')"
   folder="$(printf '%s' "$pin" | cut -f1)"
   ref="$(printf '%s' "$pin" | cut -f2)"
   url="$(printf '%s' "$pin" | cut -f3)"

   tip=""
   if git ls-remote --heads "$url" "refs/heads/$ref" >/tmp/_ci_lsr 2>/dev/null && [ -s /tmp/_ci_lsr ]; then
      tip="$(cut -f1 /tmp/_ci_lsr | head -n1)"
   fi

   rhash=""
   recipe="$ROOT/deps/$d.cmake"
   [ -f "$recipe" ] && rhash="$(sha < "$recipe" | cut -d' ' -f1)"

   acc="${acc}${d}|${ref}|${tip}|${rhash}"$'\n'
done

for f in deps/CMakeLists.txt deps/DepGraph.cmake deps/dependencies.json; do
   if [ -f "$ROOT/$f" ]; then
      acc="${acc}${f}|$(sha < "$ROOT/$f" | cut -d' ' -f1)"$'\n'
   fi
done

printf '%s' "$acc" | sha | cut -d' ' -f1
