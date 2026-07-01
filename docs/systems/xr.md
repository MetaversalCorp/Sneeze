---
title: XR System
tier: Systems
audience: [evaluator, integrator, contributor]
sources:
  - src/deps/xr/XrRuntime.h
  - src/deps/xr/XrRuntime.cpp
  - src/deps/xr/XrRuntime_Stub.cpp
verified: b487fd1
nav:
  prev: systems/compute.md
  next: systems/ui.md
---

# XR System

The XR system is the engine's door to virtual- and augmented-reality hardware. A metaverse browser must run on a flat monitor and inside a headset alike, which means the engine has to *discover* whether an XR device is present without assuming one is — and without falling over on the overwhelmingly common machine that has none. This page explains why XR support is built as a graceful, optional probe, what `XR_RUNTIME` reports, and how the build-time and run-time switches combine so the rest of the engine can stay blissfully unaware of the details.

It is a thin wrapper over **OpenXR** (SDK 1.1.58), the cross-vendor standard for VR/AR device access. The class lives in namespace `SNEEZE::DEP` and feeds the [Viewport](viewport.md), which owns rendering.

---

## Why it exists

**OpenXR** is to headsets what a graphics API is to GPUs: one vendor-neutral interface that abstracts away whether the device behind it is from one manufacturer or another, on the desktop or standalone. Adopting it is how the engine avoids hard-coding to any single headset.

But VR/AR is the exception, not the rule. The vast majority of machines running the engine have no headset and no XR runtime installed at all, and a build targeting a platform like iOS may not even have the OpenXR SDK available. A metaverse browser cannot treat "no headset" as a failure — it has to start normally on a laptop and simply report that immersive output is unavailable. So the design goal here is not "drive a headset"; it is **degrade gracefully**: detect what is there, never crash over what isn't, and expose the answer through one uniform predicate the rest of the engine can branch on.

---

## A header with two bodies

`XR_RUNTIME` is defined once but implemented twice, and which body you get is chosen at **CMake-configure time** by the `SNEEZE_ENABLE_XR` switch. The header is deliberately OpenXR-free — it uses the pimpl idiom (`class Impl; Impl* m_pImpl;`) so that consumers never need the OpenXR SDK on their include path regardless of which implementation is compiled.

- When `SNEEZE_ENABLE_XR` is **ON**, `XrRuntime.cpp` provides the real OpenXR loader implementation.
- When it is **OFF**, `XrRuntime_Stub.cpp` provides a no-op stub.

The [engine](engine.md) constructs one `XR_RUNTIME` during startup either way, handing it the owning `ENGINE*`; the build-time choice decides which body runs, not whether the object exists. This is what keeps every other subsystem free of `#ifdef`s — they all just call the same three methods.

The public surface is tiny:

- `Initialize()` — probe for a runtime.
- `HasRuntime()` — did we find a usable XR runtime?
- `GetRuntimeName()` — the runtime's name when one was found, else empty.

---

## What the real implementation does

`Initialize` in the real body walks the standard OpenXR startup. First it silences the loader's own stderr diagnostics by setting `XR_LOADER_DEBUG=none`, because without that the loader prints alarming `Error` lines on the perfectly ordinary machine that simply has no runtime installed — the engine would rather report that condition in its own clear words. It then fills out an `XrApplicationInfo` (application name `"Sneeze"`, engine name `"MBE"`, requesting API version 1.0) with no API layers or extensions enabled, and calls `xrCreateInstance`.

The result splits into the two outcomes that matter:

- **No runtime present.** `xrCreateInstance` fails, which is *not* treated as an error. `HasRuntime()` is set to false, two informative warnings are logged making clear this is normal on a machine without a headset, and `Initialize` still returns `true`. The engine starts; immersive output is just off.
- **Runtime present.** The instance is created, `HasRuntime()` becomes true, and the code queries `XrInstanceProperties` to record and log the runtime's name and version.

The destructor mirrors creation: if an instance was created, it is destroyed with `xrDestroyInstance`.

---

## What the stub does

The stub body exists so that platforms without the OpenXR SDK still compile and behave predictably. Its `Impl` is empty, `Initialize` logs that "OpenXR support disabled at build time" and returns `true`, `HasRuntime()` always returns `false`, and `GetRuntimeName()` returns an empty string.

The payoff is uniformity. Three different situations — a real loader that found a runtime, a real loader that found none, and a stub build with no loader at all — all converge on the same observable contract: `Initialize` succeeds, and `HasRuntime()` tells the caller whether immersive output is available. Nothing downstream needs to know which of the three it is.

```text
SNEEZE_ENABLE_XR = ON                         SNEEZE_ENABLE_XR = OFF
        │                                              │
   XrRuntime.cpp                                XrRuntime_Stub.cpp
        │                                              │
  xrCreateInstance                                  (no-op)
   ┌────┴─────┐                                        │
 success    failure                                    │
   │           │                                       │
HasRuntime  HasRuntime ───────────────────────────► HasRuntime
  = true     = false                                  = false
```

---

## Threading

`XR_RUNTIME` is initialized once on the engine's startup thread and thereafter its `HasRuntime()` / `GetRuntimeName()` accessors are read-only reflections of state fixed at initialization. There is no internal locking because there is no post-init mutation; once the probe is done the answer does not change for the runtime's lifetime.

---

## Current limitations

The system is honest about being a probe and nothing more.

- **Detection only.** `XR_RUNTIME` creates an OpenXR *instance* and reports its presence and name. It does not create a session, swapchains, a frame loop, or read controller input — there is no path yet from "a headset exists" to "render the scene into it."

- **No extensions or layers.** The instance is created with zero enabled extensions and zero API layers, so device-specific or optional OpenXR features are not yet reachable.

- **Fixed application identity and API version.** The application/engine names and the requested API version (1.0) are hard-coded in `Initialize`.

- **Headset rendering belongs elsewhere, and isn't built.** Driving frames to an XR device is a [Viewport](viewport.md) concern that has not been wired to this runtime; today XR availability is informational.

---

## See also

- [Viewport](viewport.md) — the rendering pipeline that would ultimately present frames to an XR device.
- [Engine](engine.md) — constructs the `XR_RUNTIME` during startup and owns its lifetime.

---

[Systems index](index.md) · Previous: [Compute](compute.md) · Next: [UI](ui.md)
