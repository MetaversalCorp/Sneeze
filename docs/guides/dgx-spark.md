---
title: Building on DGX Spark (linux-arm64)
tier: Guides
audience: [contributor, integrator]
sources:
  - scripts/build-linux.sh
  - scripts/install-prereqs-dgx.sh
  - scripts/build-dgx-spark.sh
  - tests/TestRunner.cpp
verified: bdc905d
nav:
  prev: guides/building.md
  next: guides/contributing.md
---

# Building on DGX Spark (linux-arm64)

NVIDIA DGX Spark is **linux-arm64**. Sneeze uses the same two-tree build as other Linux platforms (`deps/` then `src/`); see [Building Sneeze](building.md) for the mental model. This page is the DGX-specific shortcut.

## Prerequisites (one-time, requires sudo)

```bash
./scripts/install-prereqs-dgx.sh
```

Installs: clang, lld, libc++, Vulkan/X11 dev headers, Rust, Go, NASM.

## Build (~1–2 hours first run)

```bash
./scripts/build-dgx-spark.sh
```

The script runs [`sync-with-upstream.sh`](../../scripts/sync-with-upstream.sh) before building (merge canonical when the fork is ahead — `git pull --ff-only` is not safe in that case). See [Fork sync with MetaversalCorp](fork-sync.md).

Artifacts:

- `builds/linux-arm64/install/release/lib/libSneeze.a`
- Test binaries under `builds/linux-arm64/install/release/bin/`

## Smoke tests

```bash
bin=builds/linux-arm64/install/release/bin
"$bin/SneezeTest" --wasm --net
```

(`WasmTest` / `NetTest` were merged into `SneezeTest` — use suite flags.)

XR suites may warn without a VR runtime — expected on headless DGX.

---

[Building Sneeze](building.md) · [Guides](index.md) · [Home](../Home.md)
