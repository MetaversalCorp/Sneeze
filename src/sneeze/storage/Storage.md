# Storage — Persistent JSON Document Store

The `storage` module provides persistent, per-persona, per-organization JSON
document storage — analogous to `localStorage`/`sessionStorage` in a web
browser but with native JSON documents instead of flat key-value pairs.

## Architecture

```
STORAGE (engine singleton, constructor takes ENGINE*)
 ├── m_umpUnit: pathname -> UNIT*      (one per JSON file, shared across SILOs)
 ├── m_apSilo: vector<SILO*>          (one per Silo_Open call)
 ├── m_mxStorage_Silo (recursive_mutex)
 └── m_mxStorage_Unit (recursive_mutex)

UNIT (one per JSON file on disk)
 ├── nlohmann::json document (in-memory cache)
 ├── .meta sidecar, .log changelog (JSONL write-ahead)
 ├── m_apSilo: vector<SILO*>          (the SILOs holding this UNIT; change fan-out)
 ├── m_nCount_Open (lifetime) / m_nCount_Load (cache)
 └── m_mxUnit (recursive_mutex)

SILO (groups four UNITs for a container)
 ├── CONTAINER* m_pContainer
 ├── UNIT* indexed by eSILO_SCOPE [4]
 ├── m_bAttached
 └── Path-based API: Get/Set/Remove/Has/Json
```

All public types (`eSILO_SCOPE`, `SILO`, `IENUM_SILO`, `STORAGE`) are peers
in the `SNEEZE` namespace.

## Usage

```cpp
SILO* pSilo = pContext->Storage ()->Silo_Open (pContainer);
pSilo->Attach ();

pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name", "Dean");
pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "player.chips", 5000);
auto jName = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name");
bool bHas  = pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "player.chips");
pSilo->Remove (kSILO_SCOPE_PERMANENT_CONTAINER, "player.chips");

// Organization storage (shared with other containers from same org)
pSilo->Set (kSILO_SCOPE_PERMANENT_ORG, "org.theme", "dark");

// Bulk JSON
std::string sJson = pSilo->Json (kSILO_SCOPE_PERMANENT_CONTAINER);
pSilo->Json (kSILO_SCOPE_TEMPORARY_CONTAINER, "{\"session\": {\"start\": 12345}}");

pSilo->Detach ();
pContext->Storage ()->Silo_Close (pContainer, pSilo);
```

## Scope Enum

```cpp
enum eSILO_SCOPE
{
   kSILO_SCOPE_PERMANENT_ORG       = 0,   // shared across org, survives restarts
   kSILO_SCOPE_PERMANENT_CONTAINER = 1,   // private to container, survives restarts
   kSILO_SCOPE_TEMPORARY_ORG       = 2,   // shared across org, wiped on session end
   kSILO_SCOPE_TEMPORARY_CONTAINER = 3,   // private to container, wiped on session end
};
```

## Path Navigation

Dot notation for objects, brackets for arrays:

| Path | Meaning |
|------|---------|
| `player.name` | `root["player"]["name"]` |
| `game.scores[0]` | `root["game"]["scores"][0]` |
| `game.poker.table[5].color` | Deep nested access |

Intermediate objects auto-created on `Set()`. Array indices auto-extend.

An **empty path** (`""`) addresses the root document itself: `Get("")` returns
the whole document, `Set("", v)` replaces it, `Remove("")` clears it to `{}`,
and `Has("")` is always true (the root is always present). The bulk `Json()`
accessors are the same operation with string I/O and remain for callers that
want the raw JSON text.

## Two-Counter Ownership

- **m_nCount_Open** — how many SILOs reference this UNIT (lifetime in map)
- **m_nCount_Load** — how many consumers have JSON data loaded (cache state)

Attach increments load count (calls Load on 0->1). Detach decrements (saves +
evicts on 1->0). Org UNITs shared across containers via m_nCount_Open.

## Crash Durability — JSONL Changelog

