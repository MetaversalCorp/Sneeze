---
title: Persona API
tier: API
audience: [integrator, contributor]
sources:
  - include/Persona.h
verified: b487fd1
nav:
  prev: api/msf/CHAIN.md
  next: api/persona/PERSONA.md
---

# Persona API

The persona subsystem's public surface is declared in `include/Persona.h`. It is a single class, `PERSONA`, in the **`SNEEZE::persona`** namespace (note the lowercase sub-namespace). For the *architecture* — what a persona is, why it exists as a temporary stub, and how its hash scopes persistent storage — read the [Persona system](../../systems/persona.md) page. This section is the precise class reference.

```cpp
#include <Persona.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { namespace persona { ... } }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `PERSONA` | [PERSONA](PERSONA.md) | A temporary local identity proxy: a name and a SHA-256 hash used to scope stores and storage. |

> **Who calls this.** One `PERSONA` exists per [`ENGINE`](../../systems/engine.md), reachable as `ENGINE::Persona()`. A host application sets the active persona; the [storage](../../systems/storage.md) and [container](../container/index.md) layers read its hash to isolate per-user state. It is a testing stub, **not** an authentication mechanism.

---

## See also

- [Persona system](../../systems/persona.md) — design, the storage-scoping role, limitations.
- [Storage API](../storage/index.md) — what the persona hash scopes.
- [Container API](../container/index.md) — the identity triple the persona hash feeds.

---

[API index](../index.md) · Prev: [CHAIN](../msf/CHAIN.md) · Next: [PERSONA](PERSONA.md)
