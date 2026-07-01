---
title: CONSOLE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Console.h
  - src/sneeze/console/Console.cpp
  - src/sneeze/console/Console.h
verified: b487fd1
nav:
  prev: api/console/index.md
  next: api/console/STREAM.md
---

# `CONSOLE`

The engine-owned core of the developer console. Exactly one `CONSOLE` exists per [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it and reached through `ENGINE::Console()` (a [`CONTEXT`](../context/index.md) forwards `CONTEXT::Console()` to the engine's single instance). It owns the global in-memory ring buffer of recent entries across *every* context, the table of open per-container [`STREAM`](STREAM.md)s, and the configuration knobs that size both tiers of storage. It is the object an inspector talks to, and the object that mints, sequences, and ring-buffers every entry. For the conceptual picture — the two-tier storage model, how an entry flows to memory and disk, and the on-disk layout — see the [Console system](../../systems/console.md). This page is the exact behavior of every public member.

```cpp
class CONSOLE
{
public:
   explicit CONSOLE (ENGINE* pEngine);
   ~CONSOLE ();

   bool Initialize ();

   void Clear ();

   void Entry_Enum (IENUM_ENTRY* pEnum);

   uint32_t Entries_Cache () const;
   uint32_t Entries_Block () const;
   uint32_t Blocks        () const;

   void     Entries_Cache (uint32_t n);
   void     Entries_Block (uint32_t n);
   void     Blocks        (uint32_t n);

   STREAM*  Stream_Open  (CONTAINER* pContainer);
   void     Stream_Close (STREAM* pStream);
   void     Stream_Enum  (IENUM_STREAM* pEnum);

private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Owned by** the [`ENGINE`](../sneeze/ENGINE.md), constructed with a back-pointer to it and held for the engine's lifetime. One console serves every context in the engine; it is instantiated in the engine's initialization after the cache paths are prepared and deleted when the engine tears down.
- **Owns** the global ring buffer (a `std::deque<std::shared_ptr<const ENTRY>>` spanning all containers and all contexts), the stream table (`std::unordered_map<CONTAINER*, STREAM*>`, also engine-wide), the monotonic entry-index counter, and the sizing configuration.
- **Mints** every entry: the internal write path (driven by a [`STREAM`](STREAM.md)) calls back into the console to construct, timestamp, sequence, and ring-buffer each entry.
- **Notifies** the host of entry creation and deletion. Because one console serves every context, it holds no host pointer of its own — each notification self-resolves the host through the entry's container (`pEntry->Container()->Context()->Host()`). An engine-internal entry with no container is skipped.

Internally `CONSOLE::Impl` derives from the private `ICONSOLE_IMPL` interface, which is what [`STREAM`](STREAM.md) and the private `BLOCK` call back through — for entry creation (`Entry_Create`) and entry lookup by index (`Entry_Find`) — without depending on `CONSOLE` directly.

---

## Threading and pitfalls

**A single recursive mutex guards everything.** `m_mxConsole` is a `std::recursive_mutex`, taken by every public method and by the internal `Entry_Create` / `Entry_Find`. It is recursive because the logging write path re-enters the console (to mint an entry) while the calling `STREAM` already holds its own lock — and stream open/close run under the console lock too.

**Stream pointers are owned by the console.** `Stream_Open` returns a `STREAM*` the console owns and will `delete` on `Stream_Close` (and, as a leak safety net, on console destruction). Do not delete a stream yourself, and do not retain a stream pointer across a `Stream_Close` for that container.

**Enumeration is engine-wide.** Both `Entry_Enum` and `Stream_Enum` span the whole engine, not one context — the ring buffer and stream table are shared across every context. A per-context inspector must therefore filter, enumerating a context's containers and reading each container's one `STREAM` rather than walking the global table. Callbacks fire while `m_mxConsole` is held, so keep them short and do not re-enter the console in a way that blocks on another thread.

**No stream for a null container.** `Stream_Open(nullptr)` returns null; the console does not create an engine-internal stream channel. Engine subsystems log through the engine log instead.

**Configuration timing.** `Entries_Block` and `Blocks` are read only when a stream is *opened* (they are passed into `STREAM::Initialize`), so changing them afterward does not reshape already-open streams. Only `Entries_Cache` (the ring-buffer cap) takes effect for subsequent entry creation. The setters are not lock-protected.

---

## Construction and lifecycle

```cpp
explicit CONSOLE (ENGINE* pEngine);
~CONSOLE ();
bool Initialize ();
```

### `CONSOLE (ENGINE* pEngine)`
- **Purpose.** Construct the console owned by `pEngine` and seed the default sizing: ring-buffer cap 16384, entries-per-block 4096, block-window length 4, entry index 0. Touches no disk and logs nothing — call `Initialize`.
- **Parameters.** `pEngine` — the owning engine; must outlive the console.

### `~CONSOLE ()`
- **Purpose.** Tear down the console. Under the lock it deletes any streams still registered — a leak safety net; normally every container has already closed its stream, and because the owning contexts are gone no `OnConsole*` callback can be routed, so the streams are deleted directly — then clears the ring buffer (releasing the last `shared_ptr` to each entry).

### `bool Initialize ()`
- **Purpose.** Mark the console ready and log an initialization line to the engine log.
- **Returns.** `true`.
- **Notes.** Call once after construction.

---

## Clear

```cpp
void Clear ();
```

### `void Clear ()`
- **Purpose.** Empty the global ring buffer, notifying the host of each removed entry via `OnConsoleEntryDeleted` (self-resolved through each entry's container; entries with no container are dropped silently).
- **Returns.** Nothing.
- **Pitfalls.** Affects **only** the in-memory feed. Per-container block files on disk are not deleted; reopening and attaching a stream still surfaces its historical entries.

---

## Enumeration

```cpp
void Entry_Enum (IENUM_ENTRY* pEnum);
```

### `void Entry_Enum (IENUM_ENTRY* pEnum)`
- **Purpose.** Walk the global ring buffer in creation order, invoking `pEnum->OnEntry` once per entry. This is the inspector's unified, all-container, all-context feed.
- **Parameters.** `pEnum` — the callback interface; no-op if null.
- **Returns.** Nothing.
- **Pitfalls.** Runs under `m_mxConsole`. Only entries still resident in the capped buffer are visited; older entries live only in disk blocks (reachable via a stream). See [Threading and pitfalls](#threading-and-pitfalls) for the engine-wide scope.

---

## Configuration

```cpp
uint32_t Entries_Cache () const;
uint32_t Entries_Block () const;
uint32_t Blocks        () const;

void     Entries_Cache (uint32_t n);
void     Entries_Block (uint32_t n);
void     Blocks        (uint32_t n);
```

These are paired getter/setter overloads (no Get/Set prefixes).

| Member | Meaning | Default | When it takes effect |
|---|---|---|---|
| `Entries_Cache` | Maximum entries held in the global ring buffer. | 16384 | Live, on the next `Entry_Create`. |
| `Entries_Block` | Maximum entries per on-disk block file. | 4096 | Captured when a stream is opened. |
| `Blocks` | Length of each stream's rolling block window. | 4 | Captured when a stream is opened. |

- **Pitfalls.** The setters write the console's stored values but are **not** lock-protected, and `Entries_Block` / `Blocks` are only consulted at `Stream_Open` time — set them before opening the streams you want them to apply to.

---

## Stream management

```cpp
STREAM* Stream_Open  (CONTAINER* pContainer);
void    Stream_Close (STREAM* pStream);
void    Stream_Enum  (IENUM_STREAM* pEnum);
```

### `STREAM* Stream_Open (CONTAINER* pContainer)`
- **Purpose.** Create the disk-backed log channel for `pContainer`, register it in the stream table *before* initializing it (the engine's add-before-init rule), then initialize it with the console's configured `Blocks` and `Entries_Block` sizing.
- **Parameters.** `pContainer` — the container whose channel is wanted.
- **Returns.** The new `STREAM*` on first open; **null** if `pContainer` is null, or if a stream for that container already exists (the call only *creates* — it does not return an existing stream).
- **Ownership.** The console owns the returned stream and deletes it on `Stream_Close` or console destruction.
- **Pitfalls.** Because a second call for an already-open container returns null, callers that may have opened the stream earlier should track the pointer they received rather than re-opening. Initialization reads the stream's `.meta` sidecar and reconstructs its existing block window from disk.

### `void Stream_Close (STREAM* pStream)`
- **Purpose.** Remove `pStream` from the stream table and delete it (which detaches and flushes its blocks and writes its metadata sidecar).
- **Parameters.** `pStream` — the stream to close; no-op if null.
- **Returns.** Nothing.
- **Notes.** No console-level host callback fires here; container lifecycle is signalled separately by the container's own `OnContainerCreated` / `OnContainerDeleted` notifications.
- **Pitfalls.** Invalidates the pointer. Do not use a stream after closing it.

### `void Stream_Enum (IENUM_STREAM* pEnum)`
- **Purpose.** Walk every currently-open stream, invoking `pEnum->OnStream` once per stream. An inspector uses this to list active sources.
- **Parameters.** `pEnum` — the callback interface; no-op if null.
- **Returns.** Nothing.
- **Pitfalls.** Runs under `m_mxConsole`; iteration order is unspecified (the stream table is an unordered map), and the walk is engine-wide across every context.

---

## See also

- [Console system](../../systems/console.md) — design, two-tier storage, limitations.
- [STREAM](STREAM.md) — the per-container channel `Stream_Open` returns.
- [ENTRY](ENTRY.md) — the immutable record `Entry_Enum` hands you.
- [Container API](../container/index.md) — the key streams are opened against.
- [ENGINE](../sneeze/ENGINE.md) — the owner of the single console instance.

---

[Console API](index.md) · Next: [STREAM](STREAM.md)
