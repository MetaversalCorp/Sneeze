---
title: Systems
tier: Systems
sources: []
verified: b487fd1
---

# Systems

One page per subsystem. Each explains the subsystem's job, the problem it solves, its internal design, and how it behaves at runtime. Each links across to its matching [API](../api/index.md) page for exact class and method signatures.

## Engine core
- [Engine](engine.md) — the single entry point; owns module lifecycle and contexts.
- [Control](control.md) — the engine thread, agent pools, metronome, and job queues.

## Engine-wide services
- [Network](network.md) — resource fetching and the on-disk cache; one per engine, opened per container as a `CACHE`.
- [Storage](storage.md) — persistent JSON document storage; one per engine, opened per container as a `SILO`.
- [Console](console.md) — the developer console; one per engine, opened per container as a `STREAM`.

## Per-context subsystems
- [Context](context.md) — a single browsing session (a tab); owns its scene, viewport, and containers, and reaches the engine-wide services above through per-container handles.
- [Container](container.md) — the runtime identity and sandbox of one signed content source.
- [Scene](scene.md) — the scene object model: fabrics, nodes, and map objects.
- [Viewport](viewport.md) — rendering surface, camera, and the framebuffer handoff.

## Dependency-backed subsystems
- [MSF](msf.md) — signed spatial-fabric files: parsing, signing, and trust verification.
- [WASM](wasm.md) — the sandboxed execution runtime for content code.
- [SPIR-V](spirv.md) — GPU shader validation.
- [Compute](compute.md) — GPU compute dispatch with CPU fallback.
- [XR](xr.md) — VR/AR device access.
- [UI](ui.md) — the HTML/CSS UI toolkit.
- [Persona](persona.md) — the local identity proxy.

---

[Home](../Home.md)
