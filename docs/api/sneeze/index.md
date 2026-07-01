---
title: Engine API
tier: API
audience: [integrator, contributor]
sources:
  - include/Sneeze.h
verified: b487fd1
nav:
  prev: api/index.md
  next: api/sneeze/ENGINE.md
---

# Engine API

`include/Sneeze.h` is the engine's front door — the single header a host application includes to embed the engine. It declares one concrete class, [`ENGINE`](ENGINE.md), and three interfaces the host implements to plug itself in. For the *architecture* — what the engine owns, how it boots and shuts down, how it manages sessions and the on-disk cache — read the [Engine system](../../systems/engine.md) page. This section is the precise per-class reference: one page each, documenting every member's purpose, parameters, return value, and the pitfalls (lifetime, threading, contract) to watch for.

```cpp
#include <Sneeze.h>      // pulls in Context.h, Container.h, Msf.h, Console.h,
                          // Network.h, Storage.h, Scene.h, Viewport.h, Persona.h
namespace SNEEZE { ... }
```

## How the pieces fit together

Embedding the engine is a matter of implementing three small interfaces and driving one object. The host supplies behavior; the engine supplies everything else.

| Class | Page | Direction | Role |
|---|---|---|---|
| `ENGINE` | [ENGINE](ENGINE.md) | host → engine | The one object the host constructs. Owns every shared subsystem, the engine thread, and the contexts. |
| `IENGINE` | [IENGINE](IENGINE.md) | engine → host | Engine-level configuration and logging. The host implements it; the engine reads paths and the renderer name from it and logs through it. |
| `ICONTEXT` | [ICONTEXT](ICONTEXT.md) | engine → host | Per-session inspector callbacks (container, network, storage, console lifecycle). The host implements it to observe a context. |
| `IVIEWPORT` | [IVIEWPORT](IVIEWPORT.md) | engine → host | Per-viewport rendering callbacks (native window, frame size, finished frame). The host implements it to display rendered output. |

The relationship is a layered hand-off:

- A host **implements `IENGINE`** and constructs an `ENGINE` with it. The engine reads its data path and renderer name and routes all log output back through it.
- The host **opens a context** with `ENGINE::Context_Open`, passing an `ICONTEXT` implementation. The engine calls that interface's `On…` methods as containers, network caches and files, storage silos, and console entries come and go inside the session.
- The host **activates a viewport** on the context, passing an `IVIEWPORT` implementation (via `VIEWPORT::Activate`, documented under the [Viewport API](../viewport/index.md)). The compositor calls that interface to learn the window and frame size and to deliver each finished frame.

`ENGINE` is the only class here a host instantiates. The three interfaces are pure abstract — the host derives from them. `ENGINE` itself uses the pimpl idiom: it is a thin handle over a private implementation.

> **Who calls this.** `ENGINE` and `IENGINE` are the engine/host boundary every embedder touches. `ICONTEXT` and `IVIEWPORT` are also implemented by the host, but their methods are *called by the engine*, often from background threads — read each page's threading notes before doing real work inside a callback.

---

## See also

- [Engine system](../../systems/engine.md) — bring-up/shutdown, contexts, path management.
- [Control system](../../systems/control.md) — the engine thread and agents behind `ENGINE`.
- [Context API](../context/index.md) — the per-session object `ENGINE` opens.
- [Viewport API](../viewport/index.md) — where `IVIEWPORT` is activated.

---

[API index](../index.md) · Next: [ENGINE](ENGINE.md)
