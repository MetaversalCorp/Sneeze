# WASM — WebAssembly Sandbox

The `wasm` module provides the sandboxed execution environment for WebAssembly
modules loaded from MSF fabric payloads. It manages the Wasmtime engine,
isolated stores, compiled instances, and host function bindings.

## Architecture

```
WASM_RUNTIME (owns wasm_engine_t)
 ├── WASM_TIMERS (one engine-wide timer service, shared by every store)
 ├── WASM_STORE (one per container identity)
 │    ├── WASM_INSTANCE (url + sha256)
 │    └── WASM_INSTANCE (url + sha256)
 └── WASM_STORE (another container)
```

## WASM_RUNTIME

Top-level manager. Owns the shared Wasmtime engine and all active stores.

```cpp
DEP::WASM_RUNTIME runtime;
runtime.Initialize ();

auto* pStore = runtime.Store_Open ();
// ... add instances ...
runtime.Store_Close (pStore);
```

## WASM_STORE

Isolated execution context identified by (persona hash, fingerprint, container
name). Multiple fabrics from the same organization and container share one store.

`Fabric_AddRef()` / `Fabric_ReleaseRef()` track fabric usage. When refcount
reaches zero, the store is eligible for destruction.

## WASM_INSTANCE

A single compiled WASM module within a store. Identity is URL + SHA-256.

Lifecycle:
1. **Compile** (engine, bytes, size)
2. **Open** (fabricIx, params) — refcount 0->1 fires Initialize, then Open
3. **Close** (fabricIx) — Close, then refcount 1->0 fires Finalize

## Host Functions — the single-`Call` ABI

Every guest -> host request crosses **one** Wasmtime import:

```
Call (i32 nPacketOffset, i32 nPacketSize) -> i64        module "Sneeze"
```

The guest packs a request into its own linear memory (an 8-byte
`SNEEZE_ABI_PACKET_HEADER` — `wType`, `wMethod`, `dwSize` — followed by a
method-specific payload) and passes its offset and size. `Call` reads the
header, routes on `(wType, wMethod)` to the owning subsystem, and returns an
`i64` (a created object index, a `0/1` status, a boolean, or the byte size an
out-buffer needs). This replaces the former ~30 named imports (`Console.Log`,
`Scene.Node_Root`, …): a module compiled once keeps loading as the engine grows
new methods, because a new method is a new **number**, never a new symbol.

The full contract — the `wType`/`wMethod` registry and every payload's field
layout — lives in **`sdk/include/sneeze_abi.h`**, which the host includes and
every language SDK mirrors.

The host also looks up a small set of guest **exports** for the reverse
direction and for memory handshakes:

| Export | Signature | Used by |
|--------|-----------|---------|
| `Alloc` | `(i32 nSize) -> i32` | host writes into guest memory (Open snapshot, events) |
| `Free` | `(i32 nOffset, i32 nSize)` | release an `Alloc` block |
| `Notify` | `(i32 nPacketOffset, i32 nPacketSize) -> i64` | host -> guest events (first user: `TIMER_FIRED`) |

### Subsystems and methods

Routed today (existing engine bodies, reached through `Call`). `wType` numbers
are the permanent registry from `sneeze_abi.h`:

- **DATA** (`wType` 1) — `Has`/`Get`, read-only reads of the fabric's config
  `Data` tree (the immutable analog of STORAGE, no scope).
- **CONSOLE** (`wType` 2) — `Log`/`Debug`/`Info`/`Warn`/`Error`/`Assert`/
  `Group`/`GroupCollapsed`/`GroupEnd`/`Count`/`CountReset`/`Time`/`TimeEnd`/
  `TimeLog`. Forwarded to the container's `STREAM` (`CONTAINER::Stream()`).
- **STORAGE** (`wType` 3) — `Has`/`Get`/`Set`/`Remove`. Forwarded to the
  container's `SILO` (`CONTAINER::Silo()`). Each call carries a scope selector
  (org/container × permanent/temporary) and a dot-notation path. An **empty
  path** (`""`) addresses the scope's root document (Get returns the whole
  document, Set replaces it, Remove clears it, Has reports the always-present
  root). `Get` returns the **full byte size** of the value (truncation when it
  exceeds the supplied buffer; pass length 0 to query the size).
- **SCENE** (`wType` 6) — `Node_Root`/`Node_Map`/`Node_Open`/`Node_Close`, on
  the container's node tree.
- **NODE** (`wType` 8) — `Position`/`Scale`/`Scale_Axes`/`Bound`/`Name`/
  `Resource`/`Panel`, mutating a live `MAP_OBJECT` found by object index.
- **CHRONO** (`wType` 9) — `Time`/`Date`/`Now`/`Moment`/`Set`/`Parse`/`Format`.
  The wall clock and all civil (calendar) logic; the host fills a
  `SNEEZE_ABI_MOMENT` the guest caches and reads locally. Global — needs neither
  the store nor the container.
- **PERFORMANCE** (`wType` 10) — `Now`/`Origin`. Monotonic high-resolution clock
  (100 ns since a fixed process origin). Global.
- **TIMER** (`wType` 11) — `Set`/`Clear` (guest -> host). Arms/disarms entries on
  the engine timer service (see below); `TIMER_FIRED` is the reverse `Notify`.

