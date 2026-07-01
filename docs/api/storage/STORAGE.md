---
title: STORAGE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Storage.h
  - src/sneeze/storage/Storage.cpp
verified: b487fd1
nav:
  prev: api/storage/index.md
  next: api/storage/SILO.md
---

# `STORAGE`

The storage subsystem's orchestrator. Exactly one `STORAGE` exists per [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it and reached through `ENGINE::Storage()` (a [`CONTEXT`](../context/index.md) exposes the same singleton via `CONTEXT::Storage()`, which forwards). It is deliberately thin: it opens and closes [`SILO`](SILO.md)s for containers, enumerates them for the inspector, and owns the **unit cache** — the map from on-disk pathname to the live [`UNIT`](UNIT.md) for that file — which, because it is engine-wide, lets containers from the same organization (in the same or different contexts) share one document. Almost all document logic lives on `UNIT`; `STORAGE` is lifecycle and deduplication. For the conceptual picture see the [Storage system](../../systems/storage.md); this page is the exact behavior of every public member.

```cpp
class STORAGE
{
public:
   explicit STORAGE (ENGINE* pEngine);
   ~STORAGE ();

   bool  Initialize ();

   SILO* Silo_Open  (CONTAINER* pContainer);
   void  Silo_Close (CONTAINER* pContainer, SILO* pSilo);
   void  Silo_Enum  (IENUM_SILO* pEnum);
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Owned by** the [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it. One store serves every [`CONTEXT`](../context/index.md) in the engine. It records no disk roots of its own — each silo derives its own paths from the owning [`CONTAINER`](../container/index.md).
- **Owns** the list of open silos and the unit cache (`pathname → UNIT*`). When the last silo referencing a unit closes, the unit is deleted.
- **Reaches** the engine (logging) directly through its `ENGINE*`, and the host (silo notifications) indirectly through each silo's container (`pSilo->Container()->Context()->Host()`) — it caches no context or host pointer of its own.

---

## Threading, locking, and pitfalls

**Two independent recursive mutexes guard the two registries.** `m_mxStorage_Silo` guards the silo list (held by `Silo_Open`, `Silo_Close`, `Silo_Enum`); `m_mxStorage_Unit` guards the unit cache (held by the internal `Unit_Open`/`Unit_Close` a silo's construction and teardown drive). Both are `std::recursive_mutex`. The recursion matters because silo teardown re-enters `Unit_Close` on the same thread.

**Silo pointers are owned by the storage.** `Silo_Open` returns a raw pointer the storage owns; return it via `Silo_Close`, passing the owning container. Do not delete it yourself, and do not retain it after closing.

**Unit sharing is engine-wide, by pathname.** Two silos for containers in the same organization — even in different contexts — resolve their org units to the same pathname and therefore share one `UNIT`. Closing one silo does not free a shared unit while another still references it (the unit's open-count stays positive).

**Enumeration spans every context.** Because one store is engine-wide, `Silo_Enum` visits every open silo across all contexts; a per-context inspector must filter by container.

**Pass-through reads are not serialized at this layer.** Reads and writes go through a silo straight to a unit (guarded by the unit's own mutex); `STORAGE`'s locks cover silo and unit *lifecycle*, not per-operation document access.

---

## Construction and lifecycle

```cpp
explicit STORAGE (ENGINE* pEngine);
~STORAGE ();
bool Initialize ();
```

### `STORAGE (ENGINE* pEngine)`
- **Purpose.** Construct an empty store owned by `pEngine`. Touches no disk and records no roots — a silo derives its own paths from its container.
- **Parameters.** `pEngine` — the owning engine; must outlive the storage.

### `~STORAGE ()`
- **Purpose.** Delete every silo still registered, then delete every remaining cached unit. This is an engine-teardown leak safety net: normally every container has already closed its silo. Because the owning contexts and containers are already gone, no `OnStorageSiloDeleted` callback is routed — the silos and units are deleted directly.
- **Notes.** Takes `m_mxStorage_Silo` to drain the silo list, then `m_mxStorage_Unit` to drain the unit cache.

### `bool Initialize ()`
- **Purpose.** Bring the store online. Currently this only logs that the subsystem initialized — there is no eager disk scan; units load lazily on attach.
- **Returns.** `true`.

---

## Silo management

```cpp
SILO* Silo_Open  (CONTAINER* pContainer);
void  Silo_Close (CONTAINER* pContainer, SILO* pSilo);
void  Silo_Enum  (IENUM_SILO* pEnum);
```

### `SILO* Silo_Open (CONTAINER* pContainer)`
- **Purpose.** Create a silo for `pContainer`: construct it, register it in the open list *before* initializing (the engine's add-before-init rule), initialize it (which opens its four units against the unit cache), and fire `OnStorageSiloCreated` on the container's host.
- **Parameters.** `pContainer` — the identity the silo's data is scoped to. Must be non-null.
- **Returns.** The new `SILO*` (owned by the storage), or null if `pContainer` is null.
- **Notes.** Opening a silo references its units but does **not** load their data — the caller must call [`SILO::Attach`](SILO.md#attach-and-detach) before reading or writing.

### `void Silo_Close (CONTAINER* pContainer, SILO* pSilo)`
- **Purpose.** Fire `OnStorageSiloDeleted` (routed via `pContainer->Context()->Host()`), remove the silo from the open list, and delete it. Deleting the silo detaches it (flushing dirty data) and closes its units, freeing any that drop to zero references.
- **Parameters.** `pContainer` — the owning container, passed explicitly because the singleton no longer stores one and needs it to route the deletion callback; `pSilo` — the silo to close. A null `pSilo` is ignored.
- **Notes.** This is how a caller returns a silo handle; do not delete it directly.

### `void Silo_Enum (IENUM_SILO* pEnum)`
- **Purpose.** Invoke `pEnum->OnSilo` once for each open silo, so a host inspector can enumerate the live stores.
- **Parameters.** `pEnum` — the enumeration callback. A null pointer is ignored.
- **Thread-safety.** Runs under `m_mxStorage_Silo`; keep the callback body short and avoid re-entering storage lifecycle beyond what the recursive lock permits. Because one store is engine-wide, this spans **every** context — a per-context inspector must filter by container.

---

## See also

- [Storage system](../../systems/storage.md) — design, scopes, durability, threading, limitations.
- [SILO](SILO.md) — the handle this class hands out and the path-based API.
- [UNIT](UNIT.md) — the internal per-file document this class caches.
- [Container API](../container/index.md) — the identity passed to `Silo_Open`.

---

[Storage API](index.md) · Prev: [index](index.md) · Next: [SILO](SILO.md)
