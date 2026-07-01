---
title: IENGINE (interface reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Sneeze.h
  - src/sneeze/Engine.cpp
verified: b487fd1
nav:
  prev: api/sneeze/ENGINE.md
  next: api/sneeze/ICONTEXT.md
---

# `IENGINE`

The engine-level host interface — the few things only the host application can provide. A host **implements** `IENGINE` and hands it to the [`ENGINE`](ENGINE.md) constructor; the engine then **reads configuration** from it during bring-up and **routes all log output** through it for the engine's whole life. It is the smallest of the three host interfaces: two configuration accessors and one log sink. For how the engine consumes these, see the [Engine system](../../systems/engine.md) page.

```cpp
class IENGINE
{
public:
   enum eLOGLEVEL
   {
      kLOGLEVEL_Trace,
      kLOGLEVEL_Info,
      kLOGLEVEL_Warning,
      kLOGLEVEL_Error
   };

   virtual ~IENGINE () = default;

   virtual std::string const& sAppDataPath () const& = 0;
   virtual std::string const& sRenderer    () const& = 0;

   virtual void Log (eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage) = 0;
};
```

---

## Role and ownership

- **Implemented by** the host application. The engine never subclasses it.
- **Held by** the `ENGINE`, which stores the pointer passed to its constructor. The host owns the object; it **must outlive the engine**.
- **Direction is engine → host.** The host writes none of these methods' call sites; the engine calls them. Two are pulled during initialization; the third is called continuously thereafter.

`eLOGLEVEL` is the severity scale shared across the whole engine, from `kLOGLEVEL_Trace` (verbose) to `kLOGLEVEL_Error`.

---

## Threading and pitfalls

- **`Log` is called from many threads.** Engine subsystems and background agents (fetch, scrub, compositor) all log through this interface, so a host implementation **must be thread-safe**. Calls can arrive concurrently from the engine thread, agent threads, and the host's own thread. Keep the implementation non-blocking; it sits on hot paths.
- **The accessors return by `const&` with an lvalue-ref qualifier (`const&`).** The engine reads the referenced strings during `Initialize` (and `sRenderer()` later, at viewport activation). Return a reference to stable storage that outlives those reads — typically a member string — not a temporary.
- **`sAppDataPath()` gates bring-up.** If it returns empty, `ENGINE::Initialize` logs "Host configuration incomplete" and fails. It must name a writable directory.
- **`sRenderer()` is read lazily.** The engine does not validate it at init; it is consulted when a viewport activates its renderer. A bad value fails there, not at engine bring-up.

---

## Methods

Each virtual below is documented with who implements it (always the host), who calls it (always the engine), when, and the contract it must satisfy.

### `virtual std::string const& sAppDataPath () const& = 0`
- **Implemented by.** The host.
- **Called by.** The engine, during `ENGINE::Initialize` — first in a guard that rejects an empty value, then in `InitializePaths` to form the cache root.
- **Contract.** Return the absolute path of a writable per-user data directory. The engine builds `<sAppDataPath>/Sneeze/Cache/{Persistent,Transitory}` beneath it. Must be non-empty or initialization fails. The returned reference must remain valid for the duration of the call.
- **Returns.** The data directory path by const reference.

### `virtual std::string const& sRenderer () const& = 0`
- **Implemented by.** The host.
- **Called by.** The engine — specifically the viewport, when it activates and selects its ANARI rendering library. Not read during engine `Initialize`.
- **Contract.** Return the name of the ANARI renderer library to load (for example a Filament-backed device). May be empty, in which case the viewport skips renderer selection based on it. Because it is read lazily, an invalid name surfaces as a viewport-time failure.
- **Returns.** The renderer name by const reference.

### `virtual void Log (eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage) = 0`
- **Implemented by.** The host.
- **Called by.** Everything in the engine, via `ENGINE::Log`, which forwards here. Called from any thread, continuously, for the engine's whole lifetime.
- **Parameters.** `Level` — severity; `sModule` — a short tag identifying the source subsystem (for example `"SNEEZE"`, `"CONTROL"`, `"NETWORK"`, `"SCRUB"`); `sMessage` — the human-readable text.
- **Contract.** Deliver the line to wherever the host shows logs. **Must be thread-safe and should not block**, as it is called from hot paths and multiple threads at once. No return value; the engine ignores any failure to log.

---

## See also

- [Engine system](../../systems/engine.md) — how the engine reads this interface and where each method is called.
- [ENGINE](ENGINE.md) — the class constructed with an `IENGINE` and that forwards `Log` to it.
- [ICONTEXT](ICONTEXT.md) / [IVIEWPORT](IVIEWPORT.md) — the per-session and per-viewport host interfaces.

---

[Engine API](index.md) · Prev: [ENGINE](ENGINE.md) · Next: [ICONTEXT](ICONTEXT.md)
