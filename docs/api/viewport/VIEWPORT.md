---
title: VIEWPORT (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Viewport.h
  - src/context/viewport/Viewport.cpp
  - src/sneeze/control/Compositor.cpp
verified: b487fd1
nav:
  prev: api/viewport/index.md
  next: api/viewport/RENDERER.md
---

# `VIEWPORT`

The engine's rendering surface and orbit camera. One `VIEWPORT` exists per [`CONTEXT`](../context/index.md) (per browsing session). It owns the renderer once created, the accumulated input state, the readback framebuffer, the camera, and per-frame timing counters — but it only renders while a host (an `IVIEWPORT`) is attached. The [scene](../scene/index.md) it draws is owned by the context and reached through `Scene()`, not owned by the viewport. For the conceptual picture — deferred renderer creation, the compositor frame loop, the two paths to the screen — see the [Viewport system](../../systems/viewport.md); this page is the exact behavior of every public member.

```cpp
class VIEWPORT
{
public:
   class RENDERER;        // abstract backend (defined privately)
   class VIEW;            // orbit-camera state
   struct CAMERA;         // absolute world pose (position + orientation)
   struct INPUT;          // accumulated input deltas

   explicit VIEWPORT (CONTEXT* pContext);
   ~VIEWPORT ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Owned by** a `CONTEXT`, constructed with a back-pointer to it.
- **Owns** the [`RENDERER`](RENDERER.md) (once built on the compositor thread), the `INPUT` accumulator, the readback framebuffer, and the `VIEW` camera.
- **Delegates** the scene: it does not own a `SCENE`; `Scene()` resolves the context's scene.
- **Drives rendering** indirectly — `Activate` posts a `JOB_COMPOSITOR` to the engine's compositor pool, which runs the actual frame loop.

The owner chain is `VIEWPORT → CONTEXT → ENGINE`. The viewport reaches engine services through the context, never through a cached pointer.

---

## Lifecycle

A viewport is brought up in stages that deliberately defer the renderer onto the correct thread:

1. **Construct + Initialize.** `VIEWPORT(pContext)` then `Initialize()` do almost nothing — no renderer is created, because the constructing thread is not the rendering thread.
2. **Activate.** `Activate(pHost)` records the host and posts a compositor job. On the compositor's lifecycle thread (agent 0) the job calls `Renderer_Initialize()`, which builds the concrete renderer. After that the job cycles render/present continuously.
3. **Deactivate.** `Deactivate()` cancels the job and blocks until the renderer is destroyed on agent 0 (`Renderer_Shutdown()`), then clears the host.

`Renderer_Initialize` / `Renderer_Shutdown` are public but are intended to be called **only** from the compositor's agent 0 (the system does this for you); they exist on the public surface because the compositor job invokes them across the module boundary.

---

## Threading and pitfalls

This is the part to read before calling anything. The viewport bridges the host's threads and the engine's compositor pool, and its members have real concurrency contracts.

**The renderer is single-thread-affine.** It must be created, used, and destroyed on one thread; the engine pins all of that to compositor agent 0. Never call `Renderer_Initialize` / `Renderer_Shutdown` yourself from an arbitrary thread, and never touch the [`RENDERER`](RENDERER.md) returned by `Renderer()` off the compositor thread.

**`Activate` / `Deactivate` are guarded** by a viewport mutex and are idempotent-safe (activating an already-active viewport, or deactivating an inactive one, is a no-op). **`Deactivate` blocks** until the compositor confirms the renderer has been destroyed — so no frame can still be in flight against a freed renderer when it returns.

**Input is a producer/consumer.** `Input_Mouse` / `Input_Key` (host thread) accumulate under an input mutex; `Input_Consume` (compositor) reads and resets the deltas under the same mutex.

**The framebuffer handoff is a lock pair.** `FrameBuffer_Capture` *acquires* the framebuffer mutex and returns the pixel pointer; `FrameBuffer_Release` *releases* it. You must call them as a pair around your read and must not retain the pointer past the release. Forgetting `FrameBuffer_Release` deadlocks the next write.

**Scene invalidation is lock-free**, carried by a single atomic boolean (`Scene_Invalidate` sets it from any thread; `Scene_Invalidate_Consume` test-and-clears it on the compositor).

**The absolute camera pose is mutex-guarded.** `Camera` / `Camera_Active` / `Camera_Deactivate` take a camera mutex, so a pose can be set from any thread while the compositor reads it each frame.

**The timing members are public and unsynchronized.** They are written by the compositor; reading them from another thread is racy but harmless (diagnostics only).

---

## Construction and destruction

```cpp
explicit VIEWPORT (CONTEXT* pContext);
~VIEWPORT ();
bool Initialize ();
```

### `VIEWPORT(pContext)`
- **Purpose.** Construct a viewport owned by `pContext`. No renderer is created. `pContext` must outlive the viewport.

### `~VIEWPORT()`
- **Purpose.** Destroy the viewport. Deactivates first (cancelling the compositor job and tearing the renderer down) if still active.

### `bool Initialize ()`
- **Purpose.** Prepare the viewport. Currently a no-op that returns `true`; the real setup happens at `Activate`.
- **Returns.** `true`.

---

## Activation

```cpp
void Activate (IVIEWPORT* pHost);
void Deactivate ();
bool Renderer_Initialize ();
void Renderer_Shutdown ();
```

### `void Activate (IVIEWPORT* pHost)`
- **Purpose.** Attach the host and begin rendering: records `pHost` and posts a `JOB_COMPOSITOR` to the engine's compositor pool, which creates the renderer on its lifecycle thread and then drives the frame loop.
- **Parameters.** `pHost` — the host surface (window handle, frame size, frame delivery). Must be non-null and outlive the active period.
- **Returns.** Nothing.
- **Pitfalls.** No-op if already active or if `pHost` is null. The renderer is not ready when `Activate` returns — creation happens asynchronously on the compositor.

### `void Deactivate ()`
- **Purpose.** Stop rendering: cancel the compositor job, **block** until the renderer is destroyed on agent 0, then clear the host.
- **Returns.** Nothing.
- **Pitfalls.** Blocking by design. No-op if not active.

### `bool Renderer_Initialize ()`
- **Purpose.** Build the concrete renderer: read the renderer-library name from the engine host, construct the ANARI renderer, pass it the host's native window if any, and initialize it at the current size.
- **Returns.** `true` if the renderer is now live (or was already).
- **Pitfalls.** **Compositor agent 0 only.** Called by the compositor's create step; do not call directly. No-op if a renderer already exists or no host is attached.

### `void Renderer_Shutdown ()`
- **Purpose.** Destroy the renderer.
- **Pitfalls.** **Compositor agent 0 only**, invoked by the job's destroy step.

---

## Accessors

```cpp
ENGINE*    Engine   () const;
CONTEXT*   Context  () const;
IVIEWPORT* Host     () const;
SCENE*     Scene    () const;
bool       IsActive () const;
RENDERER*  Renderer () const;
VIEW&      View     ();
```

| Accessor | Returns | Notes |
|---|---|---|
| `Engine()` | The owning engine (via the context). | Never null for a live viewport. |
| `Context()` | The owning context. | |
| `Host()` | The attached host, or null. | Null when inactive. |
| `Scene()` | The context's scene. | Delegated — the viewport does not own it. |
| `IsActive()` | Whether a host is attached. | |
| `Renderer()` | The concrete renderer, or null. | Null until created on the compositor; touch only on the compositor thread. |
| `View()` | The orbit camera by reference. | Mutated by the compositor each frame; see [VIEW](#view--orbit-camera). |

---

## Input

```cpp
void  Input_Mouse   (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight);
void  Input_Key     (bool bKeySpace, bool bKeyPlus, bool bKeyMinus);
INPUT Input_Consume ();
```

### `void Input_Mouse (nDX, nDY, dScrollY, bMouseLeft, bMouseRight)`
- **Purpose.** Accumulate one mouse event into the pending input. Deltas (`nDX`, `nDY`, `dScrollY`) are summed; button states are latched to the latest values.
- **Parameters.** Mouse movement deltas, scroll delta, and current left/right button states.
- **Returns.** Nothing. Thread-safe (input mutex).

### `void Input_Key (bKeySpace, bKeyPlus, bKeyMinus)`
- **Purpose.** Latch the current state of the tracked keys.
- **Returns.** Nothing. Thread-safe (input mutex).

### `INPUT Input_Consume ()`
- **Purpose.** Snapshot the accumulated input and reset the *deltas* (mouse movement and scroll) to zero, leaving button/key states latched.
- **Returns.** A copy of the `INPUT` struct.
- **Pitfalls.** Called by the compositor once per frame; the reset is why two consumers would each get only part of the motion.

---

## Framebuffer

```cpp
void            FrameBuffer_Write   (const uint32_t* pPixels, int nWidth, int nHeight);
const uint32_t* FrameBuffer_Capture (int& nWidth, int& nHeight);
void            FrameBuffer_Release ();
```

These implement the readback handoff between the renderer (producer) and the host (consumer). They are unused on the native-surface path.

### `void FrameBuffer_Write (pPixels, nWidth, nHeight)`
- **Purpose.** Copy a finished frame's pixels into the viewport's internal buffer (resizing it to `nWidth × nHeight`).
- **Parameters.** `pPixels` — source RGBA pixels; `nWidth`, `nHeight` — dimensions.
- **Returns.** Nothing. Takes the framebuffer mutex for the copy.

### `const uint32_t* FrameBuffer_Capture (int& nWidth, int& nHeight)`
- **Purpose.** Begin a read of the current frame: **acquire** the framebuffer mutex and return a pointer to the pixels plus their dimensions.
- **Parameters.** `nWidth`, `nHeight` — out-parameters set to the buffer dimensions.
- **Returns.** The pixel pointer, or null if no frame has been written.
- **Pitfalls.** Leaves the mutex **held**. You must call `FrameBuffer_Release` when done, and must not use the pointer afterward.

### `void FrameBuffer_Release ()`
- **Purpose.** Release the framebuffer mutex acquired by `FrameBuffer_Capture`.
- **Pitfalls.** Must be paired with exactly one preceding `FrameBuffer_Capture`.

---

## Dimensions

```cpp
void Size   (int& nWidth, int& nHeight);
void Resize (int  nWidth, int  nHeight);
```

### `void Size (int& nWidth, int& nHeight)`
- **Purpose.** Read the viewport's current logical size into the out-parameters.

### `void Resize (int nWidth, int nHeight)`
- **Purpose.** Set the viewport's logical size. The compositor calls this when the host reports a size change and forwards the new size to the renderer.

---

## Camera — absolute world pose

Alongside the interactive orbit [`VIEW`](#view--orbit-camera), the viewport carries an **absolute world pose**: a position in metres and an orientation quaternion. It is the long-term camera model — a scene (or, later, a WASM service) can place the camera at a world pose, and the compositor drives the orbit camera from it until the user takes over. The orbit `VIEW` is a temporary interaction stop-gap; this pose is where the camera is *told* to be.

```cpp
struct CAMERA
{
   double aPosition[3] = { 0.0, 0.0, 0.0 };   // metres
   double aRotation[4] = { 0.0, 0.0, 0.0, 1.0 };   // quaternion (x, y, z, w)
};

void   Camera            (const CAMERA& Camera);
CAMERA Camera            () const;
bool   Camera_Active     (CAMERA& Camera) const;
void   Camera_Deactivate ();
```

### `void Camera (const CAMERA& Camera)`
- **Purpose.** Set the absolute world pose and mark it **active**. While active, the compositor re-seeds the orbit `VIEW` from this pose *every frame* — so the camera self-corrects as the scene streams in and `dMaxReach` (and thus the render scale) settles.
- **Pitfalls.** Thread-safe (a camera mutex). Setting the pose does not move the camera instantly; the compositor applies it on the next frame, and only once the scene has real extent (a metre-scale pose against an empty scene is ignored).

### `CAMERA Camera () const`
- **Purpose.** Read the current stored pose. Thread-safe.

### `bool Camera_Active (CAMERA& Camera) const`
- **Purpose.** Report whether a pose is active and, if so, copy it out. The compositor calls this each frame to decide whether to re-seed the orbit camera.
- **Returns.** `true` if a pose is active; `Camera` is written only then.

### `void Camera_Deactivate ()`
- **Purpose.** Clear the active flag so the compositor stops re-seeding and the user's orbit input takes over. The compositor calls this itself on any mouse movement, orbit, or zoom.

---

## Scene invalidation

```cpp
void Scene_Invalidate         ();
bool Scene_Invalidate_Consume ();
```

### `void Scene_Invalidate ()`
- **Purpose.** Request that the renderer rebuild its retained scene from scratch on the next frame. Set from any thread (typically the scene after a navigation swap) by storing an atomic flag.
- **Returns.** Nothing.

### `bool Scene_Invalidate_Consume ()`
- **Purpose.** Atomically test-and-clear the invalidation flag.
- **Returns.** `true` if a rebuild was requested since the last consume.
- **Pitfalls.** Called by the compositor before each frame; consuming clears the request.

---

## Frame timing and diagnostics

```cpp
enum eACCUMULATE
{
   kACCUMULATE_INPUT,
   kACCUMULATE_SCENE,
   kACCUMULATE_SUBMIT,
   kACCUMULATE_RENDER,
   kACCUMULATE_PUBLISH,
};

void Accumulate  (eACCUMULATE eType, std::chrono::steady_clock::time_point tpStart);
void Accumulate  (eACCUMULATE eType, double dSeconds);
void Diagnostics ();
```

### `void Accumulate (eACCUMULATE eType, ...)`
- **Purpose.** Add a duration to one of the per-phase accumulators. The time-point overload adds the elapsed time since `tpStart`; the `double` overload adds a precomputed number of seconds.
- **Parameters.** `eType` — which phase (input, scene traversal, submit, render, or publish); `tpStart` or `dSeconds` — the duration source.
- **Returns.** Nothing.

### `void Diagnostics ()`
- **Purpose.** Advance the frame counter and, once per accumulated second, log an averaged per-phase breakdown (frame / input / scene / submit / render / publish milliseconds) to the engine log at trace level, then reset the accumulators.
- **Returns.** Nothing.
- **Notes.** Called by the compositor at the end of each present step.

### Public timing members

These are written by the compositor each frame and exposed directly (no accessors). They are diagnostics; reading them off-thread is racy but harmless.

| Member | Meaning |
|---|---|
| `std::chrono::steady_clock::time_point m_tpLastFrame` | Timestamp of the last completed frame, for FPS averaging. |
| `int64_t m_tmNow` | The simulation tick the current frame is rendered at. |
| `int m_nFrameCount` | Frames accumulated in the current diagnostics window. |
| `double m_dFpsAccum` | Wall-clock seconds accumulated in the current window. |
| `double m_dAccumInput` | Summed input-phase seconds in the window. |
| `double m_dAccumScene` | Summed scene-traversal seconds. |
| `double m_dAccumSubmit` | Summed submit-phase seconds. |
| `double m_dAccumRender` | Summed render-phase seconds. |
| `double m_dAccumPublish` | Summed publish-phase seconds. |

---

## VIEW — orbit camera

`VIEWPORT::VIEW` holds the spherical orbit-camera state; each viewport owns one, reached by reference through `View()`.

```cpp
class VIEW
{
public:
   float m_dTheta    = 0.3f;   // azimuth (radians)
   float m_dPhi      = 0.4f;   // elevation (radians)
   float m_dDistance = 10.0f;  // distance from target
   VEC3  m_vTarget   = { 0.0, 0.0, 0.0 };   // orbit pivot (world)

   void Update (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight);
};
```

### `void Update (nDX, nDY, dScrollY, bMouseLeft, bMouseRight)`
- **Purpose.** Fold one frame of input into the camera. Left-drag adjusts azimuth and elevation (elevation clamped to just under ±90°); scroll multiplies the distance (clamped to a sane range).
- **Parameters.** Mouse deltas, scroll delta, and button states (from `Input_Consume`).
- **Returns.** Nothing.
- **Notes.** The compositor converts `(theta, phi, distance, target)` to a Cartesian camera each frame. The right-button case is reserved for panning; the current `Update` acts on left-drag and scroll.

---

## INPUT — accumulated input

`VIEWPORT::INPUT` is a plain struct of pending input, written by the host and drained by the compositor.

```cpp
struct INPUT
{
   int   nMouseDX    = 0;
   int   nMouseDY    = 0;
   float dScrollY    = 0.0f;
   bool  bMouseLeft  = false;
   bool  bMouseRight = false;

   bool  bKeySpace   = false;
   bool  bKeyPlus    = false;
   bool  bKeyMinus   = false;
};
```

Mouse and scroll fields are *deltas* accumulated since the last `Input_Consume` (and zeroed by it); button and key fields are latched current state.

---

## See also

- [Viewport system](../../systems/viewport.md) — design, frame loop, threading, limitations.
- [RENDERER](RENDERER.md) — the abstract backend the viewport drives.
- [Scene API](../scene/index.md) — the model the compositor traverses to feed the renderer.
- [Context API](../context/index.md) — the owner of the viewport and the scene.

---

[Viewport API](index.md) · Prev: [index](index.md) · Next: [RENDERER](RENDERER.md)