Every mutation appends a JSONL line to a `.log` sidecar (`["Set","path","value"]`).
On clean save, the `.json` is written and `.log` deleted. On crash recovery,
`.log` is replayed on top of the last `.json`.

## Disk Layout

Organization-scoped data lives at the fingerprint (org) tier — shared by every
container under that identity. Company-scoped data lives under the container.

```
Org scope   (CONTAINER::Path_*_Org + "Storage"):
<PermanentPath>/<persona>/<fp-2>/<fp-22>/Storage/
    ├── organization.json          (org permanent)
    ├── organization.json.meta
    └── organization.json.log
<TemporaryPath>/<persona>/<fp-2>/<fp-22>/Storage/
    └── organization.json          (org temporary)

Company scope (CONTAINER::Path_* + "Storage"):
<PermanentPath>/<persona>/<fp-2>/<fp-22>/<container>/Storage/
    ├── container.json             (container permanent)
    ├── container.json.meta
    └── container.json.log
<TemporaryPath>/<persona>/<fp-2>/<fp-22>/<container>/Storage/
    └── container.json             (container temporary)
```

`SILO` builds these from `CONTAINER`'s path accessors and appends only the
`Storage` segment; the identity prefix is owned by `CONTAINER`, never re-derived
in `SILO`. Each scope's leaf directory is created by `UNIT::Open` on first open
(the `UNIT` owns its own directory, parallel to how `ASSET` owns its cache dir).

## Notifications (ICONTEXT)

Two tiers, mirroring `NETWORK` (`Cache` handle tier + `File` leaf tier):

```cpp
// Handle tier — fired by STORAGE in Silo_Open / Silo_Close
OnStorageSiloCreated (SILO*)
OnStorageSiloDeleted (SILO*)

// Leaf tier — fired by UNIT (Unit.cpp)
OnStorageUnitCreated (SILO*, eSILO_SCOPE)
OnStorageUnitChanged (SILO*, eSILO_SCOPE, const std::string& sPath)
OnStorageUnitDeleted (SILO*, eSILO_SCOPE)
```

Because one `STORAGE` serves every context, the host is self-resolved through the
silo's container: `pSilo->Container()->Context()->Host()`. `Silo_Close` takes
`(CONTAINER*, SILO*)` so the silo-deleted callback can be routed.

**Cross-context fan-out.** A `UNIT` is deduplicated engine-wide by pathname, so two
contexts holding the same org/company file share one `UNIT`. `UNIT::Open(pSilo)` /
`Close(pSilo)` track the holding silos in `m_apSilo` and fire
`OnStorageUnit{Created,Deleted}` for that one silo. A mutation (`Set`/`Remove`/the
`Json` setter) calls `UNIT::Notify_Changed`, which loops `m_apSilo` and fires
`OnStorageUnitChanged` to **every** holding silo — so all contexts sharing the
`UNIT` are notified, not just the writer's. The `Json` setter passes an empty path.
This is the structural analog of `ASSET` driving `FILE::Notify_Changed`, except a
`UNIT` is both the shared object and the leaf, so it drives its own fan-out.

## Thread Safety

- `STORAGE` — `m_mxStorage_Silo` + `m_mxStorage_Unit` (recursive_mutex; silo registry vs unit registry)
- `UNIT` — `m_mxUnit` (recursive_mutex per unit)
- `SILO` — `m_mxSilo` (mutex)

## Files

| File | Contents |
|------|----------|
| `include/Storage.h` | Public header — eSILO_SCOPE, SILO, IENUM_SILO, STORAGE |
| `Storage.cpp` | STORAGE + Impl (Silo_Open/Close/Enum, Unit_Open/Close, paths) |
| `Unit.cpp` | UNIT + Impl (JSON access, path navigation, changelog, meta) |
| `Silo.cpp` | SILO (path-based API, Attach/Detach, scope routing) |
| `Storage.h` | Private header — UNIT, ISTORAGE_IMPL |
