# Network — Resource Fetching and Caching

The `network` module provides handle-based, type-agnostic resource fetching and
caching. All fetched files persist on disk across restarts. Files with a
cryptographic hash are additionally integrity-verified.

The module is split into two tiers, mirroring `STORAGE`/`SILO` and
`CONSOLE`/`STREAM`:

- **NETWORK** — an engine-owned singleton (one per `ENGINE`, constructed with
  `ENGINE*`). Owns the deduplicated **asset** tier (one `ASSET` per cached URL,
  now shared across every context) and the background fetch machinery.
- **CACHE** — a per-container handle opened from `NETWORK::Cache_Open()`. Owns
  the **file** tier (the `FILE` handles for one container) and forwards asset
  operations to its `NETWORK`.

Each `CONTAINER` opens exactly one `CACHE` for its lifetime; reach it via
`CONTAINER::Cache()`.

## Architecture

```
NETWORK (engine singleton, constructor takes ENGINE*)
 ├── m_umpAsset: pathname -> ASSET*      (active assets with FILE handles)
 ├── m_apCache: vector<CACHE*>           (open per-container handles)
 ├── m_nAssetIx_Next / m_nAssetIx_Reserve (monotonic asset counter + reserve ceiling)
 ├── m_umsReset: key -> ISO timestamp    (per-primary-container clears)
 ├── m_sTime_Stale                       (global stale floor, always a real timestamp)
 ├── network_reset.json                  (counter + global floor + reset map)
 ├── m_mxNetwork_Reset  (recursive, mutable) (m_umsReset + m_sTime_Stale + counter + file)
 ├── m_mxNetwork_Cache  (recursive)      (m_apCache registry)
 └── m_mxNetwork_Asset  (recursive)      (m_umpAsset)

CACHE (per-container handle, ctor takes INETWORK_IMPL* + CONTAINER*)
 ├── m_pINetwork_Impl                    (forwards Asset_Open/Close, Path)
 ├── m_pContainer                        (identity for disk paths; resolves Host via Context)
 ├── m_apFile: vector<FILE*>             (this container's FILE handles)
 ├── m_nNextFileIx                       (monotonic file counter)
 ├── m_bCacheEnabled                     (stamped onto each new FILE)
 └── m_mxCache (recursive_mutex)

ASSET (internal, pImpl, one per URL — owned by NETWORK)
 ├── STATE lifecycle: IDLE -> FETCHING -> VALIDATING -> READY / FAILED
 ├── m_nCount_Open (structural)
 ├── m_nCount_Attach (active listeners, triggers fetch)
 ├── m_pAsset_Fetch (IJOB*, current fetch job)
 └── Meta sidecar (.meta) per asset

FILE (per-caller handle, pImpl — owned by CACHE)
 ├── ICACHE_IMPL* m_pICache_Impl         (its single owner — cache + forwarders)
 ├── IFILE* listener
 ├── Snapshot fields (three phases: Initial, Progress, Final)
 ├── m_bPending_Close / m_bPending_Clear (dual-flag deletion)
 └── Path computation (persona/fp/container/diskkey fan-out)
```

`FILE` reaches everything through `ICACHE_IMPL`: its file lifecycle
(`File_Clear`/`File_Close`/`File_Reset`) is implemented by `CACHE`, while asset
operations and the permanent cache path are forwarded by `CACHE` to `NETWORK`.
The host (`ICONTEXT`) is no longer forwarded by `NETWORK`; `CACHE` resolves it
itself via `m_pContainer->Context()->Host()`, since one engine-wide `NETWORK`
serves many contexts.

All public types (`eASSET_STATE`, `eASSET_EXT`, `FILE`, `IFILE`, `IENUM_FILE`,
`CACHE`, `NETWORK`) are peers in the `SNEEZE` namespace.

## Usage

### Basic Fetch

