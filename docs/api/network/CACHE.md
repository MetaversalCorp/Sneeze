---
title: CACHE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Network.h
  - src/sneeze/network/Cache.cpp
verified: b487fd1
nav:
  prev: api/network/NETWORK.md
  next: api/network/FILE.md
---

# `CACHE`

A per-container handle to the network's file tier. One `CACHE` exists per [`CONTAINER`](../container/index.md), opened from [`NETWORK::Cache_Open`](NETWORK.md#cache-registry) when the container opens and held for the container's lifetime (reach it via `CONTAINER::Cache()`). The cache is the object callers actually open files against: it owns the container's [`FILE`](FILE.md) handles and contributes the file-handle layer, while forwarding every asset operation and the permanent cache path to its owning [`NETWORK`](NETWORK.md). The deduplicated on-disk store itself is owned by the network and shared across every cache. For the conceptual picture — the three-tier split and how a fetch flows — see the [Network system](../../systems/network.md). This page is the exact behavior of every public member.

```cpp
class CACHE
{
public:
   CACHE (INETWORK_IMPL* pINetwork_Impl, CONTAINER* pContainer);   // NETWORK use only
   ~CACHE ();

   void Initialize ();

   std::string DisplayName () const;

   FILE* File_Open (const std::string& sUrl, IFILE* pListener);
   FILE* File_Open (const std::string& sUrl, const std::string& sHash,
                    uint32_t nAssetIx = 0, IFILE* pListener = nullptr);
   void  File_Enum (IENUM_FILE* pEnum);

   void  SetCacheEnabled (bool b);
   bool  IsCacheEnabled  () const;
   void  Clear ();

   std::string Path     () const;
   std::string Filename (const std::string& sExt = "") const;
   std::string Pathname (const std::string& sExt = "") const;

private:
   class Impl;
   Impl* m_pImpl;
};
```

> **Do not construct or delete a `CACHE` directly.** The network creates it inside `Cache_Open` and frees it in `Cache_Close`. The constructor is shown for completeness; it is for `NETWORK` use only.

---

## Role and ownership

- **Created and owned by** the [`NETWORK`](NETWORK.md), one per [`CONTAINER`](../container/index.md). Returned as a raw pointer that stays valid until `Cache_Close`.
- **Owns** the container's list of [`FILE`](FILE.md) handles and the monotonic per-cache file index. Its destructor deletes any files still held (a leak safety net — normally each file has been closed and cleared).
- **Forwards** asset find-or-create, asset close, and the permanent cache path to its `NETWORK` (the disk cache is engine-wide, not per cache). It resolves the host itself, via `m_pContainer->Context()->Host()`.
- **Is** the `FILE`'s single owner: a `FILE` reaches its file-lifecycle operations (`File_Close` / `File_Clear` / `File_Reset`) and asset operations through the cache's private `ICACHE_IMPL`.

---

## Threading and pitfalls

**Per-cache locking is a recursive mutex** (`m_mxCache`) guarding the file list. It is held by `File_Open`, `File_Enum`, `Clear`, and the internal file-lifecycle paths. It is recursive because a file close reached during teardown re-enters the cache.

**`File_Open` never blocks on the network.** The bytes arrive asynchronously — through the [`IFILE`](IFILE.md) listener on a fetch agent thread — even for cache hits. The returned handle is usable immediately (in `IDLE`, or `FETCHING` if a fetch was triggered).

