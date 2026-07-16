# SIGNATURE

The MSF signature-verification result for the fabric. Reached through [`FABRIC::Signature`](FABRIC.md#snapshot-views), a read-only view over the [Open snapshot](SNAPSHOT.md).

Where [`CONTAINER`](CONTAINER.md)`::Trust ()` gives the single overall trust level, `SIGNATURE` gives the underlying detail that produced it.

## Methods

| Method | Returns | Meaning |
|--------|---------|---------|
| `Algorithm ()` | `&str` | The signing algorithm (e.g. `RS256`). |
| `IsValid ()` | `bool` | The signature verified against the payload. |
| `IsChainTrusted ()` | `bool` | The certificate chain is trusted. |
| `IsChainExpired ()` | `bool` | The certificate chain has expired. |

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pSig = pFabric.Signature ();

   if pSig.IsValid ()  &&  pSig.IsChainTrusted ()  &&  !pSig.IsChainExpired ()
   {
      pFabric.Console ().Info ("fully verified");
   }
   else
   {
      pFabric.Console ().Warn (pSig.Algorithm ());
   }
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the view.
- [CONTAINER](CONTAINER.md) - the summarized trust level.