Registered numbers reserved but **not yet implemented** (they fall through to a
`0` result until their host bodies land): **NETWORK** (`wType` 4, `Fetch`),
**VIEWPORT** (`wType` 5, camera get/set), and the SCENE globals
(`Ambient`/`Directional`/`Background`) and `NODE.Rotation`.

The `Call` callback receives the store pointer as its env, giving it the calling
container (one store per container; the packet's `twFabricIx` selects the fabric
within it) for storage scoping and — later — access control.

String/byte I/O helpers move data across the WASM boundary:

- `ReadWasmString()` / `ReadWasmBytes()` — copy data out of WASM linear memory.
- `WriteWasmString()` — copy a UTF-8 string into WASM linear memory and return
  the full size the string requires (so callers can detect truncation and
  re-query with a larger buffer).
- `WriteWasmBytes()` — copy a raw struct (a filled `SNEEZE_ABI_MOMENT`) into
  WASM linear memory; same size/truncation contract as `WriteWasmString`.

## TIMER service (`WASM_TIMERS`) and the Notify path

`WASM_TIMERS` is the one engine-wide timer service, owned by `WASM_RUNTIME` and
reached via `Wasm_Runtime()->Timers()`. It is the only home for timer state:
the queue, the due math, and the store-teardown drain. The control module owns
only the metronome cadence.

**Arming (guest thread, inside a `Call`).** `Dispatch_Timer` routes
`TIMER_SET` to `Arm(store, twFabricIx, eUnit, nValue, qwParam, bRepeat)` and
`TIMER_CLEAR` to `Clear(store, twTimerIx)`. `eUnit` is `TICK` (1/64 s),
`MS`, or `HZ` (period = 1/`nValue` s). `Arm` returns a nonzero `twTimerIx`
(`0` on an invalid unit/value); the entry is keyed by **(store, id)** — the
store, not one instance, is the timer's home.

**Firing (TIMER agent pool).** The Control metronome signals the TIMER pool once
per wake (~1000 Hz); its agents drive `Claim` -> `WASM_STORE::Notify_Timer` ->
`Complete`. `Claim` hands each agent a distinct due, in-flight entry (parallel
across stores). `Notify_Timer` builds the `TIMER_FIRED` packet (header +
`twFabricIx`, `twTimerIx`, `qwParam`) and delivers it to every active instance
in the store via `WASM_INSTANCE::Notify_Guest` (the `Alloc`/write/`Notify`/`Free`
handshake, mirroring the Open snapshot push) — all under the store lock, so a
timer fire never enters a store's wasmtime context alongside an
`Instance_Open`/`Close`. `Complete` reschedules a repeat one period on (clamped
so a slow fire never bursts) or drops a one-shot.

**Known limitation — head-of-line blocking on a hot store (perf only, deferred).**
`Claim` picks the earliest-due entry *regardless of store*, and an agent marks its
entry in-flight before it calls `Notify_Timer`. So if two due timers belong to the
same store, two agents can claim them, and the second parks on that store's lock —
committed to its claimed fire, unable to help elsewhere — until the first delivery
finishes. With the 4-agent pool that is ~25% of timer-drain capacity lost per
blocked agent (up to ~75% if three pile on one store). It is transient (clears when
`Complete` runs), same-store only (different stores never contend), and never a
correctness bug or a stall. Planned fix: give `Claim` per-store single-flight — skip
any store that already has an in-flight entry — so a second agent finds other work
instead of blocking. No lock-side change; purely a smarter `Claim`.

**Teardown.** `WASM_RUNTIME::Store_Close` calls `WASM_TIMERS::Store_Close(store)`
before deleting the store: it cancels the store's entries and blocks until any
in-flight fire finishes, so no timer agent can touch a freed store. (At engine
shutdown the TIMER agents are already joined — CONTROL is destroyed before
WASM_RUNTIME — so leftover stores are dropped without a drain.)

**Testing.** The `--chrono` suite (`tests/ChronoTest.cpp`) exercises the host
side directly, without a WASM store: the `Chrono.*` clock/civil fills behind
CHRONO/PERFORMANCE, and the `WASM_TIMERS` scheduler (arm/claim/complete/clear
one-shot and repeat across TICK/MS/HZ, plus the store-close drain) using opaque
store keys. The end-to-end guest round-trip through `Notify_Guest` is covered
by a guest module rather than this host suite.

## Dependencies

- **Wasmtime** v43.0.0 — C API for WASM compilation and execution.

## Files

| File | Contents |
|------|----------|
| `Wasm.h` | WASM_RUNTIME, WASM_TIMERS, WASM_STORE, WASM_INSTANCE declarations |
| `Wasm_Runtime.cpp` | WASM_RUNTIME implementation (owns the timer service) |
| `Wasm_Timers.cpp` | WASM_TIMERS — timer queue, Arm/Clear, Claim/Complete, store-close drain |
| `Wasm_Store.cpp` | WASM_STORE implementation (incl. `Notify_Timer`) |
| `Wasm_Instance.cpp` | WASM_INSTANCE implementation (incl. `Notify_Guest`) |
| `Chrono.h/.cpp` | Host wall/monotonic clocks + civil logic backing CHRONO/PERFORMANCE |
| `HostFunctions.h` | The `Call` entry point + `ReadWasmString` declarations |
| `HostFunctions.cpp` | `Call` dispatcher + per-subsystem routing to engine bodies |
| `sneeze_abi.h` (SneezeSDK) | Canonical ABI contract (shared with guest SDKs) |
