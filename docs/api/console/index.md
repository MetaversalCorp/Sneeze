---
title: Console API
tier: API
audience: [integrator, contributor]
sources:
  - include/Console.h
verified: b487fd1
nav:
  prev: api/storage/UNIT.md
  next: api/console/CONSOLE.md
---

# Console API

The console subsystem's public surface is declared in `include/Console.h`. It is the engine's developer console — the equivalent of a web browser's `console` object — providing per-source logging, grouping, counting, and timing, backed by a bounded in-memory feed and durable per-source disk files. For the *architecture* — the two-tier storage model, how an entry is created, the on-disk layout — read the [Console system](../../systems/console.md) page. This section is the precise per-class reference: each page documents every public method's purpose, parameters, return value, and the pitfalls (locking, lifetime, threading) to watch for when calling it.

```cpp
#include <Console.h>   // brought in transitively via <Sneeze.h>
namespace SNEEZE { ... }
```

## Classes

| Class | Page | Role |
|---|---|---|
| `CONSOLE` | [CONSOLE](CONSOLE.md) | Engine-owned singleton (one per [`ENGINE`](../sneeze/ENGINE.md)); owns the global ring buffer, the stream table, and configuration. |
| `STREAM` | [STREAM](STREAM.md) | One container's disk-backed log channel; owns logging, grouping, counting, and timing. |
| `ENTRY` | [ENTRY](ENTRY.md) | One immutable log record, shared as `std::shared_ptr<const ENTRY>`. |

`CONSOLE` and `STREAM` use the pimpl idiom (thin handles over a private implementation). `ENTRY` is a concrete value-like class always handled by shared pointer.

> **Who calls this.** Sandboxed content logs through the [`STREAM`](STREAM.md) opened for its own [container](../container/index.md). Engine internals create and tear down streams as containers come and go. A host inspector reads the unified feed and drills into individual streams through [`CONSOLE`](CONSOLE.md). An application that only embeds the engine rarely calls this surface directly — it more often consumes the host callbacks `OnConsoleEntryCreated` / `OnConsoleEntryDeleted`.

## Enumeration interfaces

Two tiny callback interfaces let a caller walk the console without exposing its internal containers.

| Interface | Method | Purpose |
|---|---|---|
| `IENUM_ENTRY` | `void OnEntry (std::shared_ptr<const ENTRY>)` | Receives each entry during `CONSOLE::Entry_Enum`. |
| `IENUM_STREAM` | `void OnStream (STREAM*)` | Receives each open stream during `CONSOLE::Stream_Enum`. |

Implement the interface, pass a pointer to the matching `*_Enum` method, and the console invokes your callback once per item under its lock.

## Severity levels

`eENTRY_LEVEL` is the ordered severity enum carried by every entry.

| Constant | Value | Meaning |
|---|---|---|
| `kENTRY_LEVEL_DEBUG` | `0` | Verbose diagnostic output. |
| `kENTRY_LEVEL_LOG` | `1` | General log output (the default). |
| `kENTRY_LEVEL_INFO` | `2` | Informational. |
| `kENTRY_LEVEL_WARN` | `3` | Warning. |
| `kENTRY_LEVEL_ERROR` | `4` | Error. |

---

## See also

- [Console system](../../systems/console.md) — design, two-tier storage, on-disk layout, limitations.
- [Container API](../container/index.md) — the identity each stream is keyed by.
- [Storage API](../storage/index.md) — the other per-container persistence subsystem.

---

[API index](../index.md) · Next: [CONSOLE](CONSOLE.md)
