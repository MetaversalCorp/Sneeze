---
title: ENTRY (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Console.h
  - src/sneeze/console/Entry.cpp
verified: b487fd1
nav:
  prev: api/console/STREAM.md
  next: api/network/index.md
---

# `ENTRY`

One immutable console log record: a severity level, a message, a timestamp, the originating [container](../container/index.md), a monotonic index, and grouping metadata. An `ENTRY` is **never modified after construction**, and it is always handled through `std::shared_ptr<const ENTRY>`. That immutability plus shared ownership is what lets the same entry object live at once in the console's global ring buffer, in a disk block's in-memory cache, and inside an inspector callback — no copies, no locking of its contents. For the conceptual picture see the [Console system](../../systems/console.md); this page is the exact behavior of every public member.

```cpp
class ENTRY
{
public:
   ENTRY (CONTAINER* pContainer, eENTRY_LEVEL eLevel, const std::string& sMessage,
          uint32_t nIndex, uint32_t nGroupDepth, bool bCollapsed,
          bool bSystem = false, const std::string& sStackTrace = "",
          const std::string& sSource = "");
   // ... accessors and serialization below
};
```

---

## Role and ownership

- **Created by** the console's internal write path (`ICONSOLE_IMPL::Entry_Create`), which assigns the index and pushes the entry into the ring buffer, or reconstructed from disk by `FromJson` when a block is reloaded.
- **Owned by** `std::shared_ptr<const ENTRY>` everywhere — there is no single owner; the last holder frees it.
- **Self-stamps** its creation time: the constructor sets the timestamp to `std::chrono::system_clock::now()`.
- **Carries a `CONTAINER*`** identifying the source. It may be null for a record with no resolved container (for example, one reconstructed from disk without a container context); the console does not dereference it.

---

## Threading and pitfalls

**Immutable means thread-safe to read.** Once constructed, all fields are read-only, so any number of threads may read an entry concurrently without synchronization. The shared pointer's control block handles lifetime.

**The timestamp is wall-clock, not monotonic.** `tpStamp` is a `system_clock::time_point`; it reflects civil time and can move backward across a clock adjustment. Use `Index()` for ordering, not the timestamp.

**`FromJson` mutates the timestamp once.** The factory constructs the entry and then, exactly once, overwrites its stamp from the serialized value before handing back a `shared_ptr<const ENTRY>`. This is the only write that ever happens to an entry, and it occurs before the entry is shared. Treat entries you receive as fully immutable.

**`ToJson` does not serialize the container.** The container is not written to disk; on reload, `FromJson` takes the container from the caller (the owning stream). Do not expect `Container()` to round-trip through disk.

---

## Construction

```cpp
ENTRY (CONTAINER* pContainer, eENTRY_LEVEL eLevel, const std::string& sMessage,
       uint32_t nIndex, uint32_t nGroupDepth, bool bCollapsed,
       bool bSystem = false, const std::string& sStackTrace = "",
       const std::string& sSource = "");
```

- **Purpose.** Construct an immutable entry and stamp it with the current wall-clock time.
- **Parameters.**
- `pContainer` — the originating container (may be null).
- `eLevel` — the severity ([`eENTRY_LEVEL`](index.md#severity-levels)).
- `sMessage` — the message text (possibly a JSON-array string).
- `nIndex` — the monotonic sequence index assigned by the console.
- `nGroupDepth` — the nesting depth at the time of logging.
- `bCollapsed` — whether a group label entry should fold by default.
- `bSystem` — whether this is a browser-injected (system) entry.
- `sStackTrace`, `sSource` — optional diagnostic context; empty by default.
- **Notes.** Created through the console's write path, not directly by callers. The console's `Entry_Create` supplies the first six arguments and leaves stack trace and source at their defaults.

---

## Accessors

```cpp
eENTRY_LEVEL                          Level       () const;
const std::string&                    Message     () const;
std::chrono::system_clock::time_point tpStamp     () const;
CONTAINER*                            Container   () const;
uint32_t                              Index       () const;
uint32_t                              GroupDepth  () const;
bool                                  IsCollapsed () const;
bool                                  IsSystem    () const;
const std::string&                    StackTrace  () const;
const std::string&                    Source      () const;
```

| Accessor | Returns |
|---|---|
| `Level()` | The severity level. |
| `Message()` | The raw message string (by const reference). |
| `tpStamp()` | The wall-clock creation time. |
| `Container()` | The originating container, or null. |
| `Index()` | The monotonic sequence index — the stable ordering key. |
| `GroupDepth()` | The group nesting depth at log time. |
| `IsCollapsed()` | Whether a group label should fold by default. |
| `IsSystem()` | Whether this is a browser-injected entry. |
| `StackTrace()` | Optional stack trace (empty if none). |
| `Source()` | Optional source location (empty if none). |

---

## Formatting and parsing

```cpp
static void              LevelString  (eENTRY_LEVEL eLevel, std::string& sLevel);
std::string              FormatStamp  () const;
void                     MessageParts (std::vector<std::string>& aParts) const;
```

### `static void LevelString (eENTRY_LEVEL eLevel, std::string& sLevel)`
- **Purpose.** Write the lowercase string name of a level into `sLevel` (`"debug"`, `"log"`, `"info"`, `"warn"`, `"error"`). Defaults to `"log"` for unknown values.
- **Parameters.** `eLevel` — the level; `sLevel` — out-parameter receiving the name.

### `std::string FormatStamp () const`
- **Purpose.** Format the timestamp as local time `"HH:MM:SS.mmm"` (24-hour, with milliseconds).
- **Returns.** The formatted string.

<a id="messageparts"></a>
### `void MessageParts (std::vector<std::string>& aParts) const`
- **Purpose.** Split a structured message into parts for display. If the message is a JSON array (begins with `[` and parses as an array), each element is appended — strings verbatim, non-strings as their JSON dump. Otherwise (or on a parse error) the whole message is appended as a single part.
- **Parameters.** `aParts` — vector that receives the parts (appended, not cleared).
- **Returns.** Nothing.

---

## Serialization

```cpp
nlohmann::json                      ToJson   () const;
static std::shared_ptr<const ENTRY> FromJson (const nlohmann::json& jEntry, CONTAINER* pContainer);
```

### `nlohmann::json ToJson () const`
- **Purpose.** Serialize the entry to a JSON object for one JSONL line in a block file.
- **Returns.** A JSON object with `level` (the string name), `message`, `stamp` (seconds since epoch as a double), `index`, `groupDepth`, `collapsed`, and `system`; `stackTrace` and `source` are included only when non-empty.
- **Notes.** The container is intentionally **not** serialized.

### `static std::shared_ptr<const ENTRY> FromJson (const nlohmann::json& jEntry, CONTAINER* pContainer)`
- **Purpose.** Reconstruct an entry from a JSONL object, binding it to `pContainer`. Used when a block file is reloaded from disk.
- **Parameters.** `jEntry` — the parsed JSON object; `pContainer` — the container to attribute the entry to (the console does not store the container on disk).
- **Returns.** A `shared_ptr<const ENTRY>`. Missing fields fall back to defaults (level `log`, empty message, zeroed index/depth, false flags); a positive `stamp` overrides the construction time.

---

## See also

- [Console system](../../systems/console.md) — how entries flow through the two storage tiers.
- [STREAM](STREAM.md) — produces entries via the logging methods.
- [CONSOLE](CONSOLE.md) — mints, sequences, and ring-buffers entries; enumerates them.

---

[Console API](index.md) · Prev: [STREAM](STREAM.md) · Next: [Network API](../network/index.md)
