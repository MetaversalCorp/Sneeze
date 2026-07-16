# Incorporating the ABI into a WASM Module

A Sneeze module is a WebAssembly binary that the engine loads at runtime. The engine and your module communicate through the **Sneeze ABI**, defined once, for every language, in the canonical C header `sdk/include/sneeze_abi.h`. This page explains what that contract is and how to satisfy it in each language.

You normally do not implement the ABI by hand. A per-language SDK does it for you and exposes typed objects instead. Read the [API overview](api/overview.md) for those objects. This page is for (a) setting up a project and (b) authors porting the SDK to a new language.

## The contract in one screen

WebAssembly links imports and exports by name, so the ABI is deliberately tiny and effectively frozen. It never grows new named functions as the engine gains features - new features become new *numbers* inside one routed call.

**One import** the engine provides (module name `"Sneeze"`):

| Symbol | Signature | Direction | Purpose |
|--------|-----------|-----------|---------|
| `Call` | `(i32 nOffset, i32 nSize) -> i64` | guest -> host | Every request. The guest packs a self-describing packet into its own memory and passes its `(offset, size)`. |

**Seven exports** your module provides:

| Symbol | Signature | Purpose |
|--------|-----------|---------|
| `Alloc` | `(i32 nSize) -> i32 nOffset` | Host asks the guest to reserve memory, then writes bytes into it. |
| `Free` | `(i32 nOffset, i32 nSize)` | Release a block from `Alloc`. |
| `Notify` | `(i32 nOffset, i32 nSize) -> i64` | Host -> guest event delivery. Inert until node events land (a later roadmap item). |
| `Init` | `()` | Module loaded. |
| `Open` | `(i64 twFabricIx, i32 nOffset, i32 nSize)` | A fabric opened. `twFabricIx` is the fabric handle; the immutable snapshot blob is at `(nOffset, nSize)` in your memory. |
| `Close` | `(i64 twFabricIx)` | A fabric closed. |
| `Shutdown` | `()` | Module unloading. |

That is the entire surface. Everything else - console, storage, scene building - travels *through* `Call` as data.

## The packet format

Every buffer passed to `Call` (and every buffer delivered through `Notify`) begins with an 8-byte little-endian header, followed by the method's payload:

| Field | Type | Meaning |
|-------|------|---------|
| `wType` | `u16` | Subsystem id (1 `CONSOLE`, 2 `STORAGE`, 3 `NETWORK`, 4 `VIEWPORT`, 5 `SCENE`, 6 `NODE`, 7 `DATA`). |
| `wMethod` | `u16` | Method id within that subsystem. |
| `dwSize` | `u32` | Payload byte count that follows the header. |

The payload is a sequence of little-endian scalar fields. Strings and binary blobs are not copied into the packet; instead the guest writes an `(i32 offset, i32 length)` pair that points into its own linear memory, and the referenced buffer must stay alive across the synchronous `Call`. The host reads the header, routes on `(wType, wMethod)`, reads the payload, and returns an `i64`.

The `i64` return carries whatever the method produces: a created object index, a 0/1 status, a boolean, or - for block getters - the full byte size the result needs (so the guest can size a buffer and re-read). The full per-method field layouts are documented in `sneeze_abi.h` and in each subsystem's API page.

### Why numbers, not symbols

Method ids are **permanent, monotonic, and append-only**. A revised method takes the next free number; an old number is never reused or removed. That is the backward-compatibility guarantee: a module compiled today keeps loading years from now, because the engine still answers the numbers it sent. `SNEEZE_ABI_VERSION` bumps only for a change to the framing itself, never for a new method.

## The Open handshake

Because the snapshot is arbitrary-length data, the engine cannot "pass" it as an argument - it must place it in your memory first:

1. The engine calls your `Alloc (nSize)` export to reserve `nSize` bytes and get back an offset into your linear memory.
2. The engine copies the snapshot blob into your memory at that offset.
3. The engine calls your `Open (twFabricIx, nOffset, nSize)`.
4. When `Open` returns, the engine calls your `Free (nOffset, nSize)`.

The snapshot bytes are therefore valid **only for the duration of `Open`**. Copy out anything you need to keep (the SDK's parse step does exactly this, before your `Open` runs). See [The Open snapshot](api/SNAPSHOT.md).

## Setting up a project

### Rust (the shipped SDK)

Your module is a `cdylib` targeting `wasm32-unknown-unknown`, depending on the `sneeze` crate.

`Cargo.toml`:

```toml
[package]
name = "my_module"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
sneeze = { path = "../../../sdk/rust" }   # path to the SDK crate

[profile.release]
opt-level = "s"
lto = true
```

`src/lib.rs`:

```rust
use sneeze::*;

struct MY_MODULE;

impl INSTANCE for MY_MODULE
{
   fn Open (pFabric: FABRIC)
   {
      pFabric.Console ().Log ("hello from wasm");

      let mut pRoot = SNEEZE_ABI_MAPOBJECT::Physical ();
      pRoot.Name ("Stool").Reference ("assets/Stool.glb");
      pFabric.Scene ().Node_Root (&pRoot);
   }
}

sneeze::instance! (MY_MODULE);
```

Build:

```bash
cargo build --target wasm32-unknown-unknown --release
```

The `instance!` macro emits all seven exports for you and routes them to your `INSTANCE`. You never write `Alloc`/`Free`/`Open`/etc. by hand. See [INSTANCE](api/INSTANCE.md).

### Any other language

There is no SDK yet for languages other than Rust, so until one exists you implement the ABI directly. The recipe is the same everywhere; only the syntax differs. The tiers in which the SDK is expected to fan out are, roughly: Rust / C-C++ / Zig first; Go-TinyGo / AssemblyScript / C#(.NET) next; Python / JS-TS last.

To bring up a new language you must:

1. **Produce a WASM binary** with the toolchain's WASM target and export the seven symbols above with exactly those names and signatures.
2. **Declare the `Sneeze.Call` import** with its signature.
3. **Mirror `sneeze_abi.h`** - the `wType`/`wMethod` numbers, the shared enums (`eSNEEZE_ABI_SILO_SCOPE`, `eSNEEZE_ABI_TRUST`, `eSNEEZE_ABI_MAP_OBJECT_CLASS`), and the OBJECTIX helpers - as constants in your language. Keep the names identical.
4. **Write a packet builder** - append the 8-byte header, append little-endian fields, patch `dwSize`, call `Call`. (See the Rust `PACKET` type in `sdk/rust/src/ffi.rs` for a compact model.)
5. **Reproduce the `SNEEZE_ABI_MAPOBJECT` struct** exactly - it is a 528-byte `#[repr(C, packed)]` binary layout that crosses the boundary as raw bytes, not a serialized packet. The C definition in `sneeze_abi.h` is normative; a static assert guards its size.
6. **Parse the snapshot** - the blob is UTF-8 JSON; pick any JSON facility in your language to read it (the Rust SDK uses `nanoserde` internally, but this is an implementation choice, not part of the ABI).

Because the ABI is frozen and self-describing, a correct port in any language interoperates with the same engine binary with no host-side changes.

## See also

- [API overview](api/overview.md) - the object model the SDK exposes.
- `sdk/include/sneeze_abi.h` - the normative contract (types, enums, method numbers, per-method payload layouts).
- `sdk/rust/src/ffi.rs` - the reference packet builder and `Call` import.
