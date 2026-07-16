# INSTANCE

The lifecycle trait your module implements. The engine drives a running WASM instance through four callbacks; you provide whichever you need and register the type with the [`instance!`](#the-instance-macro) macro, which generates the raw ABI exports and forwards to your implementation.

Every method has a default empty body, so you override only what you use.

The name mirrors the engine's own term for a loaded module: the engine calls it a `WASM_INSTANCE`. (A *declared* module in a fabric manifest is a separate thing, the [`MODULE`](MODULE.md) record.)

## Methods

### Init

```rust
fn Init () {}
```

- **Parameters:** none.
- **Returns:** nothing.
- **Description:** Called once, when the module is first loaded, before any fabric opens. Use it for one-time global setup that is not tied to a specific fabric. Most modules leave it as the default.
- **Example:**

```rust
impl INSTANCE for MY_MODULE
{
   fn Init ()
   {
      // one-time setup, no fabric yet
   }
}
```

- **See also:** [`Shutdown`](#shutdown) (its mirror), [`Open`](#open).

### Open

```rust
fn Open (pFabric: FABRIC) {}
```

- **Parameters:**
  - `pFabric` - the [`FABRIC`](FABRIC.md) root handle for the fabric that just opened. Every subsystem *and* the immutable configuration is reached through it.
- **Returns:** nothing.
- **Description:** The main entry point. Called once per fabric that loads your module. This is where you read configuration, build the scene, and store or restore state. Because one instance can serve several fabrics at once, key any per-fabric state you retain by `pFabric.Index ()`. The engine pushes an immutable [Open snapshot](SNAPSHOT.md) at this point; the SDK parses it privately, so you read it through the fabric's typed views (`pFabric.Resource ()`, `pFabric.Container ()`, ...) rather than touching any raw blob.
- **Example:**

```rust
fn Open (pFabric: FABRIC)
{
   pFabric.Console ().Log (pFabric.Container ().Name ());

   let mut pRoot = SNEEZE_ABI_MAPOBJECT::Physical ();
   pRoot.Name ("Stool").Reference ("assets/Stool.glb");
   pFabric.Scene ().Node_Root (&pRoot);
}
```

- **See also:** [`Close`](#close) (its mirror), [`FABRIC`](FABRIC.md), [the Open snapshot](SNAPSHOT.md).

### Close

```rust
fn Close (pFabric: FABRIC) {}
```

- **Parameters:**
  - `pFabric` - the [`FABRIC`](FABRIC.md) handle for the fabric being closed (the same index you saw in `Open`).
- **Returns:** nothing.
- **Description:** Called when a fabric unloads. Release any per-fabric state you keyed by `pFabric.Index ()`. The engine tears down the fabric's nodes itself; you only clean up your own guest-side bookkeeping.
- **Example:**

```rust
fn Close (pFabric: FABRIC)
{
   // drop any state keyed by pFabric.Index ()
}
```

- **See also:** [`Open`](#open) (its mirror).

### Shutdown

```rust
fn Shutdown () {}
```

- **Parameters:** none.
- **Returns:** nothing.
- **Description:** Called once, when the module itself is unloading, after all fabrics have closed. The mirror of `Init`; use it for one-time global teardown.
- **Example:**

```rust
fn Shutdown ()
{
   // one-time teardown
}
```

- **See also:** [`Init`](#init) (its mirror).

## The instance! macro

```rust
sneeze::instance! (MY_MODULE);
```

Place this once, at module scope, passing the type that implements `INSTANCE`. It generates the seven raw ABI exports the engine looks up by name - `Init`, `Open`, `Close`, `Shutdown`, `Alloc`, `Free`, `Notify` - and routes each to your implementation (or to the SDK's own memory management, for `Alloc`/`Free`, and to the currently-inert `Notify`). You never write those exports yourself.

- **Description:** Without this macro the engine cannot find your module's entry points, because it resolves them as named WASM exports. The macro is the one required piece of boilerplate.
- **Example:**

```rust
use sneeze::*;

struct MY_MODULE;

impl INSTANCE for MY_MODULE
{
   fn Open (pFabric: FABRIC)
   {
      pFabric.Console ().Log ("loaded");
   }
}

sneeze::instance! (MY_MODULE);
```

- **See also:** [Incorporating the ABI](../incorporating-the-abi.md) (the raw exports the macro emits), [API overview](overview.md).
