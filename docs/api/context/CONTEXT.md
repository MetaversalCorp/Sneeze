---
title: CONTEXT (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Context.h
  - include/Sneeze.h
  - include/Container.h
  - src/context/Context.cpp
verified: b487fd1
nav:
  prev: api/context/index.md
  next: api/container/index.md
---

# `CONTEXT`

One browsing session — the engine's equivalent of a browser tab. A `CONTEXT` owns the two per-session subsystems it needs to show a world — the [`SCENE`](../scene/index.md) and the [`VIEWPORT`](../viewport/index.md) — and pools the [`CONTAINER`](../container/index.md)s that give content its runtime identity. The console, network, and storage subsystems are **not** owned by the context; they are engine-wide singletons the context forwards to. For the conceptual picture — the ownership split, init/teardown order, container pooling, and the cache-reset key — see the [Context system](../../systems/context.md) page. This page is the exact behavior of every public member.

```cpp
class CONTEXT
{
public:
   enum eSESSION
   {
      kSESSION_PERSISTENT,
      kSESSION_TRANSITORY,
   };

   CONTEXT (ENGINE* pEngine, ICONTEXT* pHost, eSESSION kSession, bool bReset,
            const std::string& sPath_Permanent, const std::string& sPath_Temporary);
   ~CONTEXT ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

`CONTEXT` is non-copyable and non-movable (its copy/move constructors and assignment operators are deleted).

---

## Role and ownership

- **Owned by** the [`ENGINE`](../sneeze/index.md), which constructs it inside `Context_Open` and destroys it inside `Context_Close`.
- **Owns** one [`SCENE`](../scene/index.md) and one [`VIEWPORT`](../viewport/index.md), created in that order during `Initialize` and destroyed in reverse in the destructor.
- **Owns** a pool of `CONTAINER` objects, keyed by container identity ([`CID::Key_All()`](../container/CID.md)), in an `unordered_map`. The map is the authoritative owner of every container in the session.
- **Does not own** the console, network, or storage subsystems. `Console()`, `Network()`, `Storage()`, and `Wasm_Runtime()` forward to the engine-wide singletons; the context caches no copy of them.
- **Holds** a back-pointer to its `ENGINE` and to the host's `ICONTEXT`, the two on-disk paths, the session kind, the reset flag, and the primary container's reset key — all fixed at construction except the reset key, which is set when the primary opens.

---

## Lifecycle

A context is created, initialized, and destroyed — normally all through the engine, never by the application directly.

1. **Construct.** `ENGINE::Context_Open` builds the `CONTEXT` with the engine pointer, the host `ICONTEXT`, the session kind, the reset flag, and the permanent/temporary paths. The constructor only stores these; it builds no subsystems.
2. **Initialize.** `Initialize(sUrl)` builds the scene (then begins the asynchronous load of `sUrl`) and the viewport, in that order, aborting and reporting if either fails.
3. **Operate.** `Reset`, `Logout`, and `Clear` are the live-session hooks (see [Session operations](#session-operations)). There is no in-session navigation — a new address means a new context.
4. **Destruct.** `~CONTEXT()` deletes the viewport, then the scene (whose deletion cascades through fabrics, releasing their container references), then frees the container pool.

---

## Threading, locking, and pitfalls

This is the part to read before calling anything.

**The container pool is guarded by a recursive mutex.** `m_mxContainer` is a `std::recursive_mutex`, held by `Container_Open` and `Container_Close`. It is recursive because closing a container can re-enter the pool's locked paths on the same thread through the scene teardown cascade. The map lookup/insert/erase all run under it, so concurrent `Container_Open` calls (from different fetch completions) are serialized.

**The owned accessors are lifetime-stable.** `Scene()` and `Viewport()` return pointers that are valid and unchanging from the end of `Initialize` to the start of destruction — the context has no navigation that would swap them out. They are not lock-protected; the subsystems behind them carry their own internal synchronization.

**`Console()`, `Network()`, and `Storage()` forward to the engine.** They return the engine-wide singletons, not per-context objects. `Wasm_Runtime()` likewise forwards to `ENGINE::Wasm_Runtime`. Do not treat the returned pointers as context-private.

**`Container_Open` / `Container_Close` are engine-internal.** They are called by the scene during fabric loading and teardown, not by the application. `Container_Close` only decrements one reference — it does not remove the container from the pool; the pool is only emptied when the context is destroyed.

---

## Construction and destruction

```cpp
CONTEXT (ENGINE* pEngine, ICONTEXT* pHost, eSESSION kSession, bool bReset,
         const std::string& sPath_Permanent, const std::string& sPath_Temporary);
~CONTEXT ();
```

### `CONTEXT(pEngine, pHost, kSession, bReset, sPath_Permanent, sPath_Temporary)`
- **Purpose.** Construct a context bound to an engine and a host interface. Stores its inputs; builds no subsystems (call `Initialize`).
- **Parameters.**
- `pEngine` — the owning engine (required; must outlive the context).
- `pHost` — the host's `ICONTEXT` implementation, which receives inspector callbacks (container / network cache+file / storage silo+unit / console-entry created and deleted).
- `kSession` — `kSESSION_PERSISTENT` or `kSESSION_TRANSITORY`.
- `bReset` — request a cleared cache for this session; threaded into each `CONTAINER::Open` and stamped as a durable clear when the primary container opens.
- `sPath_Permanent` — the on-disk location for durable per-session data.
- `sPath_Temporary` — the on-disk location for scratch and cache.
- **Note.** Constructed by `ENGINE::Context_Open`; do not construct directly.

### `~CONTEXT()`
- **Purpose.** Destroy the context. Deletes the viewport, then the scene (whose deletion cascades through fabrics, releasing their container references), then any remaining pooled containers. The engine-owned console, network, and storage singletons are untouched.
- **Pitfalls.** Invoked by `ENGINE::Context_Close`. A pooled container still holding references after the scene cascade logs an error from the container's own destructor.

---

## Initialization

```cpp
bool Initialize (const std::string& sUrl);
```

### `bool Initialize (const std::string& sUrl)`
- **Purpose.** Build the session's owned subsystems and begin loading `sUrl`. Creates and initializes the `SCENE` (which starts the asynchronous fabric load at `sUrl`) and then the `VIEWPORT`, in that order.
- **Parameters.** `sUrl` — the start address (may be empty for an empty session).
- **Returns.** `true` if both the scene and viewport initialized; `false` (with a logged reason) if either failed.
- **Notes.** Call once, after construction. On failure the partially built context should be destroyed. The reset flag is not a parameter here — it is fixed at construction.

---

## Session operations

```cpp
void Reset  ();
void Logout ();
void Clear  ();
```

### `void Reset ()`
- **Purpose.** Durably record that this context's cache was cleared *now*. Forwards the context's reset key to [`NETWORK::Reset`](../network/NETWORK.md#cache-reset-durable-clear-the-cache), stamping the current time so every cached file the context relies on whose `createdAt` predates the stamp is refetched on next access. No files are deleted.
- **Returns.** Nothing.
- **Notes.** Does nothing if the context has no primary yet (the reset key is empty). Called automatically at primary open when the context was constructed with `bReset` set. See [Context system → The cache-reset key](../../systems/context.md#the-cache-reset-key).

### `void Logout ()`
- **Purpose.** Session logout hook.
- **Returns.** Nothing.
- **Notes.** Currently a **no-op**. Because `NETWORK` is an engine-wide singleton, a blanket cache clear here would wipe every other context's data, so it deliberately does nothing.

### `void Clear ()`
- **Purpose.** Reserved session-clear hook.
- **Returns.** Nothing.
- **Notes.** Not yet implemented — the body is a stub.

---

## Accessors

```cpp
ICONTEXT*           Host           () const;
ENGINE*             Engine         () const;
CONSOLE*            Console        () const;
NETWORK*            Network        () const;
STORAGE*            Storage        () const;
DEP::WASM_RUNTIME*  Wasm_Runtime   () const;
VIEWPORT*           Viewport       () const;
SCENE*              Scene          () const;
const std::string&  Path_Permanent () const;
const std::string&  Path_Temporary () const;
const std::string&  Key_Reset      () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `Host()` | The host's `ICONTEXT`. | Set at construction; receives the inspector callbacks. |
| `Engine()` | The owning engine. | Never null for a live context. |
| `Console()` | The engine's console singleton. | Forwarded from the engine; not context-owned. |
| `Network()` | The engine's network singleton. | Forwarded from the engine; not context-owned. |
| `Storage()` | The engine's storage singleton. | Forwarded from the engine; not context-owned. |
| `Wasm_Runtime()` | The engine's WASM runtime. | Forwarded from the engine; not context-owned. |
| `Viewport()` | The session's viewport. | Owned; valid and stable after `Initialize`. |
| `Scene()` | The session's scene. | Owned; valid and stable after `Initialize` (no navigation swaps it). |
| `Path_Permanent()` | The durable data path (by const reference). | Set at construction. |
| `Path_Temporary()` | The scratch/cache path (by const reference). | Set at construction. |
| `Key_Reset()` | The primary container's reset key (by const reference). | Empty until the first MSF-bearing container opens; see below. |

---

## Container management (internal)

Engine-internal. The scene calls these during fabric loading and teardown; an application does not.

```cpp
CONTAINER* Container_Open  (MSF* pMsf);
void       Container_Close (CONTAINER* pContainer);
```

### `CONTAINER* Container_Open (MSF* pMsf)`
- **Purpose.** Get (pooling if possible) the container for a source's verified MSF. Builds a [`CID`](../container/CID.md) from the MSF — fingerprint, organization, organization hash, container name, persona hash, and a trust level from the signature/chain checks — computes its `Key_All()`, and looks it up in the pool. If absent, constructs and inserts a new `CONTAINER`; if present, reuses it. Then calls `CONTAINER::Open(bReset)` to reference-count it.
- **Parameters.** `pMsf` — the parsed, verified MSF; `nullptr` builds the synthetic root container (trust `kTRUST_ROOT`, organization "Sneeze", name "Root", zero fingerprint).
- **Returns.** The opened `CONTAINER*`, or `nullptr` if `Open` failed (in which case the new entry is removed and the container deleted).
- **Side effect.** On the first successful open of an MSF-bearing container, records it as the primary — sets [`Key_Reset()`](#accessors) to its `CID::Key_All()`, and if the context was constructed with `bReset`, calls `Reset()` immediately.
- **Ownership.** The context owns the container (via the pool); the caller holds one open reference and must balance it with `Container_Close`.
- **Pitfalls.** Takes the recursive container mutex. Note the current trust override that forces `kTRUST_EXPIRED` (see [Container → Trust levels](../../systems/container.md#trust-levels)).

### `void Container_Close (CONTAINER* pContainer)`
- **Purpose.** Release one reference to a pooled container by calling `CONTAINER::Close()`.
- **Parameters.** `pContainer` — the container to release (must be non-null).
- **Returns.** Nothing.
- **Pitfalls.** Takes the recursive container mutex. Decrements the refcount only — it does **not** remove the container from the pool. Pooled containers are freed only when the context is destroyed.

---

## See also

- [Context system](../../systems/context.md) — design, init/teardown order, pooling, the cache-reset key.
- [Container API](../container/index.md) — `CONTAINER`, the `CID` identity record, and the per-container cache/silo/stream handles.
- [sneeze API](../sneeze/index.md) — `ENGINE::Context_Open` / `Context_Close`, and the `ICONTEXT` host interface.
- [Scene API](../scene/index.md) — the scene a context owns and loads at initialization.
- [Network API](../network/NETWORK.md) — `NETWORK::Reset`, the durable clear behind `CONTEXT::Reset`.

---

[Context API](index.md) · Next: [Container API](../container/index.md)
