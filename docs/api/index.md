---
title: API Reference
tier: API
sources:
  - include/Sneeze.h
  - include/Context.h
  - include/Container.h
  - include/Console.h
  - include/Network.h
  - include/Storage.h
  - include/Scene.h
  - include/Viewport.h
  - include/Msf.h
  - include/Persona.h
  - include/Image.h
verified: b487fd1
---

# API Reference

The public API of Sneeze is the set of headers in `include/`. Everything else under `src/` is private implementation. An integrator embeds Sneeze by including these headers, implementing a few host interfaces, and calling into the classes below.

The API tier is organized **one folder per public header, one page per class**. Each class page documents every public method — purpose, parameters, return value — and the nuances and pitfalls (locking, lifetime, reentrancy) that matter when calling it. For the *why* and the architecture behind a group of classes, follow the link to its [Systems](../systems/index.md) page.

| Header | Section | Classes |
|---|---|---|
| `Sneeze.h` | [sneeze](sneeze/index.md) | `ENGINE`, `IENGINE`, `ICONTEXT`, `IVIEWPORT` |
| `Context.h` | [context](context/index.md) | `CONTEXT` |
| `Container.h` | [container](container/index.md) | `CONTAINER`, `CONTAINER::CID` |
| `Console.h` | [console](console/index.md) | `CONSOLE`, `ENTRY`, `STREAM` |
| `Network.h` | [network](network/index.md) | `NETWORK`, `CACHE`, `FILE`, `IFILE` |
| `Storage.h` | [storage](storage/index.md) | `STORAGE`, `SILO`, `UNIT` |
| `Scene.h` | [scene](scene/index.md) | `SCENE`, `FABRIC`, `NODE` |
| `Viewport.h` | [viewport](viewport/index.md) | `VIEWPORT`, `RENDERER` |
| `Msf.h` | [msf](msf/index.md) | `MSF` |
| `Persona.h` | [persona](persona/index.md) | `PERSONA` |
| `Image.h` | [image](image/index.md) | `IMAGE` |

The host-facing interfaces (`IENGINE`, `ICONTEXT`, `IVIEWPORT`) are the seam between your application and the engine: you implement them, Sneeze calls them. Start with [sneeze](sneeze/index.md) to see how they fit together.

> The [scene](scene/index.md) section is the worked exemplar of this structure: a section index plus one page per class ([SCENE](scene/SCENE.md), `FABRIC`, `NODE`).

---

[Home](../Home.md)
