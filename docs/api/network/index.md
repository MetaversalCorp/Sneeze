---
title: Network API
tier: API
audience: [integrator, contributor]
sources:
  - include/Network.h
verified: b487fd1
nav:
  prev: api/container/CID.md
  next: api/network/NETWORK.md
---

# Network API

The network subsystem's public surface is declared in `include/Network.h`. It is the engine's resource loader and on-disk cache. The surface is split across two tiers of handle plus their listener interfaces: a single engine-owned [`NETWORK`](NETWORK.md) owns the deduplicated disk cache and the background fetch machinery, while a per-container [`CACHE`](CACHE.md) is the handle a caller actually opens files against. Opening a URL returns a [`FILE`](FILE.md); an optional [`IFILE`](IFILE.md) listener is notified when the bytes arrive. For the *architecture* — why there are three cooperating object types, how the two-counter asset lifecycle works, how a fetch is dispatched, how "clear the cache" is resolved in a multi-origin browser, and how deletion is deferred — read the [Network system](../../systems/network.md) page. This section is the precise per-class reference.

```cpp
#include <Network.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes and interfaces

| Type | Page | Role |
|---|---|---|
| `NETWORK` | [NETWORK](NETWORK.md) | The engine-owned resource system. One per [`ENGINE`](../sneeze/ENGINE.md). Owns the deduplicated per-URL asset store, the background fetch queue, and the durable cache-reset record. Hands out per-container caches. |
| `CACHE` | [CACHE](CACHE.md) | A per-container handle opened from `NETWORK::Cache_Open`. Owns the container's [`FILE`](FILE.md) handles and is the object callers open files against. One per [`CONTAINER`](../container/index.md). |
| `FILE` | [FILE](FILE.md) | A per-caller handle to a cached resource; carries a snapshot of display fields and the dual-flag deletion machinery. |
| `IFILE` | [IFILE](IFILE.md) | The completion-listener interface a caller implements (`OnFileReady` / `OnFileFailed`). Also covers `IENUM_FILE`, the file-enumeration callback. |

`NETWORK`, `CACHE`, and `FILE` all use the pimpl idiom — each is a thin handle over a private implementation. The shared per-URL state lives in a private `ASSET` class that is not part of the public surface (it is documented conceptually in the [Network system](../../systems/network.md#the-three-tiers-network-cache-and-file)). `IENUM_CACHE`, the cache-enumeration callback, is documented alongside [`NETWORK::Cache_Enum`](NETWORK.md#cache-registry).

> **Who calls this.** The network surface is mostly engine-internal — the [scene](../scene/index.md) drives MSF, WASM, and texture fetches through each container's cache — but it is also the seam a host's developer tools attach to, via `CACHE::File_Enum`, `NETWORK::Cache_Enum`, and the `ICONTEXT` notifications. An application embedding Sneeze rarely opens files directly.

## Enums

Defined in `Network.h` and surfaced through `FILE`.

### `eASSET_STATE`

The fetch state of a resource, reported by `FILE::State`.

| Value | Meaning |
|---|---|
| `kASSET_STATE_IDLE` | Never fetched; no request in flight. |
| `kASSET_STATE_FETCHING` | A fetch (or a notify-only completion job) is in flight. |
| `kASSET_STATE_VALIDATING` | Reserved; not used by the current flow (hashing is inline). |
| `kASSET_STATE_READY` | Bytes are cached and valid. |
| `kASSET_STATE_FAILED` | The last fetch failed. |

### `eASSET_EXT`

Selects which of an asset's three on-disk files a path refers to.

| Value | File suffix | Contents |
|---|---|---|
| `kASSET_EXT_DATA` | `.data` | The cached payload. |
| `kASSET_EXT_TEMP` | `.temp` | The in-flight download, renamed to `.data` on success. |
| `kASSET_EXT_META` | `.meta` | The JSON sidecar describing the asset. |

---

## See also

- [Network system](../../systems/network.md) — design, fetch flow, threading, and limitations.
- [Scene API](../scene/index.md) — the primary consumer of `FILE` / `IFILE`.
- [Container API](../container/index.md) — supplies the identity that keys a file's disk path and owns the cache.
- [Storage API](../storage/index.md) — the sibling persistence subsystem.

---

[API index](../index.md) · Prev: [CID](../container/CID.md) · Next: [NETWORK](NETWORK.md)
