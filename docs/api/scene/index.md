---
title: Scene API
tier: API
audience: [integrator, contributor]
sources:
  - include/Scene.h
  - include/Map_Object.h
verified: b487fd1
nav:
  prev: api/index.md
  next: api/scene/SCENE.md
---

# Scene API

The scene subsystem's public surface is declared in `include/Scene.h` (the three structural classes and the object-index constants) and `include/Map_Object.h` (the `MAP_OBJECT` payload hierarchy). For the *architecture* — what the scene model is, how a fabric loads, how the pieces relate — read the [Scene system](../../systems/scene.md) page. This section is the precise per-class reference: one page per class, each documenting every public method's purpose, parameters, return value, and the pitfalls (locking, lifetime, reentrancy) to watch for when calling it.

```cpp
#include <Scene.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `SCENE` | [SCENE](SCENE.md) | Root of the model; owns the root fabric and the scene-global fabric registry; owns the backdrop and drives primary presentation. |
| `FABRIC` | [FABRIC](FABRIC.md) | One source's branch of the tree; bound to a container and an MSF; owns its WASM instances. |
| `NODE` | [NODE](NODE.md) | A single element of the scene tree; points to a `MAP_OBJECT` payload and owns its resource fetch. |

All three use the pimpl idiom — each is a thin handle over a private implementation. The **node handle table** (`Node_Root` / `Node_Open` / `Node_Close` / `Node_Find`) is not on `SCENE`; it is owned by the [`CONTAINER`](../container/index.md), because node identity is per-container. The `MAP_OBJECT` payload hierarchy (`MAP_OBJECT` and its derived types, including `MAP_OBJECT_LIGHT`) is declared in `include/Map_Object.h` and documented in the [Scene system](../../systems/scene.md#map_object--the-renderable-payload) page.

> **Who calls this.** Most of this surface is engine-internal: the engine drives it > during loading and rendering, and the content host functions call into it. An > application embedding Sneeze rarely calls these classes directly — it navigates via > [`CONTEXT`](../context/index.md) and reads frames via [`VIEWPORT`](../viewport/index.md). Each > class page marks which members are integrator-facing and which are internal.

## Object-index constants

Nodes are addressed by a 48-bit object index stored in a `uint64_t` (the low 48 bits are the index; the upper 16 may carry a class discriminator). These reserved values, defined in `Scene.h`, steer allocation and signal results.

| Constant | Value | Meaning |
|---|---|---|
| `TWORD_MAX` | `0x0000FFFFFFFFFFFF` | Maximum 48-bit value. |
| `OBJECTIX_MAX` | `0x0000FFFFFFFFFFFC` | Highest assignable object index. |
| `OBJECTIX_LAST` | `0x0000FFFFFFFFFFFD` | Reserved sentinel. |
| `OBJECTIX_ERROR` | `0x0000FFFFFFFFFFFE` | Returned by create/open calls on failure. |
| `OBJECTIX_INVALID` | `0x0000FFFFFFFFFFFF` | Invalid index. |
| `OBJECTIX_IDENTITY` | `0x0000FFFFFFFFFFFF` | "Assign the next free index." |
| `OBJECTIX_NULL` | `0x0000000000000000` | Null / unset index. |

---

## See also

- [Scene system](../../systems/scene.md) — design, loading flow, and limitations.
- [Container API](../container/index.md) — the identity/sandbox each fabric is bound to.
- [Network API](../network/index.md) — `FILE` / `IFILE`, used for every fetch.

---

[API index](../index.md) · Next: [SCENE](SCENE.md)
