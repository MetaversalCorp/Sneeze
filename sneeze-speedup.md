# Sneeze Rendering Performance — Blocking Calls and Solutions

## Problem Statement

Runtime performance on Windows is slower than expected. Investigation traced the bottleneck to two blocking synchronization points inside Halogen's `Frame::renderFrame()`, both of which execute every frame within the `anariRenderFrame` call. The `anariFrameReady(ANARI_WAIT)` call that follows in Sneeze is a no-op — Halogen ignores the wait mask entirely and the frame is already complete.

### Blocking Point 1: Filament FIFO Vsync (~16.7ms per viewport)

Filament hardcodes `VK_PRESENT_MODE_FIFO_KHR` in its Vulkan swapchain creation. FIFO blocks `renderer->beginFrame(swapChain)` until the display's vsync releases a swapchain image — approximately 16.67ms at 60 Hz.

**File:** `libs/Halogen/src/filament/backend/src/vulkan/platform/VulkanPlatformSwapChainImpl.cpp` line 221

```cpp
VkPresentModeKHR const desiredPresentMode = VK_PRESENT_MODE_FIFO_KHR;
```

With N viewports rendered sequentially on the compositor's agent 0 thread, total frame time is N × 16ms. Two viewports = 30 FPS each. Ten viewports = 6 FPS each.

### Blocking Point 2: `engine->flushAndWait()` per frame

At the start of every `renderFrame()`, Halogen calls `engine->flushAndWait()` to ensure all pending GPU uploads and any previous readback are complete. This is a full GPU pipeline drain — the CPU stalls until the GPU finishes its entire command queue.

**File:** `deps/repos/Halogen/src/Frame.cpp` line 113

```cpp
engine->flushAndWait();
```

### Why `anariFrameReady(ANARI_WAIT)` is irrelevant

Sneeze calls `anariFrameReady(m_pDevice, m_pFrame, ANARI_WAIT)` immediately after `anariRenderFrame` in `AnariRenderer.cpp` line 616. However, Halogen's implementation ignores the wait mask:

```cpp
// deps/repos/Halogen/src/Frame.cpp line 355
int Frame::frameReady(ANARIWaitMask)
{
    return mFrameReady ? 1 : 0;
}
```

Since `renderFrame()` is fully synchronous and sets `mFrameReady = true` before returning, this always returns 1 immediately. Changing `ANARI_WAIT` to `ANARI_NO_WAIT` in Sneeze is harmless but performance-neutral.

---

## Solution 1: MAILBOX Present Mode (Filament Change)

**Impact:** Eliminates ~16.7ms vsync stall per viewport. Single-viewport render time drops from ~16.8ms to <1ms. Multi-viewport scales additively instead of multiplicatively.

**Effort:** Small — one constant change plus compositor-side frame pacing.

### Changes Required

#### 1a. Filament: Change present mode constant

**File:** `libs/Halogen/src/filament/backend/src/vulkan/platform/VulkanPlatformSwapChainImpl.cpp`

Replace line 221:
```cpp
VkPresentModeKHR const desiredPresentMode = VK_PRESENT_MODE_FIFO_KHR;
```

With:
```cpp
VkPresentModeKHR const desiredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
```

MAILBOX doesn't tear and doesn't block — the GPU renders as fast as it can, only the latest completed frame is shown at the next vsync. `beginFrame()` returns in under 1ms.

**Fallback concern:** MAILBOX is widely supported on desktop GPUs but not universally guaranteed. The existing validation check on lines 225-228 will catch unsupported devices at runtime. For robustness, change the logic to prefer MAILBOX with FIFO fallback:

```cpp
VkPresentModeKHR desiredPresentMode = VK_PRESENT_MODE_FIFO_KHR;
FixedCapacityVector<VkPresentModeKHR> presentModes = fvkutils::enumerate(
        vkGetPhysicalDeviceSurfacePresentModesKHR, mPhysicalDevice, mSurface);
if (std::find(presentModes.begin(), presentModes.end(),
              VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end()) {
    desiredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
}
bool const foundSuitablePresentMode = std::find(presentModes.begin(), presentModes.end(),
                                        desiredPresentMode) != presentModes.end();
FILAMENT_CHECK_POSTCONDITION(foundSuitablePresentMode)
        << "Desired present mode is not supported by this device.";
```

#### 1b. Sneeze: Add frame pacing to compositor

Without FIFO vsync, the compositor will render as fast as possible, burning CPU and GPU power. The Sneeze metronome already provides frame-pacing infrastructure — wire it to throttle the compositor's render loop to the target frame rate (e.g., 60 FPS = 16.67ms per frame, or match the display refresh rate).

### Trade-offs

- **Power consumption:** Without vsync throttling, the GPU renders flat-out. The compositor must implement its own pacing (sleep or yield between frames).
- **Input latency:** Improves. MAILBOX presents the most recently completed frame at vsync, reducing the pipeline depth compared to FIFO's queue.
- **Tearing:** None. MAILBOX is tear-free (unlike IMMEDIATE mode).
- **Compatibility:** MAILBOX may not be available on some mobile GPUs or older drivers. The fallback logic above handles this gracefully.

