---
title: CONTAINER (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Container.h
  - src/context/Container.cpp
verified: b487fd1
nav:
  prev: api/container/index.md
  next: api/container/CID.md
---

# `CONTAINER`

The runtime manifestation of one signed content source: its identity ([`CID`](CID.md)), its WebAssembly sandbox, and the per-source resources it is confined to — a network [`CACHE`](../network/index.md), a console [`STREAM`](../console/index.md), and a storage [`SILO`](../storage/index.md) — plus the scene nodes of the fabrics bound to it. A container is reference-counted and pooled by the [`CONTEXT`](../context/index.md), so every [`FABRIC`](../scene/FABRIC.md) from the same source under the same identity shares one. For the conceptual picture see the [Container system](../../systems/container.md) page; this page is the exact behavior of every public member.

```cpp
class CONTAINER
{
public:
   class CID { /* ... see CID.md */ };

   CONTAINER (CONTEXT* pContext, const CID* pCID);
   ~CONTAINER ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

`CONTAINER` is non-copyable and non-movable (its copy/move constructors and assignment operators are deleted).

---

## Role and ownership

- **Created and owned by** the [`CONTEXT`](../context/index.md), via `Container_Open`, which pools containers by [`CID::Key_All()`](CID.md). Never construct a container directly — the context dedupes and reference-counts them.
- **Owns**, while open, a network `CACHE`, a console `STREAM`, a storage `SILO` (attached), and a WASM store (the sandbox) plus the WASM instances opened in it.
- **Owns** the scene node handle table — the `NODE` and `MAP_OBJECT` objects of every fabric bound to it — for the container's whole lifetime, deleting any survivors on destruction.
- **Holds** a copy of its `CID` identity record and its pooling key, four derived identity paths, an optional stale-time floor, and a back-pointer to its `CONTEXT`.
- **Reaches** the network, console, storage, WASM runtime, and host through its context; it caches no other service pointers.

---

## Lifecycle

A container's resources are tied to its reference count, not to construction:

1. **Construct.** `CONTAINER(pContext, pCID)` copies the identity record and computes the pooling key and the four identity paths. It allocates no cache, stream, silo, or store yet.
2. **Open.** `Open(bReset)` increments the reference count. On the transition from 0 to 1 it creates the identity folders on disk, then stands up the cache, the stream, the silo (and attaches it), and the WASM store (setting its host data and initializing its linker), then notifies the host via `ICONTEXT::OnContainerCreated`. Later opens just bump the count.
3. **Close.** `Close()` decrements the count. On the transition from 1 to 0 it notifies the host (`OnContainerDeleted`) and tears down the store, silo, stream, and cache in reverse.
4. **Destruct.** `~CONTAINER()` deletes any surviving `MAP_OBJECT`s and frees the implementation. A container deleted with a non-zero reference count logs an error.

---

## Threading, locking, and pitfalls

**A recursive mutex guards the reference count, resources, and node table.** `m_mxContainer` is a `std::recursive_mutex`, held by `Open`, `Close`, and the `Node_*` methods. It is recursive because a failed `Open` calls `Close` while still holding the lock. Concurrent opens and closes of the same container — which arrive from different network fetch completions and from the teardown cascade — are serialized, and the "first open" / "last close" resource transitions are atomic with respect to each other.

**`Instance_Open` / `Instance_Close` are not guarded by the container mutex.** They forward directly to the WASM store, which owns the synchronization appropriate to module instantiation. Do not assume they are serialized against `Open`/`Close`.

**`Cache()`, `Stream()`, and `Silo()` are null while closed.** Those resources exist only between the first open and the last close. Reading them on a container whose count is zero returns `nullptr`.

**Close only decrements; the pool keeps the container.** `Close()` does not remove the container from the context's pool, and the destructor does not run until the context frees the pool. A closed container lingers, re-openable, for the life of the session.

**Do not delete a container yourself.** The context owns it. Deleting it while references remain logs an error and skips orderly resource teardown.

---

## Construction and destruction

```cpp
CONTAINER (CONTEXT* pContext, const CID* pCID);
~CONTAINER ();
```

### `CONTAINER(pContext, pCID)`
- **Purpose.** Construct a container bound to a context and stamped with an identity. Copies `*pCID` and computes the pooling key and the four identity paths from it. Allocates no resources.
- **Parameters.** `pContext` — the owning context (required; must outlive the container); `pCID` — the identity record to copy (the container keeps its own copy).
- **Note.** Created by `CONTEXT::Container_Open`; do not construct directly. Call `Open` next.

### `~CONTAINER()`
- **Purpose.** Delete any `MAP_OBJECT`s still held and free the implementation.
- **Pitfalls.** If the reference count is still above zero, logs an error naming the count and the source's display name. Reach zero references (via the scene teardown cascade) before the container is deleted.

---

## Lifecycle methods

```cpp
bool        Open        (bool bReset);
size_t      Close       ();
std::string Reset_Stale () const;
```

### `bool Open (bool bReset)`
- **Purpose.** Acquire a reference. On the first acquisition (count 0 → 1), create the permanent and temporary identity folders, open the network cache, open the console stream, open and attach the storage silo, open the WASM store (set its host data to this container and initialize its linker), and notify the host (`OnContainerCreated`).
- **Parameters.** `bReset` — the reload-with-reset flag. On the **root container** (`kTRUST_ROOT`) a true value stamps the in-memory stale-time floor to the current wall-clock time (reported by `Reset_Stale`); on any other container it has no effect here (the durable per-primary clear is recorded by the context, not the container).
- **Returns.** `true` if the container is open (either freshly stood up or already open); `false` if first-open resource creation failed — in which case it self-closes to unwind the partial state.
- **Pitfalls.** Takes the recursive mutex. The context calls this once per fabric that binds the container; balance each call with `Close`.

### `size_t Close ()`
- **Purpose.** Release a reference. On the last release (count 1 → 0), notify the host (`OnContainerDeleted`) and tear down the WASM store, silo (detach then close), stream, and cache in reverse order.
- **Returns.** The reference count remaining after the decrement (0 means the resources were torn down).
- **Pitfalls.** Takes the recursive mutex. Does not remove the container from the context's pool.

### `std::string Reset_Stale () const`
- **Purpose.** Report this container's contribution to cache-staleness resolution. The network cache consults it before falling back to the network's per-key reset record.
- **Returns.** For the **root container**, the stamped stale-time floor if one has been set (see `Open`), otherwise the network's start time as a baseline. For **any other container**, an empty string — meaning "no container-level clear; defer to the network's per-primary reset record."
- **Notes.** See [Network → Clearing the cache](../../systems/network.md#clearing-the-cache) for how this composes with the durable, per-primary record.

---

## WASM instance methods

```cpp
bool Instance_Open  (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const std::vector<uint8_t>& aWasmBytes);
void Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash);
```

Called by a [`FABRIC`](../scene/FABRIC.md) as it loads and unloads the WASM modules its MSF declares. Instances live in the container's WASM store and are keyed by the triple `(fabric index, module URL, module hash)` — the fabric index namespaces a fabric's instances so several fabrics can share one container's store.

### `bool Instance_Open (twFabricIx, sUrl, sHash, aWasmBytes)`
- **Purpose.** Compile and instantiate a WASM module's bytes into the container's store under the given key.
- **Parameters.** `twFabricIx` — the owning fabric's scene-global index; `sUrl` — the module's address; `sHash` — its integrity hash; `aWasmBytes` — the module bytes.
- **Returns.** `true` if the module instantiated; `false` otherwise.
- **Pitfalls.** Forwards to the WASM store without taking the container mutex. Requires the container to be open (the store exists only while open).

### `void Instance_Close (twFabricIx, sUrl, sHash)`
- **Purpose.** Unload the instance identified by the key.
- **Parameters.** As above (the key triple).
- **Returns.** Nothing.

---

## Scene node methods

```cpp
uint64_t Node_Root  (uint64_t twFabricIx, const RMCOBJECT* pRMCObject);
uint64_t Node_Open  (uint64_t twParentIx, const RMCOBJECT* pRMCObject);
bool     Node_Close (uint64_t twObjectIx);
NODE*    Node_Find  (uint64_t twObjectIx) const;
```

The container owns the scene node table for every fabric bound to it. These methods build and tear it down. They are driven from the WASM host-function bridge — content code in the sandbox constructs its scene graph by calling them, one `RMCOBJECT` per node — and also from the host-side "map-managed" path, in which the browser reads a fabric's node tree from its MSF and injects nodes through the same calls. Node object indices are allocated **per container**, so one container can hold the nodes of several fabrics in a shared index space. All four take the recursive mutex.

### `uint64_t Node_Root (twFabricIx, pRMCObject)`
- **Purpose.** Create the root node of the fabric identified by `twFabricIx`. Allocates the concrete `MAP_OBJECT` subclass for the object's class, wraps it in a `NODE`, and records both in the container's tables.
- **Parameters.** `twFabricIx` — the target fabric's scene-global index; `pRMCObject` — the node description.
- **Returns.** The new composed object index, or `OBJECTIX_ERROR` if the fabric was not found, already has a root, or the object could not be built.

### `uint64_t Node_Open (twParentIx, pRMCObject)`
- **Purpose.** Create a child node under an existing parent, inheriting the parent's fabric.
- **Parameters.** `twParentIx` — the parent node's object index; `pRMCObject` — the node description (its own index is allocated if it is the identity sentinel, or honored if it is free).
- **Returns.** The new composed object index, or `OBJECTIX_ERROR` on failure.

### `bool Node_Close (twObjectIx)`
- **Purpose.** Remove and delete the node identified by `twObjectIx` and its `MAP_OBJECT`.
- **Returns.** `true` if the node existed and was removed; `false` otherwise.

### `NODE* Node_Find (twObjectIx) const`
- **Purpose.** Look up a node by its composed object index.
- **Returns.** The `NODE*`, or `nullptr` if no node holds that index.

---

## Accessors

```cpp
CONTEXT*           Context  () const;
const CID*         Identity () const;
const std::string& Key      () const;
CACHE*             Cache    () const;
SILO*              Silo     () const;
STREAM*            Stream   () const;

