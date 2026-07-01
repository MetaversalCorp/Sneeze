---
title: ICONTEXT (interface reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Sneeze.h
  - src/context/Container.cpp
  - src/sneeze/network/Network.cpp
  - src/sneeze/network/Cache.cpp
  - src/sneeze/network/File.cpp
  - src/sneeze/storage/Silo.cpp
  - src/sneeze/storage/Storage.cpp
  - src/sneeze/console/Console.cpp
verified: b487fd1
nav:
  prev: api/sneeze/IENGINE.md
  next: api/sneeze/IVIEWPORT.md
---

# `ICONTEXT`

The per-session **inspector interface**. A host application implements `ICONTEXT` and passes it to [`ENGINE::Context_Open`](ENGINE.md#context-management); the engine then calls its `On…` methods as things happen *inside that session* — containers opening and closing, network caches and files appearing and changing, storage silos mutating, console entries being added and evicted. It exists so a host can observe and surface a context's internal activity (a developer inspector, a network panel, a console view) without reaching into the engine's internals. For the bigger picture of what a context is, see the [Engine system](../../systems/engine.md) and [Context system](../../systems/context.md).

```cpp
class ICONTEXT
{
public:
   virtual ~ICONTEXT () = default;

   virtual void OnContainerCreated    (CONTAINER*) {}
   virtual void OnContainerDeleted    (CONTAINER*) {}

   virtual void OnNetworkCacheCreated (CACHE*) {}
   virtual void OnNetworkCacheDeleted (CACHE*) {}

   virtual bool OnNetworkFileCreated  (FILE*) { return true; }
   virtual void OnNetworkFileChanged  (FILE*) {}
   virtual void OnNetworkFileDeleted  (FILE*) {}

   virtual void OnStorageSiloCreated  (SILO*) {}
   virtual void OnStorageSiloDeleted  (SILO*) {}

   virtual void OnStorageUnitCreated  (SILO*, eSILO_SCOPE eScope) {}
   virtual void OnStorageUnitChanged  (SILO*, eSILO_SCOPE eScope, const std::string&) {}
   virtual void OnStorageUnitDeleted  (SILO*, eSILO_SCOPE eScope) {}

   virtual void OnConsoleEntryCreated (std::shared_ptr<const ENTRY>) {}
   virtual void OnConsoleEntryDeleted (std::shared_ptr<const ENTRY>) {}
};
```

---

## Role and ownership

- **Implemented by** the host application; passed to `ENGINE::Context_Open`.
- **Held by** the `CONTEXT` for its lifetime; reached by per-session subsystems as `m_pContext->Host()`. The host owns the object and **must keep it alive at least as long as the context** it was given to.
- **Direction is engine → host.** Every method is a callback the engine invokes; the host never calls them.
- **Every method is optional.** All have default implementations (no-ops, or `OnNetworkFileCreated` returning `true`), so a host overrides only the events it cares about. The interface is purely observational, with one exception: `OnNetworkFileCreated` can *veto* a file (see below).

---

## Threading and pitfalls

- **Callbacks arrive on the thread doing the work — often not the host's thread.** Network callbacks fire from fetch agents and the network layer; console, storage, and container callbacks fire from whichever thread performed the operation. A host implementation **must be thread-safe** and should not block, since several of these sit on hot paths.
- **Do not call back into the engine re-entrantly from a callback.** These fire while the originating subsystem holds its own locks (the storage mutex during a silo change, the console state during an entry add). Re-entering the same subsystem from inside the callback risks deadlock; copy out what you need and defer.
- **Pointer and shared-pointer lifetimes differ.** The `CONTAINER*`, `CACHE*`, `FILE*`, and `SILO*` arguments are borrowed — valid only for the duration of the call (and the matching `…Deleted` callback marks the end of validity). The console entries are `std::shared_ptr<const ENTRY>`, so a host may retain a reference and read the entry safely after the call returns.
- **`OnNetworkFileCreated` is the only callback whose return value matters** — returning `false` rejects the file (see its entry).

---

## Container callbacks

### `virtual void OnContainerCreated (CONTAINER*) {}`
- **Implemented by.** The host (optional).
- **Called by.** A `CONTAINER`, when its resources first open (its reference count rises from zero and its WASM store and linker are initialized).
- **Parameters.** The container that opened. It exposes its console stream and storage silo for inspection.
- **Contract.** Observational. The container pointer is valid until the matching `OnContainerDeleted`.

### `virtual void OnContainerDeleted (CONTAINER*) {}`
- **Implemented by.** The host (optional).
- **Called by.** A `CONTAINER`, when its last reference is released (its open count drops to zero) and just before its resources are torn down.
- **Parameters.** The container being deleted.
- **Contract.** Observational. Do not retain the pointer past this call.

---

## Network cache callbacks

A [`CACHE`](../network/index.md) is the per-container handle onto the engine-owned network. These fire as a container opens and closes its cache, so an inspector can group file activity by container.

### `virtual void OnNetworkCacheCreated (CACHE*) {}`
- **Implemented by.** The host (optional).
- **Called by.** `NETWORK`, when a container opens its cache (`Cache_Open`).
- **Parameters.** The new cache. It exposes its container's display name and can be enumerated for files.
- **Contract.** Observational. Valid until the matching `OnNetworkCacheDeleted`.

### `virtual void OnNetworkCacheDeleted (CACHE*) {}`
- **Implemented by.** The host (optional).
- **Called by.** `NETWORK`, when a container closes its cache (`Cache_Close`).
- **Parameters.** The cache being deleted.
- **Contract.** Observational. Do not retain the pointer past this call.

---

## Network file callbacks

### `virtual bool OnNetworkFileCreated (FILE*) { return true; }`
- **Implemented by.** The host (optional).
- **Called by.** The network layer when a [`FILE`](../network/index.md) is created for a request.
- **Parameters.** The newly created file.
- **Contract.** **Return `true` to accept the file, `false` to reject it** — the network layer interprets a `false` return as a signal to clear the file. The default accepts. This is the one inspector callback that can influence engine behavior rather than merely observe it.
- **Returns.** Whether the file may proceed.

### `virtual void OnNetworkFileChanged (FILE*) {}`
- **Implemented by.** The host (optional).
- **Called by.** The network layer when a file's state changes (for example its fetch progresses or completes).
- **Parameters.** The file whose state changed.
- **Contract.** Observational; fired from the network/fetch path.

### `virtual void OnNetworkFileDeleted (FILE*) {}`
- **Implemented by.** The host (optional).
- **Called by.** The network layer when a file is cleared or removed.
- **Parameters.** The file being deleted.
- **Contract.** Observational. Do not retain the pointer past this call.

---

## Storage callbacks

Two tiers, mirroring the network `Cache` (handle) and `File` (leaf) callbacks. The silo tier reports handle lifetime; the unit tier reports the underlying JSON documents — which are shared engine-wide by pathname, so unit changes fan out to every silo holding that unit.

### `virtual void OnStorageSiloCreated (SILO*) {}`
- **Implemented by.** The host (optional).
- **Called by.** `STORAGE`, when a [`SILO`](../storage/index.md) is created and initialized.
- **Parameters.** The new silo.
- **Contract.** Observational. The silo carries the identity (display name / CID) of the source it belongs to.

### `virtual void OnStorageSiloDeleted (SILO*) {}`
- **Implemented by.** The host (optional).
- **Called by.** `STORAGE`, when a silo is removed.
- **Parameters.** The silo being deleted.
- **Contract.** Observational. Do not retain the pointer past this call.

### `virtual void OnStorageUnitCreated (SILO*, eSILO_SCOPE eScope) {}`
- **Implemented by.** The host (optional).
- **Called by.** A `UNIT`, when a silo first attaches it (`UNIT::Open`).
- **Parameters.** The silo whose view of the unit appeared; `eScope` — which of the four scopes.
- **Contract.** Observational. Fired for the attaching silo only.

### `virtual void OnStorageUnitChanged (SILO*, eSILO_SCOPE eScope, const std::string&) {}`
- **Implemented by.** The host (optional).
- **Called by.** A `UNIT`, on every mutation — `Set`, `Remove`, and bulk JSON replacement.
- **Parameters.** A silo holding the changed unit; `eScope` — which scope; the key path that changed (empty for a bulk replacement).
- **Contract.** Observational; fired once **per holding silo** so every context sharing the unit is notified. Fired while the unit holds its lock — do not re-enter storage from here.

### `virtual void OnStorageUnitDeleted (SILO*, eSILO_SCOPE eScope) {}`
- **Implemented by.** The host (optional).
- **Called by.** A `UNIT`, when a silo detaches it (`UNIT::Close`).
- **Parameters.** The silo whose view of the unit is going away; `eScope` — which scope.
- **Contract.** Observational. Fired for the detaching silo only.

---

## Console entry callbacks

### `virtual void OnConsoleEntryCreated (std::shared_ptr<const ENTRY>) {}`
- **Implemented by.** The host (optional).
- **Called by.** `CONSOLE`, when a new [`ENTRY`](../console/index.md) is appended.
- **Parameters.** A shared pointer to the immutable entry.
- **Contract.** Observational. Because the argument is a `shared_ptr<const ENTRY>`, the host **may retain it** and read the entry after the call.

### `virtual void OnConsoleEntryDeleted (std::shared_ptr<const ENTRY>) {}`
- **Implemented by.** The host (optional).
- **Called by.** `CONSOLE`, when an entry leaves the buffer — both on eviction as the ring buffer overflows its cache limit and on an explicit `Clear` (fired once per evicted entry).
- **Parameters.** A shared pointer to the entry being removed.
- **Contract.** Observational. The host's retained reference (if any) remains valid as long as the host holds the shared pointer; the engine is simply dropping its own reference.

---

## See also

- [Engine system](../../systems/engine.md) — where contexts are opened with an `ICONTEXT`.
- [ENGINE](ENGINE.md) — `Context_Open` takes the `ICONTEXT` implementation.
- [Container API](../container/index.md) · [Network API](../network/index.md) · [Storage API](../storage/index.md) · [Console API](../console/index.md) — the subsystems that fire these callbacks.
- [IVIEWPORT](IVIEWPORT.md) — the per-viewport rendering interface.

---

[Engine API](index.md) · Prev: [IENGINE](IENGINE.md) · Next: [IVIEWPORT](IVIEWPORT.md)
