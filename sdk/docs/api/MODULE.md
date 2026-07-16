# MODULE

A declared WASM module from the fabric manifest. Reached through [`FABRIC::Modules`](FABRIC.md#snapshot-views), which returns a `&[MODULE]` read from the [Open snapshot](SNAPSHOT.md). It mirrors the engine's `MSF::MODULE`.

This is a manifest *record* describing a module the fabric loads (its URL and integrity hash). It is not the lifecycle trait your code implements - that is [`INSTANCE`](INSTANCE.md). The two were deliberately given different names to avoid a collision.

## Methods

| Method | Returns | Meaning |
|--------|---------|---------|
| `Url ()` | `&str` | The module's URL, absolute or relative to the fabric. |
| `Hash ()` | `&str` | The SRI integrity hash (e.g. `sha256-96d4c0b6...`), empty if unpinned. |

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pConsole = pFabric.Console ();

   for pModule in pFabric.Modules ()
   {
      pConsole.Log (pModule.Url ());

      if pModule.Hash ().is_empty ()
      {
         pConsole.Warn ("module is not pinned with a hash");
      }
   }
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the list.
- [SERVICE](SERVICE.md) - services reference these modules by name.
- [INSTANCE](INSTANCE.md) - the lifecycle trait (not to be confused with this manifest record).