const std::string& Path_Permanent_Org () const;
const std::string& Path_Temporary_Org () const;
const std::string& Path_Permanent_All () const;
const std::string& Path_Temporary_All () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `Context()` | The owning context. | Never null for a live container. |
| `Identity()` | The container's `CID` (by pointer to the internal copy). | Stable for the container's lifetime. |
| `Key()` | The pooling key (by const reference). | The `CID::Key_All()` string the context's pool is keyed by. |
| `Cache()` | The network cache, or null. | Null while the container is closed (count at zero). |
| `Silo()` | The storage silo, or null. | Null while the container is closed. |
| `Stream()` | The console stream, or null. | Null while the container is closed. |
| `Path_Permanent_All()` | The container's folder under the permanent root: `<perm>/persona/fp2/fp22/container`. | The identity scaffold subsystems build on; cached at construction. Created at `Open()`. |
| `Path_Temporary_All()` | The same under the temporary root. | Created at `Open()`. |
| `Path_Permanent_Org()` | The organization-tier folder under the permanent root: `<perm>/persona/fp2/fp22` (no container segment). | Where org-scoped storage is filed, shared across containers of the same identity. |
| `Path_Temporary_Org()` | The same under the temporary root. | |

---

## See also

- [Container system](../../systems/container.md) — design, identity, trust, lifecycle.
- [CID](CID.md) — the identity record this container is stamped with.
- [Context API](../context/index.md) — creates, pools, opens, and closes containers.
- [Network API](../network/index.md) — the cache a container opens and reaches through.
- [Scene API](../scene/FABRIC.md) — fabrics bind to a container and open instances and nodes in it.

---

[Container API](index.md) · Next: [CID](CID.md)