```cpp
class MY_LISTENER : public SNEEZE::IFILE
{
   void OnFileReady (SNEEZE::FILE* pFile) override { /* use pFile->ReadData() */ }
   void OnFileFailed (SNEEZE::FILE* pFile) override { /* handle failure */ }
};

MY_LISTENER listener;
SNEEZE::CACHE* pCache = pContainer->Cache ();
SNEEZE::FILE* pFile = pCache->File_Open (sUrl, &listener);
// ... later:
pFile->Close ();
```

### Hash-Verified Fetch

```cpp
SNEEZE::FILE* pFile = pCache->File_Open (sUrl, sSriHash, 0, &listener);
```

### Passive Open (no fetch)

```cpp
SNEEZE::FILE* pFile = pCache->File_Open (sUrl, nullptr);   // null listener
// Returns valid handle in IDLE state; no fetch triggered
```

## Container Lifecycle

- **NETWORK::Cache_Open (pContainer)** — creates a `CACHE`, registers it in
  `m_apCache`, calls `CACHE::Initialize()` (registered-before-init, mirroring
  `STORAGE::Silo_Open`), fires `OnNetworkCacheCreated`, and returns it. Called
  by `CONTAINER::Open()`.
- **NETWORK::Cache_Close (pContainer, pCache)** — fires `OnNetworkCacheDeleted`
  (routed via `pContainer->Context()->Host()`), unregisters and deletes the
  `CACHE` (which deletes its remaining `FILE`s). Called by `CONTAINER::Close()`.
  The container is passed explicitly because `NETWORK` no longer stores one.
- **NETWORK::Cache_Enum (IENUM_CACHE*)** — enumerates every registered `CACHE`.
  Now engine-wide (spans all contexts), so a per-context inspector must filter.
- A leaked `CACHE` (container never closed it) is reaped by `~NETWORK`, which
  deletes all registered caches directly (no host callback — the owning contexts
  are already gone) before draining the asset map.

## File Lifecycle

- **CACHE::File_Open** — creates FILE, finds-or-creates ASSET (via NETWORK).
  With listener: implicit Attach triggers fetch. Without: passive open.
- **FILE::Close** — sets pending-close, calls Detach immediately. FILE stays in
  the cache's history for the inspector until also pending-clear.
- **FILE::Clear** — inspector dismissal toggle. Fires OnNetworkFileDeleted.
- **FILE::Reset** — marks ASSET for destruction on last detach.
- **File deleted** only when BOTH `m_bPending_Close` AND `m_bPending_Clear`.
- **CACHE::Clear** — sweeps one cache's files, clearing each.
- **NETWORK::Reset (sKey)** — stamps the current time against a primary-container
  key in `m_umsReset` and persists. This is the context-wide "clear cache"
  recorded durably; see *Clearing the Cache* below. A blanket network-wide clear
  was removed in the singleton migration — it no longer makes sense when one
  `NETWORK` serves every context. `CONTEXT::Logout` is currently a no-op for the
  same reason.

## Clearing the Cache — Why It Is Hard in a Multi-Origin Browser

"Clear the cache and reload" is one of the most familiar commands in a web browser, and in that world it is conceptually simple. A web browser is **single-origin**: a tab shows one web site at a time, that site is the origin, and clearing the cache means throwing away the files belonging to that one origin and fetching them fresh. There is never any ambiguity about *what* you are clearing, because there is only ever one thing it could be.

A metaverse browser does not work that way. It is **multi-origin** by nature. When you open a context (the metaverse equivalent of a tab), it loads a single **primary fabric** — but that fabric is just the starting point. The primary fabric's container can load other containers, those containers can load still more, and as you move through the world the browser continuously loads and unloads containers. A single context might, over the course of a long session lasting hours or even days, have touched hundreds of different containers from
dozens of different organizations. The context is not "a site." It is a living, changing **collection of containers** — the ones it has already visited, the ones currently in the scene, and the ones it may yet encounter.

This is what makes "clear the cache of this context" genuinely ambiguous. It is the equivalent of standing in a web browser tab that has, over a week, displayed a hundred different web sites, and saying "clear the cache on this tab." Which sites? Everything it has ever shown? Only what is on screen right now? Everything it might show in the future? The command, as stated, does not say — and in a metaverse browser there is no single origin to fall back on.

