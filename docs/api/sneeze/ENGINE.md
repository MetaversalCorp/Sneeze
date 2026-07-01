---
title: ENGINE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Sneeze.h
  - src/sneeze/Engine.cpp
verified: b487fd1
nav:
  prev: api/sneeze/index.md
  next: api/sneeze/IENGINE.md
---

# `ENGINE`

The single object a host application constructs to use the engine. One `ENGINE` owns every engine-wide subsystem (identity, WASM runtime, shader pipeline, XR, UI, HTTP, the engine thread and its agent pools, and the three engine-owned service singletons — console, network, and storage) and the set of open [`CONTEXT`](../context/index.md)s. It is the embedding boundary and the root of the engine's owner chain. For the conceptual picture — bring-up order, shutdown symmetry, the cache layout — see the [Engine system](../../systems/engine.md) page; this page is the exact behavior of every public member.

```cpp
class ENGINE
{
public:
   static constexpr const char* sFOLDER_PERSISTENT = "Persistent";
   static constexpr const char* sFOLDER_TRANSITORY = "Transitory";

   explicit ENGINE (IENGINE* pHost);
   ~ENGINE ();
   // non-copyable, non-movable
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Constructed by** the host application, with a back-pointer to its [`IENGINE`](IENGINE.md) implementation. The host owns the `ENGINE`; the `ENGINE` does not own the host.
- **Owns** (in creation order) the `PERSONA`, the WASM runtime, the SPIR-V pipeline, the XR runtime, the UI context, the global curl state, the `CONTROL` object (engine thread + agent pools), the cache paths, and then the three engine-owned service singletons created on top of them — the `CONSOLE`, the `NETWORK`, and the `STORAGE`. These three moved up from the per-context layer so one deduplicated cache, log, and document store serve every context.
- **Owns** the list of open `CONTEXT`s, guarded by an internal mutex.
- **Is the root** of the owner chain `NODE → FABRIC → SCENE → CONTEXT → ENGINE`; deeper objects reach engine services by walking up to here, never by caching a pointer.
- **Non-copyable and non-movable** — copy/move constructors and assignment are deleted.

Uses the pimpl idiom: all state lives in a private `ENGINE::Impl`.

---

## Lifecycle

An engine is used in three phases:

1. **Construct.** `ENGINE(pHost)` allocates the implementation and stores the host pointer. It does *not* bring any subsystem up — construction cannot fail.
2. **Initialize.** `Initialize()` runs the nested success cascade that creates and starts every subsystem, reading configuration from the host. It returns `true` only if every step succeeded. Call it exactly once.
3. **Destruct.** `~ENGINE()` tears everything down in the exact reverse order: it closes all remaining contexts, scrubs the session's transitory folder, then destroys the `STORAGE`, `NETWORK`, and `CONSOLE` singletons, then `CONTROL`, the HTTP stack, the UI/XR/SPIR-V/WASM subsystems, and finally the persona. The network is deliberately torn down before `CONTROL`, whose fetch agents must still be alive so `~NETWORK` can drain any in-flight assets.

There is no `Shutdown()` method — teardown is destruction. There is no restart: a failed `Initialize` leaves the engine down, and the only correct response is to destroy it.

---

## Threading and pitfalls

- **`Initialize` spawns the engine's threads.** It runs on the caller's thread, but constructing `CONTROL` starts the engine thread and all agent threads, which then run for the engine's lifetime. After `Initialize` returns true, the engine is multi-threaded; treat subsequent calls as concurrent with agent work.
- **The context list is mutex-guarded, and `Context_Close` deletes under that lock.** Closing a context destroys it while the internal context mutex is held. Context destruction is heavy (it cascades through scene/network/container teardown), so the lock is held for the whole teardown; other threads touching the context list block behind it.
- **`Initialize` must be called once.** It is neither re-entrant nor restartable; there is no partial-retry path.
- **`Log` is the one method safe to call from anywhere at any time** — it simply forwards to the host. Everything else assumes a constructed (and, except for the accessors, initialized) engine.
- **The renderer name is validated lazily.** `Initialize` does not check the host's `sRenderer()`; a bad value surfaces when a viewport activates, not at engine bring-up.

---

## Construction and destruction

```cpp
explicit ENGINE (IENGINE* pHost);
~ENGINE ();
```

### `ENGINE(pHost)`
- **Purpose.** Construct an engine bound to the host interface. Does not start any subsystem — call `Initialize` next.
- **Parameters.** `pHost` — the host's [`IENGINE`](IENGINE.md) implementation. Must outlive the engine; the engine reads configuration from it and logs through it.
- **Notes.** Construction cannot fail.

### `~ENGINE()`
- **Purpose.** Tear the engine down. If it was initialized, closes every open context and scrubs the session transitory folder, then destroys all subsystems in reverse creation order and logs `"Shutdown complete"`.
- **Pitfalls.** Joins the engine thread and all agents. Do not destroy the engine while another thread is still calling into it.

---

## Initialization

```cpp
bool Initialize ();
IENGINE* Host () const;
```

### `bool Initialize ()`
- **Purpose.** Bring the engine up. Reads configuration from the host, then creates and initializes — only if each prior step succeeds — the persona, WASM runtime, SPIR-V pipeline, XR runtime, UI context, global curl state, `CONTROL` (engine thread + agents), the cache directory layout (creating the persistent and transitory roots, scrubbing orphaned transitory folders, and making this run's session folder), and finally the three service singletons in order: the `CONSOLE`, the `NETWORK` (pointed at the engine cache root, where `network_reset.json` lives), and the `STORAGE`.
- **Parameters.** None — all configuration comes from the host.
- **Returns.** `true` if every subsystem initialized; `false` if any failed (the failing step is logged). Notably returns `false` if the host's `sAppDataPath()` is empty.
- **Notes.** Call once. On failure, destroy the engine rather than retrying.

### `IENGINE* Host () const`
- **Purpose / Returns.** The host interface the engine was constructed with.

---

## Context management

```cpp
CONTEXT* Context_Open  (ICONTEXT* pHost, const std::string& sUrl = "",
                        CONTEXT::eSESSION kSession = CONTEXT::kSESSION_PERSISTENT,
                        bool bReset = false);
