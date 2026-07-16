# AGENT

Host and engine identity - a `navigator` analog. Reached through [`FABRIC::Agent`](FABRIC.md#snapshot-views), a read-only view over the [Open snapshot](SNAPSHOT.md). It tells a module what browser/engine and platform it is running under, so it can adapt.

## Methods

| Method | Returns | Meaning |
|--------|---------|---------|
| `Browser_Name ()` | `&str` | The host application name. |
| `Browser_Version ()` | `&str` | The host application version. |
| `Engine_Name ()` | `&str` | The engine (MBE) name (e.g. `Sneeze`). |
| `Engine_Version ()` | `&str` | The engine version. |
| `Platform ()` | `&str` | The host platform/OS. |
| `Language ()` | `&str` | The active locale (e.g. `en-US`). |

> Note: the host does not yet supply the browser/platform/locale values, so `Browser_Name ()`/`Browser_Version ()`/`Platform ()` currently carry placeholder values and the engine name/version are the engine's own. Treat them as best-effort until the host wires in the real values.

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pAgent = pFabric.Agent ();

   pFabric.Console ().Log (&format! ("{} {}", pAgent.Engine_Name (), pAgent.Engine_Version ()));
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the view.