### Where the command comes from

Two things can ask for a cache clear, and **neither of them names a specific container**:

1. A **URL-level** request — the metaverse analog of "clear cache and hard reload," issued when a context loads or reloads.
2. The **Inspector** — a developer pressing a "clear cache" button.

Both are aimed at a *context*. So the design has to answer the ambiguity on the user's behalf: given a context-wide request, what does the browser actually clear?

### The resolution: clear the whole context, key the record to the primary fabric's container

Two questions are easy to conflate here, and they have different answers: *what does the clear affect*, and *where is the fact that it happened recorded*. Keeping them separate is the key to the whole design.

**What the clear affects: the whole context.** A clear request is context-wide. It does not single out one container — it marks every cached file the context relies on as stale, so the entire context refetches as it continues to run and load containers. This matches what the user means: when they say "clear the cache and reload," they are talking about the experience in front of them — the whole scene — not one fabric buried within it.

**Where it is recorded: under the key of the primary fabric's container.** The browser still needs a durable, stable place to write down the simple fact "this context's cache was cleared at time T." The natural anchor is the context's **primary fabric's container** — the one fabric that defines the context and persists across reloads. (The distinction matters: it is the *container*, the concrete identity-bearing runtime instance of the fabric, not the fabric definition in the abstract.) The record is not physically written inside that container's own folder; it is held centrally (see *How it works in code* below) under the container's **key**. The primary fabric's container is therefore the **home of the record, not the limit of the clear**.

This still borrows the web browser's intuition. In a web browser, clearing the cache "on a tab" is a durable statement keyed to the **site currently occupying that tab**: it outlives the tab, and reopening that site later still sees the clear. The metaverse analog keys that durable statement to the primary fabric's container — the stable identity of what the context is "on" — while letting the clear itself sweep the context as a whole.

### Making the clear durable

A clear that only affects the live, in-memory copy would evaporate the moment the fabric reloads — reload the same fabric and its stale files would come right back. To make the clear *stick*, the record is persisted to disk, keyed by the primary fabric's container. Because the key is stable, the clear survives a reload of that fabric in the same context, and it is still in effect if the same fabric is loaded again later, even in a brand-new context. When a context loads a primary fabric's container, the network layer resolves that container's key against the stored record and applies it.

### One timestamp per primary container

The record does not need to be elaborate. Earlier designs imagined a structured set of cache rules; the simplification is to reduce it to **a single timestamp** per primary container key. The meaning is simply: *any cached file created before this moment is stale and must be refetched.* Clearing the cache is nothing more than stamping that timestamp with the current time. From then on, whenever the context requests a cached file, the network layer compares the file's `createdAt` against the stamp — the file survives iff `createdAt >= stamp` — and serves from disk or refetches accordingly. A reset request arriving when a context initializes simply re-stamps.

### How it works in code

The whole mechanism lives in `network_reset.json`, a single file directly under the engine's cache root (it is **not** scattered per container). It carries three things:

- **A map of primary-container key → stale timestamp.** One entry per container that has been explicitly cleared. A key with no entry has never been cleared.
- **A global stale floor (`m_sTime_Stale`).** A single timestamp applied to *every* key. It is always a real timestamp, never empty.
- **The monotonic asset-index counter (`nAssetIx_Next`).** This is *unrelated to staleness* — it is the durable fetch identity persisted per asset as `nMetaIx`. It rides in the same file because it is correlated state, and it is written reserve-ahead (a ceiling `RESERVE_BLOCK` indices ahead) so a crash skips at most one block while disk writes stay rare.

The effective stale time for a request is the **later** of the global floor and the key's own entry (the most aggressive clear wins). Staleness is wall-clock time, not the asset index: the asset's `createdAt` is compared directly against that resolved timestamp.

