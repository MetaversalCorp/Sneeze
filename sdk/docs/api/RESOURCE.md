# RESOURCE

The launching resource's identity - the map object of the node that attached this fabric. Reached through [`FABRIC::Resource`](FABRIC.md#snapshot-views), a read-only view over the [Open snapshot](SNAPSHOT.md).

For the primary fabric there is no attaching node, so `Id ()` is `0` and `Name ()` is empty. The fabric's own URL is not on `RESOURCE`; read it from [`FABRIC::Location`](LOCATION.md).

## Methods

### Id

```rust
pub fn Id (&self) -> u64
```

- **Returns:** the resource id (the host's `MAP_OBJECT_RESOURCE::qwResource`). `0` for the primary fabric.
- **Description:** The id is a `u64` on the host - beyond JSON's safe-integer range - so it crosses the boundary as a decimal string and this accessor parses it back to a `u64` for you.

### Name

```rust
pub fn Name (&self) -> &str
```

- **Returns:** the launching resource's name (`MAP_OBJECT_RESOURCE::sName`). Empty for the primary fabric.

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pResource = pFabric.Resource ();

   if pResource.Id () != 0
   {
      pFabric.Console ().Log (pResource.Name ());
   }

   // the fabric's own URL lives on LOCATION, not RESOURCE
   pFabric.Console ().Log (pFabric.Location ().Href ());
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the view.
- [LOCATION](LOCATION.md) - the fabric URL, split into parts.
- [SNEEZE_ABI_MAPOBJECT](MAPOBJECT.md) - the builder whose resource fields this mirrors.
