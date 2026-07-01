---
title: Image API
tier: API
audience: [integrator, contributor]
sources:
  - include/Image.h
verified: b487fd1
nav:
  prev: api/persona/PERSONA.md
  next: api/image/IMAGE.md
---

# Image API

The image subsystem's public surface is declared in `include/Image.h`. It is not a class — it is a single free function, `SNEEZE::IMAGE::Decode`, that turns encoded image bytes (PNG, JPEG, BMP, GIF, …) into raw RGBA pixels. It is the smallest public surface in the engine, and intentionally so: image decoding is a leaf utility with no state and no lifecycle.

```cpp
#include <Image.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { namespace IMAGE { ... } }
```

## Contents

| Symbol | Page | Role |
|---|---|---|
| `IMAGE::Decode` | [IMAGE](IMAGE.md) | Free function: decode encoded image bytes into 8-bit RGBA pixels. |

> **Who calls this.** The [scene](../scene/index.md) layer uses it to decode a texture after the [network](../network/index.md) layer fetches it, before handing the pixels to the [viewport](../viewport/index.md) for upload. Any code holding encoded image bytes can call it directly.

---

## See also

- [Scene API](../scene/NODE.md) — `NODE` decodes a fetched texture with this function.
- [Network API](../network/index.md) — fetches the encoded bytes `Decode` consumes.
- [Viewport API](../viewport/index.md) — consumes the decoded pixels for rendering.

---

[API index](../index.md) · Prev: [PERSONA](../persona/PERSONA.md) · Next: [IMAGE](IMAGE.md)