Because the currency is a timestamp, **losing or corrupting `network_reset.json` is not fatal.** A failed load sets the global floor to "now," which makes every asset created in a prior session stale — an implicit, whole-cache clear that requires no tree traversal and deletes no files; surviving files simply refetch on next access. The very first run (no file yet) takes the same path and sets the baseline floor; since no cached file can predate the first run, that floor excludes nothing real. The floor is then persisted like any other state, so the implicit clear is itself durable across the next clean load.

### Two contexts that share a primary fabric share the clear

Because the record is keyed by the primary fabric's container — one durable entry — any context whose primary fabric is *that same container* resolves *that same entry*. The direct and intended consequence: if two contexts (two tabs) are running the same primary fabric and you clear the cache in one of them, the other is cleared too. They resolve the same key, so the stale timestamp they apply is identical.

This is, once again, exactly the web-browser intuition. If you have the same site open in two tabs and clear its cache, you expect both tabs to be affected; and after closing the site and reopening it in a fresh tab, you expect the clear to still be in effect, because clearing a site's cache is a durable statement about *that site*, not about one transient tab. Sharing the record is precisely what makes that behavior fall out naturally.

### The flip side: contexts with *different* primaries do not share the clear

The opposite case is just as important, and it is a deliberate decision rather than an accident. Suppose two contexts (tabs) are running at once. In context A, you clear the cache. One of the subsidiary containers loaded inside A's scene happens to be the **primary** fabric of context B.

When you clear in A, A stamps the timestamp under **its own** primary fabric's container key, and that clear lifts into A's running network layer — so A's live copy of that shared subsidiary is treated as cleared. But A does **not** stamp the entry under that subsidiary's *own* key. Therefore context B — where that very fabric is the primary — resolves **no durable clear**. Its cache is untouched.

This is exactly the web-consistent answer, and it is correct: you cleared the fabric *you* were standing in, along with your live view of what it had loaded — you did not reach across and clear someone else's primary site just because it happened to be embedded in yours. It is a deliberate scoping decision, not an oversight.

## Request Deduplication

Multiple callers opening the same URL share a single ASSET — even across different `CACHE`s and now across different contexts, because assets are keyed by disk pathname in the single engine-wide `NETWORK`. Each caller gets its own FILE handle. All listeners are notified when the fetch resolves. (This global dedup is also what eliminates two tabs writing the same on-disk cache file concurrently.)

## Disk Storage

```
<PermanentPath>/<persona>/<fp[0:2]>/<fp[2:24]>/<container>/Network/<dk[0:2]>/
    ├── <dk[2:]>.data    (cached payload)
    ├── <dk[2:]>.meta    (sidecar metadata JSON)
    └── <dk[2:]>.temp    (in-flight download)
```

The identity prefix `<persona>/<fp[0:2]>/<fp[2:24]>/<container>` is owned by
`CONTAINER` (`CONTAINER::Path_Permanent_All()`); subsystems append only their own
segment. The `Network` folder is the `CACHE`'s segment.

Disk key is truncated SHA-1 of URL (24 hex chars). First 2 chars form a
fan-out directory.

`CACHE` exposes the container-level path helpers (parallel to `SILO` and
`FILE`): `CACHE::Path()` returns the container's cache root
(`<container>/Network`, i.e. `CONTAINER::Path_Permanent_All()` + `"Network"`).
`FILE` builds on `CACHE::Path()` via `ICACHE_IMPL::Path()`; assets never
re-derive the identity prefix. `CACHE::DisplayName()` returns the container's
display name. The `Network` directory is created once at `CACHE::Initialize()`;
each `ASSET` creates its own `<dk[0:2]>` leaf at open. `network_reset.json` lives
directly under the engine cache root (`<sPath_Root>/network_reset.json`, passed to
`NETWORK::Initialize`) — a single engine-wide file holding the asset-index counter,
the global stale floor, and the per-primary-container reset map (see *Clearing the
Cache*). It is never scattered per container.

## SRI Hash Format

`algorithm-hexdigest` — supports `sha256`, `sha384`, `sha512`.

## Concurrency

