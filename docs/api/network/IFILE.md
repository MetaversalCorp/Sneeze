---
title: IFILE (interface reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Network.h
  - src/sneeze/network/Asset.cpp
  - src/sneeze/network/Cache.cpp
verified: b487fd1
nav:
  prev: api/network/FILE.md
  next: api/storage/index.md
---

# `IFILE`

The completion-listener interface for a network fetch. A caller that wants to be told when a resource is ready implements `IFILE` and passes it to [`CACHE::File_Open`](CACHE.md#opening-files); the network invokes one of its two methods when the fetch resolves. This page also covers `IENUM_FILE`, the small enumeration callback the inspector uses to walk a cache's file history. For the conceptual picture see the [Network system](../../systems/network.md).

```cpp
class IFILE
{
public:
   virtual ~IFILE () {}
   virtual void OnFileReady  (FILE* pFile) = 0;
   virtual void OnFileFailed (FILE* pFile) = 0;
};
```

---

## Role

`IFILE` is the seam between the network's asynchronous machinery and the code that asked for a resource. You implement it; the network calls it. It carries no state and owns nothing — it is a pure callback interface. The `FILE*` handed to each method is the same handle the caller received from `File_Open`; the listener reads the result through it.

---

## Threading and pitfalls

**Callbacks run on a FETCH agent thread**, never on the thread that called `File_Open`, and they may arrive long after that call returned. Treat the callback body as concurrent code: do not touch caller-thread-only state without synchronization.

**Both callbacks are delivered asynchronously even for cache hits.** When a file attaches to an asset that is already `READY` or `FAILED`, the network still posts a notify-only job rather than calling back inline, so a listener never fires during `File_Open`. You can rely on the callback always being a separate, later event.

**Closing inside the callback is supported and common.** A listener typically calls [`FILE::Close`](FILE.md#close) once it has consumed the bytes. The network's guard flag ensures the resulting deletion is deferred until it is safe — see [Network system → Threading](../../systems/network.md#the-deadlock-and-the-guard-flag). Do not delete the `FILE` yourself.

**Read the bytes before returning if you need them.** After the callback, the caller is expected to close the handle; once closed and cleared the handle is freed. Pull what you need (typically via `ReadData`) within or right after the callback.

---

## Methods

```cpp
virtual void OnFileReady  (FILE* pFile) = 0;
virtual void OnFileFailed (FILE* pFile) = 0;
```

### `void OnFileReady (FILE* pFile)`
- **Purpose.** The resource fetched (or was served from cache) successfully. The handle's snapshot reflects `READY`, and `pFile->ReadData(...)` yields the bytes.
- **Parameters.** `pFile` — the completed handle (the one returned by `File_Open`).
- **Notes.** Invoked on a fetch thread. A typical implementation reads the data and then calls `pFile->Close()`.

### `void OnFileFailed (FILE* pFile)`
- **Purpose.** The fetch failed (network error, or integrity/hash mismatch that could not be resolved). The handle's snapshot reflects `FAILED` and carries the HTTP status where applicable.
- **Parameters.** `pFile` — the failed handle.
- **Notes.** Invoked on a fetch thread. Inspect `pFile->HttpStatus()` / `State()` for diagnostics, then close the handle.

---

## `IENUM_FILE`

```cpp
class IENUM_FILE
{
public:
   virtual ~IENUM_FILE () {}
   virtual void OnAsset (FILE* pFile) = 0;
};
```

A one-method callback for enumerating one cache's file history. Implement it and pass it to [`CACHE::File_Enum`](CACHE.md#opening-files); the cache invokes `OnAsset` once per `FILE` in its file list, under the cache lock. This is how a host's developer tools list every current and past request for a container without owning any of them.

### `void OnAsset (FILE* pFile)`
- **Purpose.** Receive one file from the history during enumeration.
- **Parameters.** `pFile` — a handle in the file list; read its snapshot fields.
- **Notes.** Called synchronously inside `File_Enum` while the cache lock is held — keep the body short and do not block or re-enter the cache in ways that would outlast the recursive lock.

---

## See also

- [Network system](../../systems/network.md) — design, fetch flow, threading, limitations.
- [CACHE](CACHE.md) — where listeners and enumerators are registered.
- [FILE](FILE.md) — the handle delivered to every callback.
- [Storage API](../storage/index.md) — the next section in the reading path.

---

[Network API](index.md) · Prev: [FILE](FILE.md) · Next: [Storage API](../storage/index.md)