**Returned `FILE*` pointers are owned by the cache.** Do not delete them; return a handle via [`FILE::Close`](FILE.md#close). Deletion is deferred until the handle's two gates (close + clear) have both fired.

**`SetCacheEnabled` is not retroactive.** The flag is stamped onto each *new* file at open; files opened earlier keep the value they captured.

---

## Construction and lifecycle

```cpp
CACHE (INETWORK_IMPL* pINetwork_Impl, CONTAINER* pContainer);   // NETWORK use only
~CACHE ();
void Initialize ();
```

### `CACHE (INETWORK_IMPL* pINetwork_Impl, CONTAINER* pContainer)`
- **Purpose.** Construct the cache bound to a network (for asset/path forwarding) and a container (for identity, host resolution, and reset-key lookup). Cache is enabled by default.
- **Notes.** Called only by `NETWORK::Cache_Open`.

### `~CACHE ()`
- **Purpose.** Delete any `FILE`s still registered, then release. Each surviving file's destructor closes its asset back through the network.

### `void Initialize ()`
- **Purpose.** Create the container's `Network` directory on disk (`Path()`). Called by `NETWORK::Cache_Open` immediately after the cache is registered.

---

## Identity

### `std::string DisplayName () const`
- **Returns.** The owning container's display name (`CONTAINER::Identity()->DisplayName()`). Used to label the cache in an inspector.

---

## Opening files

```cpp
FILE* File_Open (const std::string& sUrl, IFILE* pListener);
FILE* File_Open (const std::string& sUrl, const std::string& sHash,
                 uint32_t nAssetIx = 0, IFILE* pListener = nullptr);
void  File_Enum (IENUM_FILE* pEnum);
```

### `FILE* File_Open (const std::string& sUrl, IFILE* pListener)`
- **Purpose.** Open a handle for `sUrl` with no integrity hash. Creates the [`FILE`](FILE.md) (stamping the cache's current cache-enabled flag), registers it, and initializes it. If `pListener` is non-null the handle attaches with a fetch allowed — triggering a download when the bytes are not already cached and valid; if it is null the open is *passive* (the handle sits in `IDLE`, nothing is fetched).
- **Parameters.** `sUrl` — the resource URL; `pListener` — the completion listener, or null for a passive open.
- **Returns.** The new `FILE*` (owned by the cache).
- **Notes.** A convenience overload forwarding to the four-argument form with an empty hash. The handle is registered *before* it is initialized (add-before-init).

### `FILE* File_Open (const std::string& sUrl, const std::string& sHash, uint32_t nAssetIx, IFILE* pListener)`
- **Purpose.** As above, but records `sHash` as the required Subresource-Integrity hash — the fetched bytes must match or the fetch fails (`OnFileFailed`).
- **Parameters.** `sUrl` — the resource URL; `sHash` — an SRI hash (`algorithm-hexdigest`, one of `sha256` / `sha384` / `sha512`), or empty for none; `nAssetIx` — reserved (currently unused by the file layer); `pListener` — the completion listener, or null.
- **Returns.** The new `FILE*`.

### `void File_Enum (IENUM_FILE* pEnum)`
- **Purpose.** Invoke `pEnum->OnAsset` for every `FILE` this cache holds — current and historical (closed-but-not-cleared) — under the cache lock. This is how a host inspector lists one container's requests.
- **Parameters.** `pEnum` — the enumeration callback. See [`IFILE`](IFILE.md#ienum_file).

---

## Cache management

```cpp
void SetCacheEnabled (bool b);
bool IsCacheEnabled  () const;
void Clear ();
```

### `void SetCacheEnabled (bool b)`
- **Purpose.** Set the cache-enabled flag applied to *subsequent* `File_Open` calls. When disabled, a newly opened file forces a fresh fetch rather than serving cached bytes.
- **Notes.** Not retroactive — see [Threading and pitfalls](#threading-and-pitfalls).

### `bool IsCacheEnabled () const`
- **Returns.** The current cache-enabled flag.

### `void Clear ()`
- **Purpose.** Sweep the cache's file list, marking each handle's *clear* gate. A handle whose *close* gate is already set is erased and deleted; the rest remain (for snapshot reads) until their caller closes them. This is the inspector's "clear all" for one container.

---

## Paths

```cpp
std::string Path     () const;
std::string Filename (const std::string& sExt = "") const;
std::string Pathname (const std::string& sExt = "") const;
```

### `std::string Path () const`
- **Returns.** The container's cache root — `CONTAINER::Path_Permanent_All()` joined with `"Network"`. The identity prefix (`persona / fingerprint / container`) is owned by the container; the cache appends only its `Network` segment. Every asset's fan-out leaf is created beneath this. Created once at `Initialize`.

### `std::string Filename (const std::string& sExt = "") const`
- **Returns.** A base filename, optionally suffixed with `.sExt`. Placeholder helper (parallel to [`SILO`](../storage/SILO.md) and [`FILE`](FILE.md)); currently unused by the file layer.

### `std::string Pathname (const std::string& sExt = "") const`
- **Returns.** `Path()` joined with `Filename(sExt)`.

---

## See also

- [Network system](../../systems/network.md) — design, fetch flow, threading, cache-reset model.
- [NETWORK](NETWORK.md) — opens and closes caches; owns the shared asset store.
- [FILE](FILE.md) — the handle this cache hands out.
- [IFILE](IFILE.md) — the listener interface and `IENUM_FILE`.
- [Container API](../container/index.md) — owns the cache and supplies its identity.

---

[Network API](index.md) · Prev: [NETWORK](NETWORK.md) · Next: [FILE](FILE.md)