Fetches are dispatched as `JOB_FETCH` jobs via `NETWORK::Queue_Post_Fetch` ->
`ENGINE` -> `CONTROL` -> `POOL_QUEUE`. 16 `AGENT::FETCH` workers perform
blocking curl downloads.

All FILE notifications (OnFileReady/OnFileFailed) are asynchronous — even for
cached files, delivered via notify-only JOB_FETCH jobs to prevent re-entrancy.

## Notifications (ICONTEXT)

```cpp
OnNetworkCacheCreated (CACHE*)
OnNetworkCacheDeleted (CACHE*)

OnNetworkFileCreated (FILE*)    // returns bool (host-decides pattern)
OnNetworkFileChanged (FILE*)
OnNetworkFileDeleted (FILE*)
```

## Thread Safety

`NETWORK` carries three independent locks rather than one. They guard unrelated
state (the reset record, the cache registry, the asset map), are never needed
together, and replaced a single coarse `m_mxNetwork` so cache work and asset work
no longer serialize against each other.

- `NETWORK::m_mxNetwork_Reset` — `m_umsReset`, `m_sTime_Stale`, the asset-index
  counter, and `network_reset.json`; locked by every `Reset_*` and by
  `Asset_Index`. `mutable` (const `Reset_Stale` locks it), recursive
  (`Reset`/`Asset_Index` call `Reset_Save`).
- `NETWORK::m_mxNetwork_Cache` — the cache registry `m_apCache`; locked by every
  `Cache_*`. Recursive (`~NETWORK` holds it across `Cache_Close`).
- `NETWORK::m_mxNetwork_Asset` — the asset map `m_umpAsset`; locked by
  `Asset_Open` / `Asset_Close`.
- `CACHE::m_mxCache` — one cache's file list `m_apFile` (recursive, per cache).
- `ASSET::m_mxAsset` — one asset's state (recursive, per asset).
- `FILE::m_mxFile` — one file's state (recursive, per file).
- `FILE::m_bGuarded` — atomic bool, deadlock avoidance during fetch completion
  (see below).

### Lock Ordering

Cross-lock nesting follows one order, outermost-first:

`NETWORK::m_mxNetwork_Cache (registry) -> CACHE::m_mxCache (files) -> NETWORK::m_mxNetwork_Asset (map) -> ASSET::m_mxAsset`, with `NETWORK::m_mxNetwork_Reset` taken last and never co-held with `ASSET::m_mxAsset`.

- **Cache teardown** (`Cache_Close`, `~NETWORK`) holds the registry lock and
  deletes a `CACHE`, taking that cache's `CACHE::m_mxCache`. `~CACHE` deletes its
  `FILE`s, which re-enter `Asset_Close`; the recursive locks permit the same
  thread to nest.
- **File open/close** (`CACHE::File_Open`, `File_Close`, `Clear`, `~CACHE`)
  holds a cache's `CACHE::m_mxCache`, then drives `Asset_Open` / `Asset_Close`
  under `NETWORK::m_mxNetwork_Asset`, then the per-asset `ASSET::m_mxAsset`.
- **Reset comes last and stands alone**: `Asset_Open` stamps the index via
  `Asset_Index` (`NETWORK::m_mxNetwork_Asset` -> `NETWORK::m_mxNetwork_Reset`).
  The staleness lookup is resolved *before* the asset lock is taken — `FILE::Attach`
  calls `ICACHE_IMPL::Reset_Stale` (which locks `m_mxNetwork_Reset` and releases
  it) and passes the resulting timestamp string into `ASSET::Attach`. So the reset
  lock is never held together with `ASSET::m_mxAsset`.

Splitting the old single `m_mxNetwork` removed two inversions: the
cache-registry vs asset-map conflict (one lock, taken in both orders by
`Cache_Close` and `File_Close`) and the reset vs asset conflict (`Reset_Stale` is
resolved before the asset lock and shares no lock with the asset map).

### Fetch Completion and the Guard Flag

