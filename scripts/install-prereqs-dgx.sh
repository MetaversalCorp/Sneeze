#!/usr/bin/env bash
# Install Sneeze engine build prerequisites on Ubuntu 24.04 aarch64 (DGX Spark).
# Requires sudo once.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SNEEZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  clang lld libc++-dev libc++abi-dev libvulkan-dev libgl-dev libx11-dev \
  ninja-build git cmake python3 golang nasm curl

if ! command -v rustc >/dev/null; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
fi

echo "Prerequisites ready. Build with:"
echo "  cd \"$SNEEZE_DIR\" && ./scripts/build-linux.sh --all"
