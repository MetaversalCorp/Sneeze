---
title: Viewport API
tier: API
audience: [integrator, contributor]
sources:
  - include/Viewport.h
verified: b487fd1
nav:
  prev: api/scene/NODE.md
  next: api/viewport/VIEWPORT.md
---

# Viewport API

The viewport subsystem's public surface is declared in `include/Viewport.h`. It is the engine's rendering surface and orbit camera: the object an embedding application activates to turn a live [scene](../../systems/scene.md) into frames, and through which it feeds input and receives finished pixels. For the *architecture* — deferred renderer creation under thread affinity, the compositor-driven frame loop, the native-surface vs. readback paths — read the [Viewport system](../../systems/viewport.md) page. This section is the precise per-class reference: each page documents every public method's purpose, parameters, return value, and the pitfalls (threading, lifetime, the framebuffer handoff) to watch for.

```cpp
#include <Viewport.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `VIEWPORT` | [VIEWPORT](VIEWPORT.md) | The per-context rendering surface; owns the renderer, input, framebuffer, camera, and timing. |
| `VIEWPORT::RENDERER` | [RENDERER](RENDERER.md) | The abstract rendering backend interface (concrete implementation is internal). |

`VIEWPORT` uses the pimpl idiom. It also publishes three nested helpers used across the host boundary:

- **`VIEWPORT::VIEW`** — the orbit-camera state (angles, distance, look-at target) with an `Update` method that folds one frame of input into the camera.
- **`VIEWPORT::CAMERA`** — the absolute world pose (position in metres + orientation quaternion) a scene can set to place the camera; the compositor re-seeds the orbit `VIEW` from it while active.
- **`VIEWPORT::INPUT`** — a plain struct of accumulated input deltas (mouse, scroll, buttons, key flags) produced by the host and consumed once per frame.

All three are documented in full on the [VIEWPORT](VIEWPORT.md) page.

> **Who calls this.** An application embedding the engine owns the host side of > the contract: it implements `IVIEWPORT` (declared with the engine's public > interfaces), calls `Activate` to start rendering, pushes input through > `Input_Mouse` / `Input_Key`, and receives frames via the host's `OnFrameReady`. > Most other members are driven by the engine's compositor and are documented here > for contributors.

## The host interface (`IVIEWPORT`)

A viewport only renders while a host is attached. The host implements three methods:

| Method | Purpose |
|---|---|
| `void* FrameWindow ()` | An optional native window handle to render directly into; null to use the readback path. |
| `bool FrameSize (int& w, int& h)` | The current drawable size; returns whether it changed since last asked. |
| `void OnFrameReady (const uint32_t* pFB, int w, int h)` | Receives a finished frame on the readback path. |

---

## See also

- [Viewport system](../../systems/viewport.md) — design, frame loop, threading, limitations.
- [Scene API](../scene/index.md) — the model a viewport renders.
- [MSF API](../msf/index.md) — the signed manifest format (next section).

---

[API index](../index.md) · Prev: [NODE](../scene/NODE.md) · Next: [VIEWPORT](VIEWPORT.md)
