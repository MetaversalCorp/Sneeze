---
title: STREAM (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Console.h
  - src/sneeze/console/Stream.cpp
  - src/sneeze/console/Block.cpp
verified: b487fd1
nav:
  prev: api/console/CONSOLE.md
  next: api/console/ENTRY.md
---

# `STREAM`

One container's disk-backed log channel. A `STREAM` owns all of the browser-style console operations for a single [container](../container/index.md) — the severity methods, grouping, counting, and timing — and persists every entry to a rolling window of JSONL block files on disk. Streams are created and owned by [`CONSOLE`](CONSOLE.md): obtain one with `CONSOLE::Stream_Open` and release it with `CONSOLE::Stream_Close`. For the conceptual picture see the [Console system](../../systems/console.md); this page is the exact behavior of every public member.

```cpp
class STREAM
{
public:
   STREAM (ICONSOLE_IMPL* pIConsole_Impl, CONTAINER* pContainer);
  ~STREAM ();

   void Initialize (int nBlocks, int nEntries_Block);
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Created and owned by** the [`CONSOLE`](CONSOLE.md), via `Stream_Open`. Never construct or delete a stream directly.
- **Bound to** one `CONTAINER`, passed at construction and never reassigned. The container supplies the cryptographic identity (`CID`) that partitions the stream's files on disk and the display name.
- **Owns** a vector of [`BLOCK`](../../systems/console.md#block--one-disk-file) objects — its rolling on-disk window — and the per-label state for counters and timers.
- **Calls back** into the console through the private `ICONSOLE_IMPL` interface to mint entries (which timestamps, sequences, and ring-buffers them) before writing each to the active block.

---

## Lifecycle

A stream is brought up in two steps and torn down by the console:

1. **Construct.** `CONSOLE::Stream_Open` constructs the stream against a container.
2. **Initialize.** The console immediately calls `Initialize(nBlocks, nEntries_Block)` with its configured sizing. Initialization reads the stream's `.meta` sidecar (the saved current-block index and entry count) and, if a prior session left blocks behind, reconstructs the in-window `BLOCK` objects pointing at the existing files — so logging resumes at the right block.
3. **Attach / Detach (optional).** A reader (typically an inspector) calls `Attach` to load the window's block files into memory, and `Detach` to unload them and persist the metadata sidecar.
4. **Close.** `CONSOLE::Stream_Close` deletes the stream, which detaches if still attached (writing the sidecar), then deletes its blocks.

---

## Threading and pitfalls

**A single recursive mutex guards the stream.** `m_mxStream` is a `std::recursive_mutex` taken by attach/detach, the write path, grouping, counting, and timing. It is recursive because grouping logs an entry (taking the lock) and then adjusts group depth within the same locked call.

**The write path re-enters the console.** Each logging call mints its entry through the console (which takes `m_mxConsole`) and then writes to a block (which takes the block's mutex). The lock order is stream → console / block; do not invert it.

**Disk writes are synchronous and inline.** Logging appends a JSONL line on the caller's thread through an OS-buffered append stream — there is no background writer and no per-line flush. A burst of logging is paid for by the logging thread.

**Rotation deletes files synchronously.** When the active block fills, the stream rotates; if the window then exceeds its length, the oldest block is detached, deleted, and its `.log` file removed via `std::filesystem::remove` — inline.

**One directory per stream; the block number only names the file.** `Path()` takes no argument and returns the single identity-derived directory that holds all of a stream's block files; `Filename`/`Pathname` take a block number that distinguishes only the *filename* within that directory.

---

## Construction and initialization

```cpp
STREAM (ICONSOLE_IMPL* pIConsole_Impl, CONTAINER* pContainer);
~STREAM ();
void Initialize (int nBlocks, int nEntries_Block);
```

### `STREAM(pIConsole_Impl, pContainer)`
- **Purpose.** Construct a stream bound to a container, calling back through `pIConsole_Impl` for entry creation and the temporary path.
- **Parameters.** `pIConsole_Impl` — the console's private callback interface; `pContainer` — the owning container (required).
- **Note.** Constructed by `CONSOLE::Stream_Open`; not for direct use.

### `~STREAM()`
- **Purpose.** Detach if attached (persisting the metadata sidecar), then delete the block objects.

### `void Initialize (int nBlocks, int nEntries_Block)`
- **Purpose.** Set the rolling-window length and per-block entry cap, then load the metadata sidecar and reconstruct any existing block window from disk.
- **Parameters.** `nBlocks` — number of block files to keep; `nEntries_Block` — maximum entries per block file.
- **Returns.** Nothing.
- **Notes.** Called once by the console right after construction with the console's configured values.

---

## Attach / Detach

```cpp
void Attach ();
void Detach ();
```

### `void Attach ()`
- **Purpose.** Load the stream's current block window from disk into memory so its entries can be enumerated. Idempotent — repeated calls do not reload.
- **Returns.** Nothing.
- **Notes.** Block loading is reference-counted per block, so multiple readers share one resident copy.

### `void Detach ()`
- **Purpose.** Unload the in-memory block caches and write the metadata sidecar (current block index and entry count, plus the container's identity fields).
- **Returns.** Nothing.

---

## Logging

```cpp
void Log    (const std::string& sMessage, bool bSystem = false);
void Debug  (const std::string& sMessage, bool bSystem = false);
void Info   (const std::string& sMessage, bool bSystem = false);
void Warn   (const std::string& sMessage, bool bSystem = false);
void Error  (const std::string& sMessage, bool bSystem = false);
void Assert (bool bCondition, const std::string& sMessage, bool bSystem = false);
```

Each method records one entry at its severity level (`Log` → `kENTRY_LEVEL_LOG`, and so on). All share the same write path: rotate if the active block is full or missing, mint the entry through the console, then append it to the active block.

- **`bSystem`** — marks the entry as browser-injected (`ENTRY::IsSystem`) so an inspector can render it differently from content-authored output. Defaults to `false`.
- **`Assert(bCondition, ...)`** — records an `ERROR` entry only when `bCondition` is false; the message is prefixed with `"Assertion failed: "`. A true condition logs nothing.
- **Returns.** Nothing.
- **Notes.** A message that is a JSON array string (e.g. `["a", {"b":1}]`) is stored verbatim; an inspector can split it for structured display via [`ENTRY::MessageParts`](ENTRY.md#messageparts).

---

## Grouping

```cpp
void Group          (const std::string& sLabel);
void GroupCollapsed (const std::string& sLabel);
void GroupEnd       ();
```

### `void Group (const std::string& sLabel)` / `void GroupCollapsed (const std::string& sLabel)`
- **Purpose.** Open a nesting level. Both log `sLabel` as a `LOG` entry and then increase the stream's group depth, so subsequent entries record a deeper nesting level for indented display. `GroupCollapsed` additionally marks the label entry collapsed (`ENTRY::IsCollapsed`) so an inspector folds it by default.
- **Returns.** Nothing.

### `void GroupEnd ()`
- **Purpose.** Close the innermost group by decrementing the group depth (never below zero).
- **Returns.** Nothing.
- **Notes.** Group depth is stored on each entry at creation time, so unbalanced `Group`/`GroupEnd` calls skew the nesting of later entries but never error.

---

## Counting

```cpp
void Count      (const std::string& sLabel);
void CountReset (const std::string& sLabel);
```

### `void Count (const std::string& sLabel)`
- **Purpose.** Increment the per-label counter and log `"<label>: <count>"` at `INFO` level.
- **Returns.** Nothing.

### `void CountReset (const std::string& sLabel)`
- **Purpose.** Forget the counter for `sLabel` (the next `Count` starts again at one). Logs nothing.
- **Returns.** Nothing.

---

## Timing

```cpp
void Time    (const std::string& sLabel);
void TimeEnd (const std::string& sLabel);
void TimeLog (const std::string& sLabel);
```

Timers use a `steady_clock` start time stored per label.

### `void Time (const std::string& sLabel)`
- **Purpose.** Start (or restart) a timer for `sLabel`. Logs nothing.

### `void TimeEnd (const std::string& sLabel)`
- **Purpose.** Log the elapsed milliseconds since `Time(sLabel)` at `INFO` level (formatted `"<label>: <ms>ms"` to three decimals) and stop the timer. No-op if the label has no running timer.

### `void TimeLog (const std::string& sLabel)`
- **Purpose.** Log the elapsed milliseconds like `TimeEnd`, but leave the timer running so it can be sampled again. No-op if the label has no running timer.

---

## Accessors

```cpp
std::string DisplayName () const;
std::string Path     ()                                              const;
std::string Filename (uint32_t nBlock, const std::string& sExt = "") const;
std::string Pathname (uint32_t nBlock, const std::string& sExt = "") const;
```

| Accessor | Returns |
|---|---|
| `DisplayName()` | The bound container's human-readable display name (`CONTAINER::Identity()->DisplayName()`). |
| `Path()` | The single directory holding the stream's block files: `CONTAINER::Path_Temporary_All()` joined with `"Console"`. Takes no argument — every block shares this directory. |
| `Filename(nBlock, sExt)` | The bare filename for a block: `NNNN` with the zero-padded four-digit block index, plus `.sExt` if given. |
| `Pathname(nBlock, sExt)` | The full path: `Path()` joined with `Filename(nBlock, sExt)`. |

- **Notes.** These build on `CONTAINER`'s path accessors (the identity prefix is not re-derived in the stream); they do not require the stream to be attached. Block `.log` files are produced with `sExt = "log"`.

---

## See also

- [Console system](../../systems/console.md) — design, rotation, on-disk layout, limitations.
- [CONSOLE](CONSOLE.md) — creates, owns, and closes streams.
- [ENTRY](ENTRY.md) — the immutable record each logging call produces.
- [Container API](../container/index.md) — the identity a stream is bound to.

---

[Console API](index.md) · Prev: [CONSOLE](CONSOLE.md) · Next: [ENTRY](ENTRY.md)
