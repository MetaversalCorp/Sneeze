---
title: SILO (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Storage.h
  - src/sneeze/storage/Silo.cpp
verified: b487fd1
nav:
  prev: api/storage/STORAGE.md
  next: api/storage/UNIT.md
---

# `SILO`

The per-container storage handle — the object a caller actually reads and writes through. A `SILO` is created for one [container](../container/index.md) and groups **four [`UNIT`](UNIT.md)s**, one per `eSILO_SCOPE` (permanent/temporary × organization/container). Every read or write names a scope, and the silo routes it to the matching unit. The same silo is handed to WASM host functions (scoped to one source's data) and to a host's developer tools. For the conceptual picture see the [Storage system](../../systems/storage.md); this page is the exact behavior of every public member.

```cpp
class SILO
{
public:
   SILO (ISTORAGE_IMPL* pIStorage_Impl, CONTAINER* pContainer);
   ~SILO ();

   void Initialize ();

   std::string DisplayName () const;
   CONTAINER*  Container   () const;

   nlohmann::json Get    (eSILO_SCOPE eScope, const std::string& sPath) const;
   void           Set    (eSILO_SCOPE eScope, const std::string& sPath, const nlohmann::json& jValue);
   void           Remove (eSILO_SCOPE eScope, const std::string& sPath);
   bool           Has    (eSILO_SCOPE eScope, const std::string& sPath) const;

   std::string    Json (eSILO_SCOPE eScope) const;
   void           Json (eSILO_SCOPE eScope, const std::string& sJson);

   void Attach ();
   void Detach ();

   std::string Path     (eSILO_SCOPE eScope) const;
   std::string Filename (eSILO_SCOPE eScope, const std::string& sExt = "") const;
   std::string Pathname (eSILO_SCOPE eScope, const std::string& sExt = "") const;
private:
   class Impl;
   Impl* m_pImpl;
};
```

> **Do not construct a `SILO` directly.** The storage creates it inside [`STORAGE::Silo_Open`](STORAGE.md#silo-management) and frees it in `Silo_Close`. The constructor signature is shown for completeness; it is for `STORAGE` use only.

---

## Role and ownership

- **Created and owned by** the [`STORAGE`](STORAGE.md), via `Silo_Open`.
- **Bound to** a `CONTAINER` (passed at construction; not owned — the context owns it), whose identity determines the on-disk paths of all four units.
- **References** four `UNIT`s obtained from the storage's engine-wide unit cache at `Initialize`. Per-container ("company") units are unique to this silo; organization units may be shared with other silos for the same organization — in the same context or another.

---

## Lifecycle

A silo is brought up in two steps and torn down by the storage:

1. **Construct + Initialize.** `STORAGE::Silo_Open` constructs the silo, then calls `Initialize`, which opens (references) the four units against the storage's cache. At this point the units exist but their documents are **not loaded**.
2. **Attach.** The caller calls `Attach` to load the units' documents into memory (replaying any changelog). Reads and writes only see data after this.
3. **Detach + Close.** `Detach` flushes and evicts; `STORAGE::Silo_Close` then deletes the silo, which detaches if still attached and closes its units.

---

## Threading and pitfalls

**`m_mxSilo` (a plain `std::mutex`) guards only attach/detach.** It protects the `m_bAttached` flag so the transition runs once. The read/write methods are **not** guarded by it — they route straight to a unit, relying on the unit's own recursive mutex for document-level safety.

**Reads before `Attach` see an empty document.** Because loading is tied to attach, a `Get`/`Has` on a silo that was opened but never attached returns empty results, not the on-disk data. Always `Attach` first.

**`Attach`/`Detach` are idempotent at the silo level.** The `m_bAttached` flag means a second `Attach` without an intervening `Detach` does nothing; the underlying units use their own load counters, so shared org units stay loaded as long as any silo holds them.

**Mutations notify the host.** `Set`, `Remove`, and the `Json` setter route to the underlying `UNIT`, which fires the host's `OnStorageUnitChanged` callback (the setter with an empty path). Because a `UNIT` is shared engine-wide by pathname, it fans the callback out to **every** silo holding it — so all contexts sharing the unit are notified, not just the writer's. Reads do not notify.

**No lifetime guard on the handle.** Nothing prevents another thread from closing the silo while a call is in progress; the silo must outlive its calls.

---

## Construction and lifecycle methods

```cpp
SILO (ISTORAGE_IMPL* pIStorage_Impl, CONTAINER* pContainer);
~SILO ();
void Initialize ();
```

### `SILO (pIStorage_Impl, pContainer)`
- **Purpose.** Construct a silo bound to a container, with its four unit slots empty.
- **Parameters.** `pIStorage_Impl` — the storage back-interface that opens units; `pContainer` — the identity scoping this silo. For `STORAGE` use only.

### `~SILO ()`
- **Purpose.** Detach if still attached, then close all four units (returning them to the storage's cache, which frees any that hit zero references).

### `void Initialize ()`
- **Purpose.** Open the four units — one per scope — against the storage's unit cache, using each scope's computed pathname.
- **Notes.** Called by `STORAGE::Silo_Open`; does not load document data.

---

## Identity

```cpp
std::string DisplayName () const;
CONTAINER*  Container   () const;
```

### `std::string DisplayName () const`
- **Purpose / Returns.** The owning container's display name (from its certificate identity) — a label for inspectors and logs.

### `CONTAINER* Container () const`
- **Purpose / Returns.** The owning container. Used to reach the host for notification routing (`pSilo->Container()->Context()->Host()`), which is how a shared `UNIT` fans change callbacks out to every holding silo's context.

---

## Attach and detach

```cpp
void Attach ();
void Detach ();
```

### `void Attach ()`
- **Purpose.** Load all four units' documents into memory (the first loader of each unit reads its `.json` and replays its `.log`). Marks the silo attached.
- **Notes.** Idempotent — a no-op if already attached. Call before any read or write.

### `void Detach ()`
- **Purpose.** Detach all four units. The last detach of each unit saves its `.meta` sidecar, flushes the document if dirty, and evicts the in-memory JSON.
- **Notes.** Idempotent — a no-op if not attached.

---

## JSON access (path-based)

```cpp
nlohmann::json Get    (eSILO_SCOPE eScope, const std::string& sPath) const;
void           Set    (eSILO_SCOPE eScope, const std::string& sPath, const nlohmann::json& jValue);
void           Remove (eSILO_SCOPE eScope, const std::string& sPath);
bool           Has    (eSILO_SCOPE eScope, const std::string& sPath) const;
```

All four take a scope (selecting the unit) and a dot/bracket path (`player.name`, `game.scores[0]`, `game.poker.table[5].color`).

### `nlohmann::json Get (eScope, sPath)`
- **Purpose.** Read the value at `sPath` in the scope's document.
- **Returns.** The value, or an empty/null JSON value if the path is absent.

### `void Set (eScope, sPath, jValue)`
- **Purpose.** Write `jValue` at `sPath`, creating intermediate objects and extending arrays as needed. Marks the unit dirty, appends a changelog entry, and notifies the host of the change.
- **Parameters.** `eScope` — target unit; `sPath` — the location; `jValue` — the value.

### `void Remove (eScope, sPath)`
- **Purpose.** Delete the leaf at `sPath` (an object key or array element). Marks the unit dirty, appends a changelog entry, and notifies the host.

### `bool Has (eScope, sPath)`
- **Purpose / Returns.** Whether a value exists at `sPath`.

---

## Bulk JSON

```cpp
std::string Json (eSILO_SCOPE eScope) const;
void        Json (eSILO_SCOPE eScope, const std::string& sJson);
```

### `std::string Json (eScope)`
- **Purpose / Returns.** The scope's entire document serialized as a pretty-printed JSON string.

### `void Json (eScope, sJson)`
- **Purpose.** Replace the scope's entire document by parsing `sJson` (an unparseable string yields an empty object). Marks the unit dirty and notifies the host with an empty change path.
- **Notes.** Unlike `Set`, this does not append per-path changelog entries — it replaces the whole document in memory; durability comes at the next save.

---

## Paths

```cpp
std::string Path     (eSILO_SCOPE eScope) const;
std::string Filename (eSILO_SCOPE eScope, const std::string& sExt = "") const;
std::string Pathname (eSILO_SCOPE eScope, const std::string& sExt = "") const;
```

### `std::string Path (eScope)`
- **Purpose / Returns.** The directory a scope's files live in: `CONTAINER::Path_*_Org()` + `"Storage"` for the org scopes (the fingerprint tier, shared by all containers under that identity) or `CONTAINER::Path_*_All()` + `"Storage"` for the container scopes (one level deeper, under the container), choosing the permanent or temporary root by scope.

### `std::string Filename (eScope, sExt)`
- **Purpose / Returns.** The base filename for a scope: `organization` for the org scopes, `container` for the container scopes, optionally suffixed with `sExt`.
- **Notes.** The shared `organization` filename under an identity-keyed path is exactly what makes org units shared across containers of the same organization.

### `std::string Pathname (eScope, sExt)`
- **Purpose / Returns.** `Path(eScope)` joined with `Filename(eScope, sExt)` — the key the storage deduplicates units on.

---

## See also

- [Storage system](../../systems/storage.md) — design, scopes, durability, threading, limitations.
- [STORAGE](STORAGE.md) — creates and owns silos.
- [UNIT](UNIT.md) — the internal per-file document each silo routes to.
- [Container API](../container/index.md) — the identity a silo is bound to.

---

[Storage API](index.md) · Prev: [STORAGE](STORAGE.md) · Next: [UNIT](UNIT.md)
