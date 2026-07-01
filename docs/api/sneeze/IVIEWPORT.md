---
title: IVIEWPORT (interface reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Sneeze.h
  - src/context/viewport/Viewport.cpp
  - src/sneeze/control/Compositor.cpp
verified: b487fd1
nav:
  prev: api/sneeze/ICONTEXT.md
  next: api/context/index.md
---

# `IVIEWPORT`

The per-viewport **rendering interface** — the contract through which the engine asks the host for a surface to draw into and hands back finished frames. A host implements `IVIEWPORT` and passes it to a viewport's `Activate` (see the [Viewport API](../viewport/index.md)); from then on the engine's compositor agent calls it each frame to learn the target window, query the current frame size, and deliver rendered pixels. It is three methods, all on the rendering hot path. For how rendering is scheduled, see the [Control system](../../systems/control.md); for the viewport itself, the [Viewport system](../../systems/viewport.md).

```cpp
class IVIEWPORT
{
public:
   virtual ~IVIEWPORT () = default;

   virtual void* FrameWindow () = 0;
   virtual bool  FrameSize   (int& nWidth, int& nHeight) = 0;

   virtual void  OnFrameReady (const uint32_t* pFB, int nFbW, int nFbH) = 0;
};
```

---

## Role and ownership

- **Implemented by** the host application; passed to `VIEWPORT::Activate`.
- **Held by** the `VIEWPORT` while it is active; reached by the compositor agent as `pViewport->Host()`. The host owns the object and **must keep it alive for as long as the viewport is active** (until `Deactivate`).
- **Direction is engine → host.** All three methods are called by the engine — the host never calls them.
- **All three are pure virtual** — unlike [`ICONTEXT`](ICONTEXT.md), a host must implement every method; there are no default bodies.

---

## Threading and pitfalls

- **Every method runs on the compositor agent thread, not the host's thread.** `FrameWindow` is read once at activation; `FrameSize` and `OnFrameReady` are called from the compositor agent on every frame. A host implementation **must be thread-safe** with respect to its own UI/windowing state.
- **`OnFrameReady` is on the per-frame critical path.** It is called while the compositor holds the captured framebuffer, between `FrameBuffer_Capture` and `FrameBuffer_Release`. Copy or blit the pixels quickly and return; do not block, and do not retain `pFB` past the call — the buffer is released as soon as `OnFrameReady` returns.
- **`OnFrameReady` only fires for off-surface rendering.** When the renderer draws directly to a native surface, the engine never reads back a framebuffer and `OnFrameReady` is not called for that viewport. A host that provides a native window via `FrameWindow` should expect frames to appear on that surface rather than through `OnFrameReady`.
- **`FrameSize`'s return value drives resizes.** Returning `true` tells the compositor the size changed, which triggers a viewport and renderer resize that frame; returning `false` means "unchanged." Report changes accurately or the render target will not track the window.

---

## Methods

### `virtual void* FrameWindow () = 0`
- **Implemented by.** The host.
- **Called by.** The viewport, once, during `Activate` — to obtain a native window handle for the renderer.
- **Returns.** An opaque platform window handle, or `nullptr` for headless / off-surface rendering. If non-null, the renderer targets that native surface; if null, the engine renders off-screen and delivers frames through `OnFrameReady`.
- **Contract.** The returned handle must remain valid while the viewport is active.

### `virtual bool FrameSize (int& nWidth, int& nHeight) = 0`
- **Implemented by.** The host.
- **Called by.** The compositor agent — at renderer creation (`Execute_Create`) and at the start of every render (`Execute_Render`).
- **Parameters.** `nWidth`, `nHeight` — in/out: the host writes the current target size in pixels.
- **Returns.** `true` if the size changed since the last query (the compositor then resizes the viewport and renderer this frame); `false` if unchanged.
- **Contract.** Must report the current drawable size and signal changes via the return value. Called every frame, so keep it cheap.

### `virtual void OnFrameReady (const uint32_t* pFB, int nFbW, int nFbH) = 0`
- **Implemented by.** The host.
- **Called by.** The compositor agent in `Execute_Present`, after a frame is rendered and read back — only when not rendering to a native surface.
- **Parameters.** `pFB` — pointer to the frame's pixels (`nFbW × nFbH`, 32-bit per pixel); `nFbW`, `nFbH` — the framebuffer dimensions.
- **Contract.** Present the pixels (copy, upload to a texture, blit to a window). **Do not retain `pFB`** — it points into a buffer the compositor releases the moment this returns. Return promptly; the call sits between the compositor's capture and release of the frame buffer.

---

## See also

- [Viewport system](../../systems/viewport.md) — the rendering pipeline and camera behind this interface.
- [Viewport API](../viewport/index.md) — where `IVIEWPORT` is activated and deactivated.
- [Control system](../../systems/control.md) — the compositor agent that calls these methods each frame.
- [ENGINE](ENGINE.md) · [ICONTEXT](ICONTEXT.md) — the other host-facing engine interfaces.

---

[Engine API](index.md) · Prev: [ICONTEXT](ICONTEXT.md) · Next: [Context API](../context/index.md)
