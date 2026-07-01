---
title: SPIR-V System
tier: Systems
audience: [evaluator, integrator, contributor]
sources:
  - src/deps/spirv/SpvPipeline.h
  - src/deps/spirv/SpvPipeline.cpp
verified: b487fd1
nav:
  prev: systems/wasm.md
  next: systems/compute.md
---

# SPIR-V System

The SPIR-V system is the engine's gatekeeper for GPU shader bytecode. When content supplies a shader — and in the open metaverse shaders arrive from untrusted sources just like everything else — the engine must confirm that the bytecode is well-formed before it goes anywhere near a graphics driver. This page explains why that check exists, what `SPV_PIPELINE` actually does, and the narrow shape of its current implementation.

It is a thin wrapper over **SPIRV-Tools** (vulkan-sdk-1.4.341.0). The class lives in namespace `SNEEZE::DEP` and pairs naturally with the [Compute](compute.md) system, which dispatches kernels, and the [Viewport](viewport.md), which owns rendering.

---

## Why it exists

**SPIR-V** (Standard Portable Intermediate Representation) is the binary interchange format for GPU shaders and compute kernels — a stream of 32-bit words that a driver consumes to build a pipeline. It is the lingua franca the engine's [core abstractions](../overview/what-is-omb.md) settle on for shaders: content authors compile from GLSL or another front end down to SPIR-V, and the engine consumes the SPIR-V.

The problem is that a graphics driver is one of the least forgiving consumers on the machine. Handing a driver malformed or hostile bytecode can do anything from a silent wrong result to a device hang to a crash that takes the whole process down. A web browser would never pass an unvalidated shader to the GPU, and neither can a metaverse browser. The SPIR-V system exists to be the validation gate: foreign bytecode passes through it first, and only well-formed, profile-conformant modules are allowed onward to the renderer or the compute dispatcher.

---

## What SPV_PIPELINE is

`SPV_PIPELINE` is a single, small class with one real method. It is owned by the [engine](engine.md) — constructed once during startup, immediately after the WASM runtime — and held for the engine's lifetime. It is constructed with the owning `ENGINE*`, which it records for logging.

The argument-less `Initialize()` marks the pipeline ready, logs that the validator is up, and returns. There is no heavy state to build; the SPIRV-Tools validator is created fresh per call, so the pipeline itself is effectively stateless once initialized.

`Validate(const std::vector<uint32_t>& aBinary, std::string& sError)` is the method that matters. It constructs a SPIRV-Tools validator pinned to the **Vulkan 1.3 environment** (`SPV_ENV_VULKAN_1_3`), installs a message consumer that accumulates any diagnostics into a string, and runs the validator over the binary. It returns `true` when the module is valid; on failure it returns `false` and copies the collected diagnostic text into `sError` so the caller can log or surface exactly what was wrong.

Pinning to the Vulkan 1.3 profile is a deliberate choice, not a detail: the validator does not merely check that the words form legal SPIR-V in the abstract, it checks conformance to the rules a Vulkan 1.3 implementation will enforce. That makes a pass here a meaningful predictor that a real driver will accept the module.

---

## Runtime behavior

A validation call is synchronous and self-contained: build validator, run, report. Because each call creates its own SPIRV-Tools instance, there is no shared mutable state between calls and nothing to reset between them. The pipeline holds no compiled artifacts and caches nothing — validation is a pure predicate over the bytes it is given.

```text
content SPIR-V (uint32_t words)
        │
        ▼
  SPV_PIPELINE::Validate  ──valid──▶  safe to dispatch / render
        │
        └──invalid──▶  sError filled, module rejected
```

The validator is exercised today through the dedicated test suite (`tests/SpvPipelineTest.cpp`), which is how its behavior is pinned down. The [Compute](compute.md) system is the natural consumer for runtime-loaded kernels — its embedded kernels are validated at build time, while any module arriving at runtime is expected to pass `Validate` before dispatch.

---

## Threading

`SPV_PIPELINE` holds no shared mutable state, so validation is inherently re-entrant: each call builds and tears down its own validator on the calling thread. There is no internal locking because there is nothing to protect. Multiple threads may validate concurrently as long as each passes its own binary and its own error string.

---

## Current limitations

These reflect the code as it stands.

- **Validation only.** SPIRV-Tools also offers assembly (text → binary) and disassembly (binary → text), but `SPV_PIPELINE` wraps only `Validate`. There is no API here to assemble human-readable SPIR-V or to disassemble a module for inspection.

- **The content-path gate is not yet wired.** The validator is in place and engine-owned, and validation is its intended role before any content SPIR-V reaches the renderer or compute dispatcher, but that call site is not yet hooked up. Today the pipeline is driven by its test suite rather than by a live content load.

- **Fixed target environment.** The validator is hard-pinned to `SPV_ENV_VULKAN_1_3`. There is no way to select a different SPIR-V or Vulkan profile per module.

- **Initialization cannot fail meaningfully.** `Initialize` always returns `true`, and the destructor only flips a flag — there is no resource to acquire or release, so there is no real failure or teardown path to reason about.

---

## See also

- [Compute](compute.md) — dispatches SPIR-V kernels on the GPU; the consumer that validation protects.
- [Viewport](viewport.md) — the rendering pipeline that shaders ultimately feed.
- [WASM](wasm.md) — the other sandbox boundary, for content *logic* rather than content *shaders*.

---

[Systems index](index.md) · Previous: [WASM](wasm.md) · Next: [Compute](compute.md)
