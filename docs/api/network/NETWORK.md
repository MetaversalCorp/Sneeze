---
title: NETWORK (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Network.h
  - src/sneeze/network/Network.cpp
verified: b487fd1
nav:
  prev: api/network/index.md
  next: api/network/CACHE.md
---

# `NETWORK`

The resource subsystem's engine-owned core. Exactly one `NETWORK` exists per [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it. It owns the deduplicated on-disk cache (one private `ASSET` per cached URL, shared across every context), the background fetch machinery, and the durable "cache was cleared" record. It does not, itself, open files — a caller opens files against a per-container [`CACHE`](CACHE.md), which `NETWORK` hands out through `Cache_Open` and reclaims through `Cache_Close`. For the conceptual picture — the three-tier NETWORK/CACHE/FILE split, the two-counter asset lifecycle, the multi-origin cache-reset design, and the fetch dispatch path — see the [Network system](../../systems/network.md). This page is the exact behavior of every public member.

```cpp
class NETWORK
{
public:
   explicit NETWORK (ENGINE* pEngine);
   ~NETWORK ();

   bool Initialize (const std::string& sPath_Root);

   CACHE* Cache_Open  (CONTAINER* pContainer);
   void   Cache_Close (CONTAINER* pContainer, CACHE* pCache);
   void   Cache_Enum  (IENUM_CACHE* pEnum);

   void        Reset      (const std::string& sKey);
   std::string Time_Start () const;

private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Owned by** the [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it. One network serves every [`CONTEXT`](../context/index.md) in the engine.
- **Owns** the deduplicated asset store (one private `ASSET` per active URL, keyed by disk pathname), the registry of open per-container caches, the monotonic asset-index counter, and the reset record (`network_reset.json`).
- **Hands out** a [`CACHE`](CACHE.md) per [`CONTAINER`](../container/index.md); the cache owns that container's [`FILE`](FILE.md) handles and forwards asset operations back to the network.
- **Reaches** the engine (logging, fetch-queue dispatch) directly through its `ENGINE*`, and the host (cache/file notifications) indirectly through each container's context — it caches no context or host pointer of its own.

> **There is no `NETWORK::File_Open`.** Files are opened on a [`CACHE`](CACHE.md) (`CACHE::File_Open`), not on the network. The network's public surface manages caches and the durable reset record; the per-container cache is the file-opening seam.

---

## Threading, locking, and pitfalls

A network is touched from container open/close paths, from the host's inspector thread, and from FETCH agent threads delivering completions. It carries **three independent recursive mutexes** rather than one coarse lock, because they guard unrelated state that is never needed together:

- `m_mxNetwork_Reset` guards the reset map, the global stale floor, the asset-index counter, and `network_reset.json`. It is `mutable` (the const staleness lookup locks it) and recursive (reset and index paths call the save path).
- `m_mxNetwork_Cache` guards the cache registry. Recursive, because the destructor holds it across `Cache_Close`.
- `m_mxNetwork_Asset` guards the asset map.

**The cross-lock order is registry → cache → asset map → asset, with the reset lock taken last and never co-held with an asset lock.** Staleness is resolved *before* an asset lock is taken (see [Network system → Threading](../../systems/network.md#threading-model)), which is what keeps the reset lock and the asset lock from ever nesting.

**Returned `CACHE*` and `FILE*` pointers are owned by the engine, not the caller.** Do not delete them. A cache is returned via `Cache_Close`; a file via [`FILE::Close`](FILE.md#close).

**Cache and file notifications run on the caller's or a fetch agent's thread.** `OnNetworkCache*` fires inline on the open/close path; `OnNetworkFile*` may run from a fetch agent after the originating call has returned.

**Shutdown can block.** The destructor deletes any leaked caches directly (their owning contexts are already gone, so no callback is routed) and then busy-waits until the asset map drains — a documented race workaround. Destroying the engine while fetches are outstanding pauses until they complete.

---

## Construction and lifecycle

```cpp
explicit NETWORK (ENGINE* pEngine);
~NETWORK ();
bool Initialize (const std::string& sPath_Root);
```

### `NETWORK (ENGINE* pEngine)`
- **Purpose.** Construct an empty network owned by `pEngine`. Touches no disk and reads no state — call `Initialize`.
- **Parameters.** `pEngine` — the owning engine; must outlive the network.

### `~NETWORK ()`
- **Purpose.** Tear down the network. Persists the asset-index counter (writing back the exact next index so a clean shutdown wastes no reserve), saves `network_reset.json`, deletes any caches still registered (a leak safety net — normally every container has already closed its cache), then waits for the asset map to drain before returning.
- **Pitfalls.** Can block while in-flight fetches complete — see [Threading and pitfalls](#threading-locking-and-pitfalls).

### `bool Initialize (const std::string& sPath_Root)`
- **Purpose.** Prepare the network: record the engine session start time, locate `network_reset.json` directly under `sPath_Root`, and load it (or start fresh if it is missing, unparseable, or fails validation). Logs the reset path, the number of stored resets, and the restored asset-index counter.
- **Parameters.** `sPath_Root` — the engine-wide cache root; `network_reset.json` lives directly beneath it.
- **Returns.** `true` on success.
- **Notes.** Call once, after construction. A failed load is not fatal: it sets the global stale floor to "now," implicitly staling every asset created in a prior session (see [Network system → Clearing the cache](../../systems/network.md#clearing-the-cache)).

---

## Cache registry

```cpp
CACHE* Cache_Open  (CONTAINER* pContainer);
void   Cache_Close (CONTAINER* pContainer, CACHE* pCache);
void   Cache_Enum  (IENUM_CACHE* pEnum);
```

### `CACHE* Cache_Open (CONTAINER* pContainer)`
- **Purpose.** Create a [`CACHE`](CACHE.md) for `pContainer`, register it, initialize it, and announce it. The cache is added to the registry *before* `Initialize` is called (the engine's add-before-init rule), then `OnNetworkCacheCreated` fires on the container's host.
- **Parameters.** `pContainer` — the container the cache belongs to; supplies the disk-path identity and, through its context, the host for notifications.
- **Returns.** The new `CACHE*` (owned by the network), or null if `pContainer` is null.
- **Notes.** Called by [`CONTAINER::Open`](../container/CONTAINER.md). A container opens exactly one cache for its lifetime and reaches it via `CONTAINER::Cache()`.

### `void Cache_Close (CONTAINER* pContainer, CACHE* pCache)`
- **Purpose.** Announce (`OnNetworkCacheDeleted`, routed via `pContainer->Context()->Host()`), unregister, and delete `pCache` — which in turn deletes any `FILE`s it still holds.
- **Parameters.** `pContainer` — the owning container, passed explicitly because the network no longer stores one; `pCache` — the cache to close.
- **Notes.** Called by [`CONTAINER::Close`](../container/CONTAINER.md).

### `void Cache_Enum (IENUM_CACHE* pEnum)`
- **Purpose.** Invoke `pEnum->OnCache` for every registered cache. This is how a host inspector enumerates open caches.
- **Parameters.** `pEnum` — the enumeration callback.
- **Notes.** Runs under the cache-registry lock. Because one network is engine-wide, this spans **every** context — a per-context inspector must filter by container.

`IENUM_CACHE` is the matching callback interface:

```cpp
class IENUM_CACHE
{
public:
   virtual ~IENUM_CACHE () {}
   virtual void OnCache (CACHE* pCache) = 0;
};
```

---

## Cache reset (durable "clear the cache")

```cpp
void        Reset      (const std::string& sKey);
std::string Time_Start () const;
```

### `void Reset (const std::string& sKey)`
- **Purpose.** Record, durably, that the cache for a given key was cleared *now*. Stamps the current time against `sKey` in the reset map and persists `network_reset.json`. Any cached file whose `createdAt` predates the stamp is treated as stale and refetched on next access; no files are deleted.
- **Parameters.** `sKey` — the primary fabric's container key the clear is recorded under (a context supplies its `Key_Reset`). An empty key is ignored.
- **Notes.** This is the durable half of "clear the cache and reload" in a multi-origin browser. Why the record is keyed to the primary fabric's container while the clear sweeps the whole context — and why two contexts on the same primary share it — is explained at length in [Network system → Clearing the cache](../../systems/network.md#clearing-the-cache). The staleness currency is a wall-clock timestamp, not the asset index.

### `std::string Time_Start () const`
- **Returns.** The engine session's start time as an ISO-8601 instant, captured in `Initialize`.

---

## See also

- [Network system](../../systems/network.md) — design, fetch flow, threading, cache-reset model, limitations.
- [CACHE](CACHE.md) — the per-container handle files are actually opened against.
- [FILE](FILE.md) — the handle a cache hands out; where `Close` lives.
- [IFILE](IFILE.md) — the listener interface and `IENUM_FILE`.
- [Container API](../container/index.md) — opens and owns a cache.

---

[Network API](index.md) · Prev: [index](index.md) · Next: [CACHE](CACHE.md)
