---
title: FILE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Network.h
  - src/sneeze/network/File.cpp
  - src/sneeze/network/Asset.cpp
verified: b487fd1
nav:
  prev: api/network/CACHE.md
  next: api/network/IFILE.md
---

# `FILE`

A per-caller handle to a cached resource. Every call to [`CACHE::File_Open`](CACHE.md#opening-files) returns one `FILE`; each caller that wants a URL gets its own. The handle carries a **snapshot** of the resource's display-level fields — copied from the shared, private `ASSET` at defined moments — so it can keep reporting what happened even after it has detached from the live data. For the conceptual picture (the CACHE/ASSET/FILE split, the two-counter lifecycle, deferred deletion) see the [Network system](../../systems/network.md). This page is the exact behavior of every public member.

```cpp
class FILE
{
public:
   FILE (ICACHE_IMPL* pICache_Impl, uint32_t nFileIx,
         const std::string& sUrl, const std::string& sHash, bool bCacheEnabled);
   ~FILE ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

> **Do not construct or delete a `FILE` directly.** The owning [`CACHE`](CACHE.md) creates it inside `File_Open` and frees it when both deletion flags are set. The constructor signature is shown for completeness; it is for `CACHE` use only.

---

## Role and ownership

- **Created and owned by** its [`CACHE`](CACHE.md). Handed to callers as a raw pointer that stays valid until the cache deletes it.
- **Reaches everything through** the cache's private `ICACHE_IMPL` — its single owner. File-lifecycle operations are implemented by the cache; asset operations and the permanent cache path are forwarded by the cache to the [`NETWORK`](NETWORK.md).
- **References** exactly one private `ASSET` — the shared, one-per-URL state, owned by the network — which it opens in `Initialize` and closes in its destructor.
- **Holds** an optional `IFILE` listener, a snapshot of display fields, its capture of the cache-enabled flag, and its two one-way deletion flags.
- **Bound to** a [`CONTAINER`](../container/index.md) (through its cache), whose identity determines the on-disk path the asset is cached at.

---

## Two field families: snapshot vs. ASSET-dependent

The accessors fall into two groups, and the difference matters once a file has closed.

**Snapshot fields** are stored on the `FILE` itself, copied from the asset by the `Snapshot*` methods during the fetch lifecycle. They remain readable for the life of the handle, **including after `Close`** — this is what lets an inspector keep displaying a finished request. They are: `State`, `IsReady`, `Url`, `Hash`, `IsHashed`, `FileIx`, `AssetIx`, `HttpStatus`, `FetchQueuedTime`, `FetchStartTime`, `FetchEndTime`, `FetchDuration`, `IsServedFromCache`, `ContentType`, and `SizeBytes`.

**ASSET-dependent fields** read straight through to the attached asset: `ReadData`, `DiskPath`, `CreatedTime`, `LastAccessTime`, `AccessCount`, `ReqHeaders`, `RspHeaders`, and `RemoteAddress`. They return meaningful values only while the asset is attached and live; once the last attach detaches and the asset evicts (or after the handle closes), they return empty or default values. `RemoteAddress` in particular dereferences the asset pointer directly — read it only on a live, attached handle.

---

## Three ways a FILE is used

The same class serves three distinct callers, distinguished by whether a listener is attached and who drives deletion.

1. **Active fetch with a listener.** A consumer opens the file with an `IFILE` listener. `Initialize` attaches the handle (allowing a fetch), so the asset fetches if the bytes are not already cached and valid. When the result is ready the listener receives `OnFileReady` (or `OnFileFailed`) on a fetch thread; the consumer reads the bytes via `ReadData` and then calls `Close`.

2. **Inspector observe.** A host's developer tools receive `FILE*` pointers via [`CACHE::File_Enum`](CACHE.md#opening-files) and the `ICONTEXT` file notifications. The inspector only reads snapshot fields and never owns the fetch; it dismisses a row with `Clear` (setting the clear flag), which is independent of the consumer's `Close`.

3. **Passive open.** A caller opens the file with a null listener. The asset is created and referenced but **not attached** — nothing is fetched and the handle sits in `IDLE`. The caller may later `Attach` explicitly (which attaches *without* forcing a fetch, surfacing cached data) and `Detach`, then `Close`.

---

## Lifecycle and the dual-flag deletion

A `FILE` is born in `File_Open`, lives in the cache's file list, and is deleted only when **both** of two independent one-way gates have fired:

- **`Pending_Close`** — the *caller* is finished with the handle. Set via `Close`. It detaches the listener and ends engagement with the asset.
- **`Pending_Clear`** — the *inspector* has dismissed the handle. Set via `Clear` (or by [`CACHE::Clear`](CACHE.md#cache-management)). It fires the host's file-deleted notification.

Each setter, after flipping its own flag, checks whether the *other* flag is already set; if so, the cache erases and deletes the handle. Either order works — whichever side fires last frees the object. A handle whose caller has closed it but that the inspector still shows stays alive (for snapshot reads) until the inspector clears it, and vice versa.

The **guard flag** (`Guard`) protects the one dangerous moment: a fetch completion holding the asset lock while a listener's `Close` tries to take the network lock. While guarded, `Close` defers; the completion path performs the deferred close after releasing the asset lock. See [Network system → Threading](../../systems/network.md#the-deadlock-and-the-guard-flag).

---

## Threading and pitfalls

**Per-file locking is a recursive mutex** (`m_mxFile`), held by `Initialize`, `Attach`, `Detach`, the pending-flag setters, and `Notify_Changed`. The guard (`m_bGuarded`) is a separate `std::atomic<bool>`, exchanged rather than locked.

**Listener callbacks run on FETCH threads.** Your `OnFileReady` / `OnFileFailed` runs on a fetch agent. If it calls `Close` during the callback, the guard ensures the actual deletion is deferred until the completion path is safe — do not work around this.

**ASSET-dependent accessors can return empty after detach/close.** Snapshot fields survive; pass-through fields do not. Read `RemoteAddress` only while attached.

**Do not retain the handle past deletion.** The network deletes the `FILE` once both flags are set; a pointer held after that is dangling. Read everything you need before the final `Close`/`Clear` pairing completes.

**`Url()` and snapshot getters return by value; some pass-throughs return by const reference into the asset.** Treat reference-returning pass-throughs (`ReqHeaders`, `RspHeaders`, `RemoteAddress`) as valid only for the duration of the call's surrounding lock scope.

---

## Lifecycle methods

```cpp
bool Initialize (IFILE* pListener = nullptr);
bool Attach     ();
void Detach     ();
bool Guard      (bool bValue);
void Clear      ();
void Close      ();
void Reset      ();
```

### `bool Initialize (IFILE* pListener = nullptr)`
- **Purpose.** Bind the handle to its asset (find-or-create) and, if a listener is given, attach with a fetch allowed — kicking off a fetch when the bytes are not already cached and valid. Notifies the host that a file was created.
- **Parameters.** `pListener` — the completion listener, or null for a passive open.
- **Returns.** `true` if an asset was attached (handle is usable); `false` otherwise.
- **Notes.** Called by `CACHE::File_Open`; not a method callers invoke directly. If the host declines the created file, the handle is cleared.

### `bool Attach ()`
- **Purpose.** Attach the handle to its asset **without** forcing a fetch (`bFetch = false`). Used by a passive opener to engage cached data, or by an inspector to load the sidecar.
- **Returns.** `true` if the attach succeeded (asset index matched); `false` if the handle holds a stale asset index.
- **Notes.** Increments the file's attach count, which the asset reflects in its own two-counter model.

### `void Detach ()`
- **Purpose.** Release one attach taken by `Initialize` or `Attach`. On the asset's last detach, the sidecar is flushed and the asset's in-memory fields are evicted.
- **Notes.** Safe to call only while the attach count is positive; a no-op otherwise.

### `bool Guard (bool bValue)`
- **Purpose.** Atomically set the deferred-deletion guard and return its previous value (an exchange). The fetch-completion path arms it before notifying and disarms it after; the close path consults it to decide whether to defer.
- **Parameters.** `bValue` — the new guard value.
- **Returns.** The previous guard value.
- **Notes.** Internal coordination; not for application use. See the [system page](../../systems/network.md#the-deadlock-and-the-guard-flag).

### `void Clear ()`
- **Purpose.** Mark the handle's *clear* flag (the inspector-dismissal gate). Fires the host's file-deleted notification; deletes the handle if the *close* flag is also set.

### `void Close ()`
- **Purpose.** Mark the handle's *close* flag (the caller-done gate), detaching the listener; deletes the handle if the *clear* flag is also set. **This is how a caller returns a handle** — there is no public `File_Close` on the cache or network.
- **Pitfalls.** If the handle is currently guarded (mid-fetch-completion), the close is deferred and performed safely by the completion path.

### `void Reset ()`
- **Purpose.** Mark the underlying asset for re-fetch/destruction — routes to the asset's reset, discarding cached bytes (or flagging them) so the next attach re-fetches.

---

## Accessors — snapshot fields (survive Close)

```cpp
eASSET_STATE State            () const;
bool         IsReady          () const;
std::string  Url              () const;
std::string  Hash             () const;
bool         IsHashed         () const;
uint32_t     FileIx           () const;
uint32_t     AssetIx          () const;
long         HttpStatus       () const;
double       FetchQueuedTime  () const;
double       FetchStartTime   () const;
double       FetchEndTime     () const;
double       FetchDuration    () const;
bool         IsServedFromCache() const;
std::string  ContentType      () const;
uint64_t     SizeBytes        () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `State()` | The snapshot `eASSET_STATE`. | Updated by the `Snapshot*` methods. |
| `IsReady()` | Whether the snapshot state is `READY`. | |
| `Url()` | The requested URL, by value. | Fixed at construction. |
| `Hash()` | The accepted hash, by value. | Empty until snapshotted post-fetch. |
| `IsHashed()` | Whether a hash was recorded. | |
| `FileIx()` | The per-network file index. | Monotonic; identifies the handle in the inspector. |
| `AssetIx()` | The snapshot asset index. | Set by `SnapshotInitial`. |
| `HttpStatus()` | The HTTP status code. | Zero until a real fetch completes. |
| `FetchQueuedTime()` | Time the fetch was queued. | Currently always `0` (instrumentation stub). |
| `FetchStartTime()` | Time the fetch started. | |
| `FetchEndTime()` | Time the fetch ended. | |
| `FetchDuration()` | `FetchEndTime − FetchStartTime`. | |
| `IsServedFromCache()` | Whether the bytes came from disk. | |
| `ContentType()` | The response content-type, by value. | Snapshotted from response headers. |
| `SizeBytes()` | The payload size in bytes. | |

---

## Accessors — ASSET-dependent (empty/default after Close)

```cpp
void        ReadData       (std::vector<uint8_t>& aData) const;
std::string DiskPath       () const;
std::string CreatedTime    () const;
std::string LastAccessTime () const;
uint32_t    AccessCount    () const;
const std::unordered_map<std::string, std::string>& ReqHeaders () const;
const std::unordered_map<std::string, std::string>& RspHeaders () const;
const std::string& RemoteAddress () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `ReadData(aData)` | The cached payload bytes, by out-parameter. | Reads `.data` from disk when the asset is `READY`; clears `aData` otherwise. |
| `DiskPath()` | The `.data` file path. | |
| `CreatedTime()` | The asset's creation timestamp. | |
| `LastAccessTime()` | The asset's last-access timestamp. | |
| `AccessCount()` | How many times the asset was accessed. | |
| `ReqHeaders()` | The request headers map, by const reference. | Valid only while attached. |
| `RspHeaders()` | The response headers map, by const reference. | Valid only while attached. |
| `RemoteAddress()` | The peer address, by const reference. | Dereferences the asset directly — read only on a live, attached handle. |

---

## Accessors — container, paths, listener, open-time state

```cpp
std::string  ContainerName () const;
std::string  Path     () const;
std::string  Filename (const std::string& sExt = "") const;
std::string  Pathname (const std::string& sExt = "") const;
IFILE*       Listener () const;
const std::string& OpenHash    () const;
bool               CacheEnabled () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `ContainerName()` | The owning container's display name. | |
| `Path()` | The fan-out directory the asset's files live in. | `CACHE::Path()` (via `ICACHE_IMPL::Path()`) joined with the URL disk key's fan-out prefix; the identity portion is not re-derived here. |
| `Filename(sExt)` | The base filename, optionally with an extension. | The disk key with its fan-out prefix removed. |
| `Pathname(sExt)` | `Path()` joined with `Filename(sExt)`. | The key the network deduplicates assets on. |
| `Listener()` | The attached `IFILE`, or null. | Nulled when the close flag is set. |
| `OpenHash()` | The hash requested at open time, by const reference. | The integrity requirement the caller asked for. |
| `CacheEnabled()` | The cache-enabled flag captured at construction. | Drives the force-fetch-when-disabled path. |

---

## Internal members (NETWORK use only)

```cpp
bool IsPending_Clear () const;
bool IsPending_Close () const;
bool Pending_Clear   ();
bool Pending_Close   ();
void Pending_Reset   ();
void Notify_Changed  ();
void SnapshotInitial ();
void SnapshotProgress ();
void SnapshotFinal   ();
```

These drive the deletion gates and the snapshot pipeline and are called by the network and asset code, not by applications.

- **`IsPending_Clear()` / `IsPending_Close()`** — read the two deletion flags.
- **`Pending_Clear()`** — set the clear flag (once); fires the host file-deleted notification. Returns whether it changed.
- **`Pending_Close()`** — set the close flag (once); detaches the listener. Returns whether it changed.
- **`Pending_Reset()`** — route to the asset's reset.
- **`Notify_Changed()`** — fire the host file-changed notification (unless already cleared).
- **`SnapshotInitial()` / `SnapshotProgress()` / `SnapshotFinal()`** — copy successive layers of asset fields into the handle's snapshot (index; then state and timing; then the full result set).

---

## See also

- [Network system](../../systems/network.md) — design, fetch flow, threading, limitations.
- [CACHE](CACHE.md) — opens files and owns their deletion.
- [NETWORK](NETWORK.md) — owns the shared per-URL asset store the file references.
- [IFILE](IFILE.md) — the listener interface whose callbacks a `FILE` delivers.
- [Container API](../container/index.md) — the identity behind a file's path.

---

[Network API](index.md) · Prev: [CACHE](CACHE.md) · Next: [IFILE](IFILE.md)