bool     Context_Close (CONTEXT* pContext);
```

### `CONTEXT* Context_Open (ICONTEXT* pHost, const std::string& sUrl, CONTEXT::eSESSION kSession, bool bReset)`
- **Purpose.** Open a browsing session. Creates a per-context transitory folder, selects the context's permanent path from the session kind, constructs the `CONTEXT`, adds it to the engine's context list *before* initializing it, then calls `CONTEXT::Initialize(sUrl)`.
- **Parameters.**
- `pHost` — the context's [`ICONTEXT`](ICONTEXT.md) inspector interface (must outlive the context).
- `sUrl` — the initial address to navigate to; may be empty to open an empty session.
- `kSession` — `kSESSION_PERSISTENT` (data cached under the shared persistent folder, kept across runs) or `kSESSION_TRANSITORY` (data under this run's session folder, scrubbed at shutdown). Defaults to persistent.
- `bReset` — when `true`, the context stamps a durable cache-clear against its primary fabric's container key as it comes up, so the session refetches everything (the "clear cache and reload" entry point). Defaults to `false`. See [`NETWORK::Reset`](../network/NETWORK.md#cache-reset-durable-clear-the-cache).
- **Returns.** The new `CONTEXT*`, or `nullptr` if initialization failed (in which case the context is removed, deleted, and its temporary folder scrubbed — no trace is left).
- **Ownership.** On success the engine owns the context; close it with `Context_Close`.

### `bool Context_Close (CONTEXT* pContext)`
- **Purpose.** Close and destroy a context, then queue its temporary folder for scrubbing.
- **Parameters.** `pContext` — the context to close. Must be non-null: the public method rejects a null pointer and returns `false`.
- **Returns.** `true` if a context was closed; `false` otherwise.
- **Pitfalls.** Destroys the context while holding the internal context-list mutex — see [Threading and pitfalls](#threading-and-pitfalls). The engine's own destructor drains all contexts via the internal null-means-most-recent path; that behavior is not exposed through this public method.

---

## Persona

```cpp
void Login         (const std::string& sFirst, const std::string& sSecond);
void Logout        ();
void ChangePersona (const std::string& sFirst, const std::string& sSecond);
```

### `void Login (const std::string& sFirst, const std::string& sSecond)`
- **Purpose.** Log the shared identity proxy in with the given two-part name.
- **Parameters.** `sFirst`, `sSecond` — the identity name parts.

### `void Logout ()`
- **Purpose.** Log out. Walks every open context, logging each out, then logs the persona out. Emits trace logs marking the teardown phases.

### `void ChangePersona (const std::string& sFirst, const std::string& sSecond)`
- **Purpose.** Switch identity — a `Logout` immediately followed by a `Login`.
- **Parameters.** As `Login`.

---

## Paths

```cpp
const std::string& Path_Persistent () const;
const std::string& Path_Session    () const;
```

### `const std::string& Path_Persistent () const`
- **Purpose / Returns.** The persistent cache directory (`<sAppDataPath>/Sneeze/Cache/Persistent`), by const reference. Populated by `Initialize`.

### `const std::string& Path_Session () const`
- **Purpose / Returns.** This run's transitory session directory (`…/Transitory/s<8 hex>`), by const reference. Populated by `Initialize`; scrubbed at shutdown.

> Both are returned by reference into the engine. Do not retain them past the engine's lifetime.

---

## Subsystems

```cpp
persona::PERSONA*  Persona      () const;
DEP::WASM_RUNTIME* Wasm_Runtime () const;
DEP::UI_CONTEXT*   Ui_Context   () const;
NETWORK*           Network      () const;
STORAGE*           Storage      () const;
CONSOLE*           Console      () const;
```

### `persona::PERSONA* Persona () const`
- **Purpose / Returns.** The shared identity proxy. See [Persona](../persona/index.md).

### `DEP::WASM_RUNTIME* Wasm_Runtime () const`
- **Purpose / Returns.** The engine-wide WebAssembly runtime that hosts every container's sandbox.

### `DEP::UI_CONTEXT* Ui_Context () const`
- **Purpose / Returns.** The shared RmlUi manager: the one-time global RmlUi lifecycle, system interface, fonts, and the single render interface every in-scene panel draws through. See [UI](../../systems/ui.md).

### `NETWORK* Network () const`
- **Purpose / Returns.** The engine-owned resource loader and disk cache. Containers open a per-container [`CACHE`](../network/index.md) onto it; a context forwards `CONTEXT::Network()` here. See [Network](../network/index.md).

### `STORAGE* Storage () const`
- **Purpose / Returns.** The engine-owned persistent JSON document store. Containers open a per-container [`SILO`](../storage/index.md) onto it. See [Storage](../storage/index.md).

### `CONSOLE* Console () const`
- **Purpose / Returns.** The engine-owned developer console. Containers open a per-container [`STREAM`](../console/index.md) onto it. See [Console](../console/index.md).

---

## Logging and job submission

```cpp
void Log                   (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage);
void Queue_Post_Fetch      (JOB_FETCH* pJob_Fetch);
void Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor);
```

### `void Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage)`
- **Purpose.** Emit a log line. Forwards to the host's `IENGINE::Log` so all engine output reaches the host through one path.
- **Parameters.** `Level` — severity (`kLOGLEVEL_Trace` … `kLOGLEVEL_Error`); `sModule` — a short source tag; `sMessage` — the text.
- **Notes.** Safe to call from any thread at any time; a no-op if there is no host.

### `void Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)`
- **Purpose.** Submit a network fetch job to the fetch agent pool. Delegates to `CONTROL`.
- **Parameters.** `pJob_Fetch` — a heap-allocated, self-cleaning fetch job.
- **Notes.** Exists so per-context subsystems can post work through the owner chain. See [Control system](../../systems/control.md) for the job lifecycle.

### `void Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor)`
- **Purpose.** Submit a perpetual compositor job to the render pool. Delegates to `CONTROL`.
- **Parameters.** `pJob_Compositor` — the viewport's render job. Unlike fetch jobs, its lifetime is owned by the viewport, not self-cleaned on completion.

---

## Constants

| Constant | Value | Meaning |
|---|---|---|
| `sFOLDER_PERSISTENT` | `"Persistent"` | Name of the persistent cache subfolder. |
| `sFOLDER_TRANSITORY` | `"Transitory"` | Name of the transitory cache subfolder; also the literal a scrub path is validated against. |

---

## See also

- [Engine system](../../systems/engine.md) — bring-up/shutdown, contexts, path management.
- [IENGINE](IENGINE.md) — the host interface this engine reads and logs through.
- [ICONTEXT](ICONTEXT.md) / [IVIEWPORT](IVIEWPORT.md) — the per-session and per-viewport host interfaces.
- [Control system](../../systems/control.md) — the engine thread and agents behind `Queue_Post_*`.
- [Context API](../context/index.md) — the session object opened by `Context_Open`.

---

[Engine API](index.md) · Next: [IENGINE](IENGINE.md)
