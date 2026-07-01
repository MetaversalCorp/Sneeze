---
title: MSF API
tier: API
audience: [integrator, contributor]
sources:
  - include/Msf.h
verified: b487fd1
nav:
  prev: api/viewport/RENDERER.md
  next: api/msf/MSF.md
---

# MSF API

The MSF subsystem's public surface is declared in `include/Msf.h`. It consists of a single class, `MSF`, with a nested X.509 validator `MSF::CHAIN` and three nested payload structs (`MSF::SERVICE`, `MSF::MODULE`, `MSF::CERT`). For the *architecture* — what a Metaversal Spatial Fabric file is, why parsing and verification are separate steps, and how cryptographic facts become a trust level — read the [MSF system](../../systems/msf.md) page. This section is the precise per-class reference: every public method's purpose, parameters, return value, and pitfalls.

```cpp
#include <Msf.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `MSF` | [MSF](MSF.md) | The whole MSF lifecycle: parse, sign, verify, certificate management, typed payload access. |
| `MSF::CHAIN` | [CHAIN](CHAIN.md) | X.509 certificate-chain validator over BoringSSL's `X509_STORE`, plus static certificate utilities. |

The nested payload structs — `SERVICE`, `MODULE`, and `CERT` — are documented on the [MSF](MSF.md) page alongside the methods that produce and consume them.

> **Who calls this.** This surface is mostly engine-internal: the [scene](../scene/index.md) loading flow constructs an `MSF`, parses it, verifies it, and the [context](../context/index.md) turns the result into a [container](../container/index.md) identity. An application embedding the engine does not normally touch `MSF` directly. The class is also used by the project's signing tooling to *produce* MSF files.

---

## See also

- [MSF system](../../systems/msf.md) — design, the parse-then-verify model, trust levels.
- [Container API](../container/index.md) — the identity an MSF's verification produces.
- [Scene API](../scene/index.md) — `FABRIC` owns the `MSF` that describes it.

---

[API index](../index.md) · Prev: [RENDERER](../viewport/RENDERER.md) · Next: [MSF](MSF.md)
