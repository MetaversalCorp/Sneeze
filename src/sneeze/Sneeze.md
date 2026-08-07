# Sneeze — Engine Core

The `sneeze` module is the heart of the Sneeze engine. It owns the ENGINE class
(the single entry point for the host application), the THREAD base class for
managed thread lifecycle, and the foundational types (VEC3, QUAT, constants).

## ENGINE

ENGINE is the single entry point the host application instantiates.
Uses the pimpl idiom. Constructor takes `IENGINE*` (host interface).

```cpp
#include <Sneeze.h>

auto* pHost = new ENGINE_HOST ();   // implements IENGINE
ENGINE engine (pHost);
engine.Initialize ();

// Open a context (one per browser tab)
auto* pContextHost = new CONTEXT_HOST ();   // implements ICONTEXT
CONTEXT* pContext = engine.Context_Open (pContextHost, "https://example.com/world.msf", kSESSION_PERSISTENT);

// Attach a viewport to the context (activates rendering)
auto* pViewportHost = new VIEWPORT_HOST ();   // implements IVIEWPORT
pContext->Viewport ()->Activate (pViewportHost);

// Application event loop feeds input to the viewport
pContext->Viewport ()->Input_Mouse (dx, dy, scroll, bLeft, bRight);
pContext->Viewport ()->Input_Key (bSpace, bPlus, bMinus);

// Teardown
pContext->Viewport ()->Deactivate ();
engine.Context_Close (pContext);
```

### Initialization

`Initialize()` is parameterless — reads configuration from the `IENGINE*` host.
Uses nested `if` success pattern with `bool m_bInitialized`. Creates engine-level
services in order:

1. curl_global_init
2. WASM_RUNTIME
3. SPV_PIPELINE
4. XR_RUNTIME (if enabled)
5. UI_CONTEXT
6. PERSONA
7. CONTROL (spawns the engine thread, creates all agent pools)
8. InitializePaths (cache directory structure, orphan cleanup)

Shutdown tears down in reverse order.

### Context Lifecycle

- `Context_Open(pHost, sUrl, kSession)` — computes permanent/temporary paths,
  creates CONTEXT, adds to `m_apContext` (add-before-init), calls Initialize
- `Context_Close(pContext)` — captures temporary path, deletes, erases, queues
  cleanup via SCRUB

### Persona

```cpp
engine.Login ("Dean", "Abramson");
engine.Logout ();
engine.ChangePersona ("Jane", "Doe");   // Logout + Login
```

### Path Management

ENGINE owns three path strings:
- `m_sPath_Persistent` — `sAppDataPath/Sneeze/Cache/Persistent/`
- `m_sPath_Transitory` — `sAppDataPath/Sneeze/Cache/Transitory/`
- `m_sPath_Transitory_Session` — `.../Transitory/s<8-hex>/`

`InitializePaths()` creates directories, scans for orphaned session folders,
and queues them for cleanup via AGENT::SCRUB.

## IENGINE

Host interface between the application and the engine.

```cpp
class IENGINE
{
public:
   virtual std::string const& sAppDataPath () const& = 0;
   virtual std::string const& sRenderer ()    const& = 0;
   virtual void Log (eLOGLEVEL, const std::string& sModule, const std::string& sMessage) = 0;
};
```

## ICONTEXT

Per-context host interface for inspector callbacks. Optional virtual callbacks
(default no-op) for NETWORK file, STORAGE silo, CONSOLE entry, and CONTAINER
lifecycle notifications. `OnContainerCreated(CONTAINER*)` /
`OnContainerDeleted(CONTAINER*)` fire when a container's resources open and
close — the `CONTAINER*` exposes `Stream()` and `Silo()` for inspection.

## IVIEWPORT

Per-viewport host interface for rendering.

```cpp
class IVIEWPORT
{
public:
   virtual void* FrameWindow () = 0;
   virtual void  FrameSize (int& nWidth, int& nHeight) = 0;
   virtual void  OnFrameReady (const uint32_t* pFB, int nFbW, int nFbH) = 0;
};
```

## THREAD

Base class for managed thread lifecycle. Declared in `Engine.h`, implemented
in `Thread.cpp`. All managed threads (CONTROL, every AGENT) inherit THREAD.

- `Initialize()` — spawns thread, blocks until `Ready()` is called
- `Main()` — pure virtual entry point
- `Wait(predicate)` / `Wait(duration)` — neither reads shutdown flag
- `Signal(bShutdown)` — wakes thread, optionally latches shutdown
- `Join()` — signals shutdown + joins; must be called in every derived destructor
- `~THREAD()` — calls Join() as safety net, then deletes thread

## Types

`Types.h` defines foundational types used across all modules:

```cpp
struct VEC3 { double dX, dY, dZ; };
struct QUAT { double dX, dY, dZ, dW; };

constexpr double PI      = 3.14159265358979323846;
constexpr double TWO_PI  = 6.28318530717958647692;
constexpr double AU_M    = 149597870700.0;
```

## Files

| File | Contents |
|------|----------|
| `Engine.cpp` | ENGINE::Impl (pimpl body, subsystem lifecycle, context management, paths) |
| `Engine.h` | THREAD base class declaration |
| `Thread.cpp` | THREAD implementation (spawn, signal, wait, shutdown, join) |
| `Types.h` | VEC3, QUAT, constants |
