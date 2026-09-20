#!/usr/bin/env bash
# Build Sneeze on DGX Spark (linux-arm64) after install-prereqs-dgx.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SNEEZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SNEEZE_DIR"

source "$HOME/.cargo/env" 2>/dev/null || true

# Merge canonical when fork is ahead (ff-only would fail).
bash ./scripts/sync-with-upstream.sh --no-build

# Deps already built? Incremental Sneeze-only rebuild is enough after pull.
if [[ -f builds/linux-arm64/install/release/lib/libSneeze.a ]]; then
  bash ./scripts/build-linux.sh
else
  bash ./scripts/build-linux.sh --all
fi

LIB=builds/linux-arm64/install/release/lib/libSneeze.a
BIN=builds/linux-arm64/install/release/bin
ls -la "$LIB"
test -x "$BIN/SneezeTest"

echo "Smoke tests (Wasm + Net)..."
"$BIN/SneezeTest" --wasm --net