---

## Solution 2: Remove/Guard `flushAndWait()` in Halogen (Halogen Change)

**Impact:** Eliminates per-frame GPU pipeline drain. Allows GPU pipelining (CPU prepares frame N+1 while GPU renders frame N).

**Effort:** Small to medium — needs dirty tracking or fence-based synchronization.

### Changes Required

#### 2a. Option A: Conditional flush (minimal change)

**File:** `deps/repos/Halogen/src/Frame.cpp`

The `flushAndWait()` on line 113 exists to ensure pending GPU uploads (texture data, buffer data) are complete before rendering. Most frames have no pending uploads — the scene is already on the GPU.

Add a dirty flag to `DeviceState` that tracks whether any upload occurred since the last flush:

```cpp
// In DeviceState (or Frame):
bool mHasPendingUploads = false;
```

Guard the flush:

```cpp
void Frame::renderFrame()
{
    DeviceState * const state = deviceState();
    filament::Engine * const engine = state->engine;
    filament::Renderer * const renderer = state->renderer.get();

    state->commitBuffer.flush();

    if (!isValid())
        return;

    if (state->mHasPendingUploads || mReadbackScheduled) {
        engine->flushAndWait();
        state->mHasPendingUploads = false;
        mReadbackScheduled = false;
    }

    // ... rest of renderFrame unchanged ...
```

Set `mHasPendingUploads = true` in any code path that uploads data to the GPU (texture creation, buffer updates, etc.).

#### 2b. Option B: Fence-based synchronization (better pipelining)

Instead of `flushAndWait()` (which drains the entire GPU queue), use Filament's `Fence` API to wait only on the previous frame's completion:

```cpp
// In Frame (member):
filament::Fence *mPreviousFrameFence = nullptr;

void Frame::renderFrame()
{
    // Wait only for the previous frame, not the entire GPU queue
    if (mPreviousFrameFence) {
        mPreviousFrameFence->wait(
            filament::Fence::Mode::FLUSH,
            filament::Fence::FENCE_WAIT_FOR_EVER);
        state->engine->destroy(mPreviousFrameFence);
        mPreviousFrameFence = nullptr;
    }

    // ... render ...

    // Create a fence after this frame's work is submitted
    mPreviousFrameFence = state->engine->createFence();
}
```

This allows frames N-2 and earlier to remain in the GPU pipeline while only blocking on frame N-1.

### Trade-offs

- **Option A** is safe and conservative — identical behavior when uploads are pending, zero-cost skip when they aren't. Most frames in a steady-state scene skip the flush entirely.
- **Option B** allows deeper GPU pipelining but adds fence management complexity. The improvement is most noticeable when GPU work per frame is significant (complex scenes, high resolution).
- Both options are backward-compatible — the rendering output is identical.

---

## Solution 3: Async `renderFrame()` + Honest `frameReady()` (Halogen + Sneeze Change)

**Impact:** Enables CPU/GPU overlap. Scene traversal and ANARI parameter setup for frame N+1 runs while the GPU renders frame N.

**Effort:** Medium to large — threading model change in Halogen, state machine change in Sneeze compositor.

### Overview

Currently, `renderFrame()` is fully synchronous:

```
CPU: [scene traversal] → [anariRenderFrame BLOCKS] → [anariFrameReady (no-op)] → [present]
GPU:                      [============= render ==============]
```

With async rendering:

```
CPU: [scene traversal N] → [submit N] → [scene traversal N+1] → [wait N] → [submit N+1] → ...
GPU:                        [======= render N =======]           [======= render N+1 =======]
```

### Changes Required

#### 3a. Halogen: Make `renderFrame()` non-blocking

**File:** `deps/repos/Halogen/src/Frame.cpp`

Restructure `renderFrame()` to submit GPU work and return immediately without waiting for completion:

```cpp
void Frame::renderFrame()
{
    DeviceState * const state = deviceState();
    filament::Engine * const engine = state->engine;
    filament::Renderer * const renderer = state->renderer.get();

    state->commitBuffer.flush();

    if (!isValid()) {
        mFrameReady = true;  // Nothing to wait for
        return;
    }

    mFrameReady = false;

    // ... scene setup (view, camera, viewport, lighting — same as current) ...

    if (nativeSurface) {
        filament::SwapChain *sc = mNativeSurface->swapChain();
        if (renderer->beginFrame(sc)) {
            renderer->render(mView.get());
            renderer->endFrame();
        }
        // DO NOT set mFrameReady = true here.
        // The GPU is still working. Completion detected in frameReady().
    } else {
        // Offscreen path: similar restructure
        // ...
    }
}
```

#### 3b. Halogen: Implement honest `frameReady()`

**File:** `deps/repos/Halogen/src/Frame.cpp`

Replace the current no-op implementation with proper wait/poll behavior:

