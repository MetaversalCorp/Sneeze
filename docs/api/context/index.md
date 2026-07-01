---
title: Context API
tier: API
audience: [integrator, contributor]
sources:
  - include/Context.h
verified: b487fd1
nav:
  prev: api/sneeze/IVIEWPORT.md
  next: api/context/CONTEXT.md
---

# Context API

The context subsystem's public surface is declared in `include/Context.h`. It is a single class, `CONTEXT`, representing one browsing session — the engine's equivalent of a browser tab. For the *architecture* — what a context owns (and the console/network/storage singletons it forwards to instead), the order it builds and tears down its subsystems, how it pools containers, and the cache-reset key — read the [Context system](../../systems/context.md) page. This section is the precise per-class reference: every public method's purpose, parameters, return value, and the pitfalls (locking, lifetime, forwarding) to watch for when calling it.

```cpp
#include <Context.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `CONTEXT` | [CONTEXT](CONTEXT.md) | One browsing session; owns the scene and viewport, forwards to the engine-wide console/network/storage, and pools the session's containers. |

`CONTEXT` uses the pimpl idiom — it is a thin handle over a private implementation.

> **Who calls this.** A host application does not construct a `CONTEXT` directly. It opens one via [`ENGINE::Context_Open`](../sneeze/index.md) and closes it via `ENGINE::Context_Close`. Once it has the pointer, the host reads the owned subsystems (`Scene()`, `Viewport()`) and the forwarded services (`Console()`, `Network()`, `Storage()`), and drives the session hooks (`Reset`, `Logout`, `Clear`). The `Container_Open` / `Container_Close` pair is engine-internal — the scene calls it during fabric loading.

## The `eSESSION` enum

A context is opened as one of two session kinds, declared on `CONTEXT`:

| Value | Meaning |
|---|---|
| `kSESSION_PERSISTENT` | A session whose cache and storage are meant to survive across runs. |
| `kSESSION_TRANSITORY` | A session whose data is meant to be discarded. |

---

## See also

- [Context system](../../systems/context.md) — design, init/teardown order, pooling, cache-reset key.
- [Container API](../container/index.md) — the identity/sandbox `Container_Open` pools.
- [sneeze API](../sneeze/index.md) — `ENGINE::Context_Open` and the `ICONTEXT` host interface.

---

[API index](../index.md) · Prev: [IVIEWPORT](../sneeze/IVIEWPORT.md) · Next: [CONTEXT](CONTEXT.md)
