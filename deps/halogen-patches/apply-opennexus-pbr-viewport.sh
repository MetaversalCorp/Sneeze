#!/usr/bin/env bash
# Apply Space-Time / Scene Assembler PBR viewport patches onto Halogen v1.1.10.
# Safe to run repeatedly; skips when physicallyBased.mat already has trim logic.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HALOGEN="${HALOGEN_ROOT:-$ROOT/deps/repos/Halogen}"
PATCH="$(dirname "$0")/opennexus-pbr-viewport.patch"
TAG="v1.1.10"

if [[ ! -d "$HALOGEN/.git" ]]; then
   echo "[halogen-pbr] skip: no Halogen git checkout at $HALOGEN"
   exit 0
fi

if rg -q 'isTrim' "$HALOGEN/src/materials/physicallyBased.mat" 2>/dev/null; then
   echo "[halogen-pbr] already applied"
   exit 0
fi

echo "[halogen-pbr] applying patch to $HALOGEN (base $TAG)"
cd "$HALOGEN"
git fetch --tags origin 2>/dev/null || true
git checkout "$TAG"
git apply "$PATCH"
echo "[halogen-pbr] done — rebuild Halogen (scripts/build-linux.sh --only halogen)"