```cpp
int Frame::frameReady(ANARIWaitMask m)
{
    if (mFrameReady)
        return 1;

    if (m == ANARI_NO_WAIT) {
        // Poll: check if the GPU fence has signaled
        // (Implementation depends on how Filament exposes fence status)
        return checkGpuFenceSignaled() ? 1 : 0;
    }

    // ANARI_WAIT: block until GPU work completes
    DeviceState *const state = deviceState();
    state->engine->flushAndWait();
    mFrameReady = true;
    return 1;
}
```

This requires Filament fence integration — either via `filament::Fence` or by tracking the Vulkan fence/semaphore that `endFrame()` signals.

#### 3c. Sneeze: Restructure compositor state machine

**File:** `src/sneeze/control/Compositor.cpp` and `src/context/viewport/AnariRenderer.cpp`

Split the current `Execute_Render` → `Execute_Present` flow into three states:

1. **SUBMIT** — Scene traversal + `anariRenderFrame` (returns immediately)
2. **WAIT** — Poll `anariFrameReady(ANARI_NO_WAIT)`. If not ready, yield and re-enter WAIT. If ready, transition to PRESENT.
3. **PRESENT** — Framebuffer readback (offscreen path only) + publish

During the WAIT state, the compositor agent can process other viewports' SUBMIT work, achieving GPU-level parallelism across viewports.

In `AnariRenderer.cpp`, change the render call from blocking to fire-and-forget:

```cpp
void RENDERER::ANARI::EndFrame ()
{
    // ... scene build/update (unchanged) ...

    anariCommitParameters (m_pDevice, m_pWorld);
    anariCommitParameters (m_pDevice, m_pFrame);

    auto tpRenderStart = std::chrono::steady_clock::now ();
    m_dLastSubmitSeconds = std::chrono::duration<double> (tpRenderStart - tpSubmitStart).count ();

    anariRenderFrame (m_pDevice, m_pFrame);
    // DO NOT call anariFrameReady here — the compositor polls it in the WAIT state
}

bool RENDERER::ANARI::IsFrameReady () const
{
    return anariFrameReady (m_pDevice, m_pFrame, ANARI_NO_WAIT) != 0;
}

void RENDERER::ANARI::WaitForFrame ()
{
    anariFrameReady (m_pDevice, m_pFrame, ANARI_WAIT);
}
```

### Trade-offs

- **Significant complexity.** The compositor state machine, renderer interface, and Halogen's frame lifecycle all need coordinated changes.
- **Filament thread constraint.** All Filament API calls must be on one thread (agent 0). Async `renderFrame` doesn't mean multi-threaded Filament — it means the single compositor thread can interleave submission and polling across multiple viewports.
- **Best combined with Solution 1.** Without MAILBOX, `beginFrame()` still blocks for vsync inside `renderFrame()`, negating much of the async benefit. With MAILBOX, `renderFrame()` returns in <1ms and the WAIT state polls GPU completion independently.
- **Prerequisite for Solution 2B.** Fence-based sync naturally fits the async model — the fence created at submission is polled in `frameReady()`.

---

## Recommended Implementation Order

| Priority | Solution | Impact | Effort | Dependencies |
|----------|----------|--------|--------|--------------|
| **1** | Solution 1 — MAILBOX present mode | Eliminates ~16.7ms vsync stall per viewport | Small | None |
| **2** | Solution 2A — Conditional `flushAndWait` | Skips GPU drain on steady-state frames | Small | None |
| **3** | Solution 3 — Async render pipeline | CPU/GPU overlap, multi-viewport parallelism | Large | Solutions 1 + 2B recommended first |

Solution 1 alone should resolve the "slower than expected" perception. It is a single constant change in Filament plus compositor-side pacing. Solutions 2 and 3 provide additional headroom as scene complexity and viewport count grow.

---

## File Reference

| File | Repo | Purpose |
|------|------|---------|
| `libs/Halogen/src/filament/backend/src/vulkan/platform/VulkanPlatformSwapChainImpl.cpp:221` | Halogen (Filament subtree) | Hardcoded `VK_PRESENT_MODE_FIFO_KHR` |
| `libs/Halogen/src/filament/backend/include/backend/DriverEnums.h:58-112` | Halogen (Filament subtree) | `SWAP_CHAIN_CONFIG_*` flags (next free bit: `0x100`) |
| `libs/Halogen/src/filament/include/filament/SwapChain.h:152-249` | Halogen (Filament subtree) | Public `CONFIG_*` constants |
| `deps/repos/Halogen/src/Frame.cpp:100-305` | Halogen (ANARI device) | `renderFrame()` — sync render + `flushAndWait` |
| `deps/repos/Halogen/src/Frame.cpp:355-358` | Halogen (ANARI device) | `frameReady()` — ignores wait mask |
| `deps/repos/Halogen/src/NativeSurface.cpp:25-31` | Halogen (ANARI device) | `rebuildSwapChain()` — passes flags to Filament |
| `src/context/viewport/AnariRenderer.cpp:615-616` | Sneeze | `anariRenderFrame` + `anariFrameReady(ANARI_WAIT)` |
| `src/sneeze/control/Compositor.cpp:1042-1053` | Sneeze | Compositor render → present state machine |
