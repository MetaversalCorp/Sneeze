---
title: Container API
tier: API
audience: [integrator, contributor]
sources:
  - include/Container.h
verified: b487fd1
nav:
  prev: api/context/CONTEXT.md
  next: api/container/CONTAINER.md
---

# Container API

The container subsystem's public surface is declared in `include/Container.h`. It consists of the `CONTAINER` class, its nested identity record `CONTAINER::CID`, and the `eTRUST` trust-level enum. A container is the engine's runtime manifestation of one signed content source — its identity, sandbox, and per-source resources (a network cache, a console stream, and a storage silo). For the *architecture* — why containers exist, what a `CID` is, what `Open`/`Close` manage — read the [Container system](../../systems/container.md) page. This section is the precise per-class reference.

```cpp
#include <Container.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `CONTAINER` | [CONTAINER](CONTAINER.md) | One signed source's runtime identity and sandbox; owns its network cache, console stream, storage silo, WASM store, and the scene nodes of its fabrics; reference-counted and pooled by the context. |
| `CONTAINER::CID` | [CID](CID.md) | The identity record — fingerprint, organization, container name, persona hash, and trust level — that names and pools a container. |

`CONTAINER` uses the pimpl idiom — it is a thin handle over a private implementation. `CID` is a plain value record with public fields and two derived strings.

> **Who calls this.** Containers are engine-internal. The [context](../context/index.md) > creates and pools them (`CONTEXT::Container_Open`), and [fabrics](../scene/FABRIC.md) > open WASM instances in them (`Instance_Open` / `Instance_Close`). A host application > typically only *observes* containers, through the `ICONTEXT::OnContainerCreated` / > `OnContainerDeleted` callbacks, and reads a container's `Identity()` to display it.

## The `eTRUST` enum

A source's trust level, from least to most trusted:

| Value | Meaning |
|---|---|
| `kTRUST_NONE` | No trust evaluated (default on a fresh `CID`). |
| `kTRUST_UNTRUSTED` | The MSF signature did not validate. |
| `kTRUST_UNVERIFIED` | Signature valid, certificate chain not trusted. |
| `kTRUST_EXPIRED` | Chain trusted but expired. |
| `kTRUST_VERIFIED` | Signature valid, chain trusted and current. |
| `kTRUST_ROOT` | The engine's own root container (the source-less root fabric). |

The values are ordered; comparisons like `eTrust >= kTRUST_EXPIRED` are meaningful (and used by [`CID::DisplayName`](CID.md)).

---

## See also

- [Container system](../../systems/container.md) — design, identity, trust, lifecycle.
- [Context API](../context/index.md) — pools containers and assigns their identity.
- [Scene API](../scene/index.md) — fabrics bind to containers and open instances in them.
- [MSF API](../msf/index.md) — the signed manifest a container's identity derives from.

---

[API index](../index.md) · Prev: [CONTEXT](../context/CONTEXT.md) · Next: [CONTAINER](CONTAINER.md)
