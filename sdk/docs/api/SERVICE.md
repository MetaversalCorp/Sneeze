# SERVICE

A declared service from the fabric manifest. Reached through [`FABRIC::Services`](FABRIC.md#snapshot-views), which returns a `&[SERVICE]` read from the [Open snapshot](SNAPSHOT.md). It mirrors the engine's `MSF::SERVICE`.

A service is a discrete unit of functionality declared by the fabric; it names the WASM modules that implement it.

## Methods

| Method | Returns | Meaning |
|--------|---------|---------|
| `Name ()` | `&str` | The service name. |
| `Type ()` | `&str` | The service type. |
| `Endpoint ()` | `&str` | The service endpoint. |
| `Modules ()` | `&[String]` | Names of the modules this service uses. |

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pConsole = pFabric.Console ();

   for pService in pFabric.Services ()
   {
      pConsole.Log (&format! ("{} ({})", pService.Name (), pService.Type ()));

      for sModule in pService.Modules ()
      {
         pConsole.Log (sModule);
      }
   }
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the list.
- [MODULE](MODULE.md) - the declared WASM modules a service references.