`ASSET::Impl::Fetch_Complete` runs on an `AGENT::FETCH` worker thread. It holds
the asset's `ASSET::m_mxAsset` while iterating `m_apFiles` to snapshot and notify
all attached FILEs. A listener callback (OnFileReady/OnFileFailed) typically
calls `pFile->Close()`, which flows through `CACHE::Impl::File_Close`. If both
pending flags are set (`m_bPending_Close` and `m_bPending_Clear`), `File_Close`
acquires that cache's `CACHE::m_mxCache` to erase and delete the file. If another
thread simultaneously holds `CACHE::m_mxCache` and is waiting on
`ASSET::m_mxAsset` (e.g., inside `File_Open` -> `Asset_Open`), the result is a
deadlock: the fetch thread holds `ASSET::m_mxAsset` and wants `CACHE::m_mxCache`,
the other thread holds `CACHE::m_mxCache` and wants `ASSET::m_mxAsset`.

The solution is a per-FILE atomic guard flag (`m_bGuarded`) that defers file
deletion during fetch completion:

1. **Before the notification loop**, `Fetch_Complete` arms the guard on each
   FILE: `pFile->Guard(true)`.

2. **During the loop**, if a listener callback calls `File_Close`, the deletion
   path in `CACHE::Impl::File_Close` checks the guard via `pFile->Guard(false)`
   (atomic exchange). If the file was guarded, the exchange returns `true`, and
   `File_Close` skips the entire close — no `Pending_Close`, no `Detach`, no
   `CACHE::m_mxCache` acquisition. The exchange atomically clears the guard,
   signaling that a close was attempted.

3. **After the loop**, `Fetch_Complete` checks each file's guard via
   `pFile->Guard(false)`. If the exchange returns `true`, the guard was still
   set — no close was attempted, nothing to do. If it returns `false`, the
   guard was cleared by `File_Close` during the callback — the file is
   collected into a local `apDelete` vector.

4. **After releasing `ASSET::m_mxAsset`**, `Fetch_Complete` processes the
   deferred closes by calling `pFile->Close()` on each collected file. At this
   point no conflicting mutex is held, so the full close path (Pending_Close ->
   Detach -> deletion under `CACHE::m_mxCache`) proceeds without deadlock.

The guard flag is lightweight (one atomic bool per FILE), requires no changes
to the mutex hierarchy, and confines the deadlock avoidance to the single code
path that needs it.

### Notify-Only Fetch Completion

When a FILE attaches to an ASSET that is already READY or FAILED, a notify-only
`ASSET_FETCH` job is posted to the fetch pool instead of delivering the callback
synchronously. This prevents re-entrancy during `File_Open` / `Initialize`.

The notify-only path sets `m_bState = kASSET_STATE_FETCHING` while the job is
in flight to prevent overlapping jobs from overwriting `m_pAsset_Fetch`. If
additional FILEs attach during this window, they land in the FETCHING branch
and wait. When the notify-only job fires, `Fetch_Complete` iterates **all**
files in `m_apFiles` — not just the original requester — so every FILE that
arrived during the window receives its SnapshotFinal, Notify_Changed, and
listener callback.

The trade-off: FILEs that piggy-back on a notify-only job inherit the same
fetch timing and served-from-cache values as the original. This is a minor
reporting approximation — the data they receive is identical.

## Files

| File | Contents |
|------|----------|
| `include/Network.h` | Public header — eASSET_STATE, FILE, IFILE, IENUM_FILE, CACHE, NETWORK |
| `Network.cpp` | NETWORK + Impl (asset tier, Cache_Open/Close/Enum, reset/staleness, fetch queue) |
| `Cache.cpp` | CACHE + Impl (file tier — File_Open/Close/Clear/Reset/Enum; forwards assets) |
| `Asset.cpp` | ASSET + Impl + ASSET_FETCH (fetch lifecycle, FetchComplete) |
| `File.cpp` | FILE + Impl (snapshots, path computation, dual-flag deletion) |
| `Network.h` | Private header — INETWORK_IMPL, ICACHE_IMPL (the FILE's single owner), ASSET |
