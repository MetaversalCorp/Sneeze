---
title: UNIT (class reference)
tier: API
audience: [contributor]
sources:
  - src/sneeze/storage/Storage.h
  - src/sneeze/storage/Unit.cpp
verified: b487fd1
nav:
  prev: api/storage/SILO.md
  next: api/scene/index.md
---

# `UNIT`

> **`UNIT` is an internal implementation class, not part of the public API.** It is only forward-declared in `include/Storage.h`; its full declaration lives in the module's private header `src/sneeze/storage/Storage.h`, and an application embedding the engine never calls it. It is documented here because the storage subsystem cannot be understood without it — a [`SILO`](SILO.md) is little more than four units plus routing. This page describes it from the source so contributors can reason about `STORAGE` and `SILO` behavior.

A `UNIT` represents **one JSON file on disk**. It owns the in-memory `nlohmann::json` document, the dot/bracket path-navigation logic, the `.meta` sidecar, and the JSONL write-ahead changelog that gives the store its crash durability. It is the storage counterpart of the network subsystem's private `ASSET`: shared, deduplicated by pathname, and governed by two reference counts. For the conceptual picture see the [Storage system](../../systems/storage.md).

```cpp
class UNIT
{
public:
   UNIT (ISTORAGE_IMPL* pIStorage_Impl, eSILO_SCOPE eScope, const std::string& sPathname);
   virtual ~UNIT ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Created and owned by** the [`STORAGE`](STORAGE.md)'s engine-wide unit cache, via the internal `Unit_Open` (find-or-create by pathname) and freed by `Unit_Close` when its open count reaches zero.
- **Referenced by** one or more [`SILO`](SILO.md)s, tracked in a `m_apSilo` list. A per-container unit has exactly one referencing silo; an organization unit is shared by every silo for that organization, across contexts — which is why org data is consistent across a publisher's containers and drives the change fan-out.
- **Owns** the document, its dirty/loaded flags, the two counters, the holding-silo list, and the sidecar metadata.

---

## The two-counter model

A unit's behavior turns on two independent counters (full discussion on the [system page](../../systems/storage.md#the-two-counter-unit-model)):

- **`m_nCount_Open`** — how many silos reference the unit (its lifetime in the cache). `Open(pSilo)` adds the silo to the holding list and increments it; `Close(pSilo)` removes the silo and decrements, returning the new value, and at zero the storage deletes the unit.
- **`m_nCount_Load`** — how many consumers have the document loaded in memory. `Attach` increments it and loads on `0 → 1`; `Detach` decrements it and saves + evicts on `1 → 0`.

A unit can be referenced (open) without being loaded — an opened-but-not-attached silo leaves its units in exactly that state.

---

## Durability mechanism (internal)

These mechanisms run inside the unit and have no public method surface, but they define how the unit behaves:

- **Path navigation.** A path string is split into segments (dot-separated keys, bracketed array indices) to find the parent node and final key. On `Set`, intermediate objects/arrays are auto-created and array indices auto-extend.
- **JSONL changelog.** Every `Set`/`Remove` appends one JSON-array line (`["Set","path",value]` / `["Remove","path"]`) to a `.log` sidecar before the change is considered durable. The bulk `Json` setter appends a root-level `["Set","",<document>]` so a bulk replace also survives a crash before the next save.
- **Load = parse + replay.** `Load` parses the last good `.json`, then replays the `.log` on top of it — reconstructing the exact pre-crash state.
- **Save = collapse.** `Save` writes the full document (atomic `.temp`-then-rename) and deletes the `.log`, folding accumulated changes back into the base file.
- **Change fan-out.** `Notify_Changed(path)` loops the holding-silo list (`m_apSilo`) and fires `OnStorageUnitChanged` once per silo, so every context sharing the unit hears a mutation — not just the writer's. `Open`/`Close` similarly fire `OnStorageUnitCreated`/`OnStorageUnitDeleted` for the one attaching/closing silo. All three route the host via `pSilo->Container()->Context()->Host()`.
- **Directory creation.** The first `Open` (when the open count is zero) creates the unit's on-disk parent directory — the unit owns its own directory, parallel to the network's `ASSET`.

---

## Threading and pitfalls

**`m_mxUnit` (a recursive `std::recursive_mutex`) guards document operations.** `Get`, `Set`, `Remove`, `Has`, the `Json` getter/setter, `Open`, `Close`, `Load`, `Save`, `Evict`, and `Notify_Changed` all take it. It is recursive because a mutation takes the lock and then calls `Notify_Changed`, which takes it again; a host callback fired from inside `Notify_Changed` must therefore not re-enter storage on the same unit.

**The load counter is not self-synchronized.** The increment/decrement inside `Attach`/`Detach` (`m_nCount_Load`) are plain operations that rely on the caller holding `SILO`'s `m_mxSilo`; `Open`/`Close` themselves take `m_mxUnit` and are driven under `STORAGE`'s `m_mxStorage_Unit`. Do not drive a unit's counters from an unlocked path.

**Detach needs the container.** `Detach` takes a `CONTAINER*` because the `.meta` sidecar it writes on the last detach records that container's identity. The unit does not store the container itself.

**Eviction discards in-memory data.** After the last detach, the document is cleared and `m_bLoaded` is reset; reads then return empty until the next attach reloads.

---

## Construction and destruction

```cpp
UNIT (ISTORAGE_IMPL* pIStorage_Impl, eSILO_SCOPE eScope, const std::string& sPathname);
virtual ~UNIT ();
```

### `UNIT (pIStorage_Impl, eScope, sPathname)`
- **Purpose.** Construct a unit for the file at `sPathname` in scope `eScope`. Reads the `.meta` sidecar if present (size, timestamps, access count) but does **not** load the document.
- **Parameters.** `pIStorage_Impl` — the storage back-interface (paths, host, logging); `eScope` — the unit's scope; `sPathname` — the base on-disk pathname (without extension).

### `~UNIT ()`
- **Purpose.** Destroy the unit and its document. Called by `STORAGE` once the open count hits zero.

---

## State accessors

```cpp
bool        IsLoaded () const;
bool        IsDirty  () const;
eSILO_SCOPE GetScope () const;
```

| Accessor | Returns |
|---|---|
| `IsLoaded()` | Whether the document is currently in memory. |
| `IsDirty()` | Whether there are unsaved in-memory changes. |
| `GetScope()` | The unit's `eSILO_SCOPE`. |

---

## JSON access

```cpp
nlohmann::json Get    (const std::string& sPath) const;
void           Set    (const std::string& sPath, const nlohmann::json& jValue);
void           Remove (const std::string& sPath);
bool           Has    (const std::string& sPath) const;
std::string    Json   () const;
void           Json   (const std::string& sJson);
```

These are the per-unit forms of the silo's JSON API (the silo's scope parameter selects which unit to call). `Get` returns the value or empty; `Set` writes (auto-creating intermediates), marks dirty, touches access time, appends a changelog entry, and fans a change notification out to every holding silo; `Remove` deletes the leaf and does the same; `Has` reports presence; `Json()` serializes the whole document; `Json(sJson)` replaces it by parsing (empty object on parse failure), logs a root-level entry, and notifies with an empty path. All take the unit's recursive mutex, and the mutating forms fire `OnStorageUnitChanged` while holding it.

---

## Lifecycle methods

```cpp
void     Open   (SILO* pSilo);
uint32_t Close  (SILO* pSilo);
void     Attach ();
void     Detach (CONTAINER* pContainer);
void     Load   ();
void     Save   ();
void     Evict  ();
```

### `void Open (SILO* pSilo)`
- **Purpose.** Reference the unit for `pSilo`: on the first open create the on-disk parent directory, add `pSilo` to the holding-silo list, increment the open count, and fire `OnStorageUnitCreated(pSilo, scope)` on the silo's host. Called by `STORAGE::Unit_Open`.
- **Parameters.** `pSilo` — the silo taking a reference; also the host-routing path for the notification.

### `uint32_t Close (SILO* pSilo)`
- **Purpose / Returns.** Fire `OnStorageUnitDeleted(pSilo, scope)`, remove `pSilo` from the holding-silo list, decrement the open count, and return its new value; the storage deletes the unit when this reaches zero.
- **Parameters.** `pSilo` — the silo releasing its reference.

### `void Attach ()`
- **Purpose.** Increment the load count; on `0 → 1`, `Load` the document.

### `void Detach (CONTAINER* pContainer)`
- **Purpose.** Decrement the load count; on `1 → 0`, save the sidecar for `pContainer`, flush the document if dirty, and evict it.
- **Parameters.** `pContainer` — the identity recorded in the `.meta` sidecar.

### `void Load ()`
- **Purpose.** Load the document from disk: parse `.json` (empty object if missing or unparseable), then replay the `.log`, and stamp the creation time if it was unset. Idempotent while loaded. (The parent directory is created by `Open`, not here.)

### `void Save ()`
- **Purpose.** Write the document to `.json` (atomic rename), update the recorded size, delete the `.log`, and clear the dirty flag. No-op if not loaded.

### `void Evict ()`
- **Purpose.** Flush if dirty, then drop the in-memory document and mark unloaded.

---

## Meta sidecar

```cpp
const std::string& Pathname       () const;
uint64_t           SizeBytes      () const;
const std::string& CreatedTime    () const;
const std::string& LastAccessTime () const;
uint32_t           AccessCount    () const;
void               TouchAccess    ();
void               Meta_Save      (CONTAINER* pContainer);
```

| Member | Purpose |
|---|---|
| `Pathname()` | The base on-disk pathname (the unit-cache key). |
| `SizeBytes()` | The last-saved document size in bytes. |
| `CreatedTime()` | The unit's creation timestamp. |
| `LastAccessTime()` | The last-access timestamp. |
| `AccessCount()` | How many times the unit was accessed. |
| `TouchAccess()` | Bump the access timestamp and count (called internally on each mutation). |
| `Meta_Save(pContainer)` | Write the `.meta` sidecar — identity from `pContainer`, plus scope, size, timestamps, and access count. |

---

## See also

- [Storage system](../../systems/storage.md) — design, scopes, durability, threading, limitations.
- [SILO](SILO.md) — groups four units and routes scoped calls to them.
- [STORAGE](STORAGE.md) — owns the unit cache and deduplicates units by pathname.

---

[Storage API](index.md) · Prev: [SILO](SILO.md) · Next: [Scene API](../scene/index.md)
