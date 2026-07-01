---
title: Storage API
tier: API
audience: [integrator, contributor]
sources:
  - include/Storage.h
verified: b487fd1
nav:
  prev: api/network/IFILE.md
  next: api/storage/STORAGE.md
---

# Storage API

The storage subsystem's public surface is declared in `include/Storage.h`. It is the engine's persistent JSON document store — the analog of a web browser's `localStorage`/`sessionStorage`, but holding structured JSON scoped per cryptographic identity. A caller opens a [`SILO`](SILO.md) for a [container](../container/index.md), attaches it, and reads and writes JSON by scope and path. For the *architecture* — the four scopes, the two-counter unit model, the write-ahead changelog and crash recovery, how organization data is shared — read the [Storage system](../../systems/storage.md) page. This section is the precise per-class reference.

```cpp
#include <Storage.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes and interfaces

| Type | Page | Role |
|---|---|---|
| `STORAGE` | [STORAGE](STORAGE.md) | The engine-owned orchestrator; one per [`ENGINE`](../sneeze/ENGINE.md). Opens/closes silos and owns the unit cache, deduplicated across every context. A thin coordinator. |
| `SILO` | [SILO](SILO.md) | The per-container handle; groups four `UNIT`s by scope and exposes the path-based JSON API. One per [`CONTAINER`](../container/index.md). |
| `UNIT` | [UNIT](UNIT.md) | **Internal.** One JSON file on disk; owns the document, sidecar, and changelog. Forward-declared in the header; documented here because it is essential to understanding `STORAGE`. |

`STORAGE` and `SILO` use the pimpl idiom. `UNIT` is declared in the module's private header, not the public one — it is surfaced in this reference only because the system cannot be understood without it.

> **Who calls this.** The storage surface is reached by sandboxed content code through WASM host functions (scoped to its own silo) and by a host's developer tools through the inspector enumeration. An application embedding Sneeze typically interacts with it via the [context](../context/index.md), not directly.

## Enums

### `eSILO_SCOPE`

Selects one of the four storage units within a [`SILO`](SILO.md). Each pairs a lifetime with a reach.

| Value | Lifetime | Reach |
|---|---|---|
| `kSILO_SCOPE_PERMANENT_ORG` | survives restarts | shared across the organization |
| `kSILO_SCOPE_PERMANENT_COMPANY` | survives restarts | private to this container |
| `kSILO_SCOPE_TEMPORARY_ORG` | wiped at session end | shared across the organization |
| `kSILO_SCOPE_TEMPORARY_COMPANY` | wiped at session end | private to this container |
| `kSILO_SCOPE_COUNT` | — | the count (`4`); not a real scope. |

The "company" scopes are the per-container ones; the "org" scopes resolve to a shared organization document. Permanent scopes live under the context's permanent path, temporary scopes under its temporary path.

## Interfaces

### `IENUM_SILO`

```cpp
class IENUM_SILO
{
public:
   virtual ~IENUM_SILO () {}
   virtual void OnSilo (SILO* pSilo) = 0;
};
```

The enumeration callback for [`STORAGE::Silo_Enum`](STORAGE.md#silo-management). The storage system invokes `OnSilo` once per open silo, under the storage lock, so a host inspector can list them.

---

## See also

- [Storage system](../../systems/storage.md) — design, scopes, durability, threading, limitations.
- [Container API](../container/index.md) — the identity that scopes and isolates a silo.
- [Network API](../network/index.md) — the sibling subsystem with the same lifecycle patterns.

---

[API index](../index.md) · Prev: [IFILE](../network/IFILE.md) · Next: [STORAGE](STORAGE.md)
