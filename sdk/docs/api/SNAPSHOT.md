# The Open Snapshot

When a fabric opens your module, the engine hands it an **immutable snapshot**: a JSON document, synthesized for that one fabric, describing everything fixed about it. This is a *concept*, not a type you handle - the SDK reads it for you and exposes it through the [`FABRIC`](FABRIC.md) handle. This page explains where the data comes from and how you reach it.

## What is in it

The snapshot carries fixed-shape sections, each surfaced as a typed view off the fabric:

| Section | Reached through | Page |
|---------|-----------------|------|
| The launching resource | `pFabric.Resource ()` | [RESOURCE](RESOURCE.md) |
| The fabric URL (split into parts) | `pFabric.Location ()` | [LOCATION](LOCATION.md) |
| Container identity and trust | `pFabric.Container ()` | [CONTAINER](CONTAINER.md) |
| MSF signature-verification result | `pFabric.Signature ()` | [SIGNATURE](SIGNATURE.md) |
| Host / engine identity | `pFabric.Agent ()` | [AGENT](AGENT.md) |
| Declared services | `pFabric.Services ()` | [SERVICE](SERVICE.md) |
| Declared WASM modules | `pFabric.Modules ()` | [MODULE](MODULE.md) |

The fabric's open-ended configuration `Data` block is **not** in the snapshot. It is served on demand, read-only, through [`DATA`](DATA.md), so a large data block never inflates the snapshot.

## How it reaches you

Because the snapshot is arbitrary-length data, the engine cannot pass it as a call argument - it places it into your linear memory first, through the ABI's `Alloc`/`Open`/`Free` handshake (see [Incorporating the ABI](../incorporating-the-abi.md#the-open-handshake)):

1. The engine calls your `Alloc` export to reserve a block and copies the JSON into it.
2. It calls `Open`, passing the block's offset and size.
3. The SDK parses those bytes **once**, into a private owned record, before your `INSTANCE::Open` runs.
4. When `Open` returns, the engine frees the block.

The raw bytes are transient - valid only during that handshake - which is exactly why the SDK copies them into an owned record up front. You never see the offset, the size, or the JSON text: you read the parsed data through the fabric's typed views for the life of the module.

```rust
fn Open (pFabric: FABRIC)
{
   pFabric.Console ().Log (pFabric.Container ().Name ());
   pFabric.Console ().Log (pFabric.Location ().Host ());

   for pModule in pFabric.Modules ()
   {
      pFabric.Console ().Log (pModule.Url ());
   }
}
```

Every field is defaulted, so a missing, partial, or rearranged snapshot yields empty values rather than a failure - the views never error.

## See also

- [FABRIC](FABRIC.md) - the handle every snapshot view hangs off.
- [INSTANCE::Open](INSTANCE.md#open) - where the snapshot arrives.
- [Incorporating the ABI](../incorporating-the-abi.md#the-open-handshake) - the raw `Alloc`/`Open`/`Free` push handshake beneath this.
