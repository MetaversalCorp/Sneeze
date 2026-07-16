# WASM — WebAssembly Sandbox

The `wasm` module provides the sandboxed execution environment for WebAssembly
modules loaded from MSF fabric payloads. It manages the Wasmtime engine,
isolated stores, compiled instances, and host function bindings.

## Architecture

```
WASM_RUNTIME (owns wasm_engine_t)
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

The host also looks up (but does not yet call) a small set of guest **exports**
for the reverse direction and for memory handshakes:

| Export | Signature | Used by |
|--------|-----------|---------|
| `Alloc` | `(i32 nSize) -> i32` | host writes into guest memory (Open snapshot, events) |
| `Free` | `(i32 nOffset, i32 nSize)` | release an `Alloc` block |
| `Notify` | `(i32 nPacketOffset, i32 nPacketSize) -> i64` | host -> guest events |

### Subsystems and methods

Routed today (existing engine bodies, reached through `Call`):

- **CONSOLE** (`wType` 1) — `Log`/`Debug`/`Info`/`Warn`/`Error`/`Assert`/
  `Group`/`GroupCollapsed`/`GroupEnd`/`Count`/`CountReset`/`Time`/`TimeEnd`/
  `TimeLog`. Forwarded to the container's `STREAM` (`CONTAINER::Stream()`).
- **STORAGE** (`wType` 2) — `Has`/`Get`/`Set`/`Remove`. Forwarded to the
  container's `SILO` (`CONTAINER::Silo()`). Each call carries a scope selector
  (org/container × permanent/temporary) and a dot-notation path. An **empty
  path** (`""`) addresses the scope's root document (Get returns the whole
  document, Set replaces it, Remove clears it, Has reports the always-present
  root). `Get` returns the **full byte size** of the value (truncation when it
  exceeds the supplied buffer; pass length 0 to query the size).
- **SCENE** (`wType` 5) — `Node_Root`/`Node_Map`/`Node_Open`/`Node_Close`, on
  the container's node tree.
- **NODE** (`wType` 6) — `Position`/`Scale`/`Scale_Axes`/`Bound`/`Name`/
  `Resource`/`Panel`, mutating a live `MAP_OBJECT` found by object index.

Registered numbers reserved but **not yet implemented** (they fall through to a
`0` result until their host bodies land): **NETWORK** (`wType` 3, `Fetch`),
**VIEWPORT** (`wType` 4, camera get/set), and the SCENE globals
(`Ambient`/`Directional`/`Background`) and `NODE.Rotation`.

The `Call` callback receives the store pointer as its env, giving it the calling
container (one store per container; the packet's `twFabricIx` selects the fabric
within it) for storage scoping and — later — access control.

String/byte I/O helpers move data across the WASM boundary:

- `ReadWasmString()` / `ReadWasmBytes()` — copy data out of WASM linear memory.
- `WriteWasmString()` — copy a UTF-8 string into WASM linear memory and return
  the full size the string requires (so callers can detect truncation and
  re-query with a larger buffer).

## Dependencies

- **Wasmtime** v43.0.0 — C API for WASM compilation and execution.

## Files

| File | Contents |
|------|----------|
| `Wasm.h` | WASM_RUNTIME, WASM_STORE, WASM_INSTANCE declarations |
| `Wasm_Runtime.cpp` | WASM_RUNTIME implementation |
| `Wasm_Store.cpp` | WASM_STORE implementation |
| `Wasm_Instance.cpp` | WASM_INSTANCE implementation |
| `HostFunctions.h` | The `Call` entry point + `ReadWasmString` declarations |
| `HostFunctions.cpp` | `Call` dispatcher + per-subsystem routing to engine bodies |
| `../../../sdk/include/sneeze_abi.h` | Canonical ABI contract (shared with guest SDKs) |
| `ThreadPool.h/cpp` | Fixed-size worker pool for parallel WASM execution |
