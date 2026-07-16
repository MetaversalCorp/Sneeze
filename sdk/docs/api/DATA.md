# DATA

The fabric's read-only configuration `Data` tree, reached through [`FABRIC::Data`](FABRIC.md#data). This is the open-ended `Data` block the fabric author wrote in the manifest - scene descriptions, parameters, whatever the module needs. `DATA` is a zero-cost view over the fabric handle.

`DATA` is the **immutable analog of [`STORAGE`](STORAGE.md)**: the same path-addressed, JSON-text model, but read-only (`Has` and `Get` only, no `Set` or `Remove`) and with no scope, because the data belongs to the one fabric.

The data block is deliberately *not* included in the [`Open`](INSTANCE.md#open) snapshot. It is served on demand, so a large data block never inflates the snapshot blob. It stays available for the whole life of the fabric, not just during `Open`.

## Paths

A path addresses a value inside the data document using dotted segments (`"Scene.sTitle"`). An **empty path** (`""`) addresses the whole document.

## Methods

### Has

```rust
pub fn Has (&self, sPath: &str) -> bool
```

- **Parameters:** `sPath` - the value's path (`""` = the whole document).
- **Returns:** `true` if a value exists at `sPath`.
- **Description:** Tests for the presence of a value in the data tree without reading it.
- **Example:**

```rust
if pFabric.Data ().Has ("Scene.aLight")
{
   // the author supplied lights
}
```

- **See also:** [`Get`](#get), [`STORAGE::Has`](STORAGE.md#has).

### Get

```rust
pub fn Get (&self, sPath: &str) -> Option<String>
```

- **Parameters:** `sPath` - the value's path (`""` = the whole document).
- **Returns:** `Some(json)` with the value as JSON text, or `None` if missing or null.
- **Description:** Reads the value at `sPath` as a JSON string. Sizes the result buffer in one probe and, if needed, one exact re-read, so large values return correctly. Parse the returned string with your JSON library of choice.
- **Example:**

```rust
if let Some (sTitle) = pFabric.Data ().Get ("Scene.sTitle")
{
   pFabric.Console ().Log (&sTitle);
}
```

- **See also:** [`Has`](#has), [`STORAGE::Get`](STORAGE.md#get), [`SCENE::Node_Map`](SCENE.md#node_map) (build a node tree straight from a data path).

## See also

- [STORAGE](STORAGE.md) - the mutable, scoped counterpart with the same shape.
- [SCENE::Node_Map](SCENE.md#node_map) - lets the engine read a `Data` subtree and build nodes from it for you.
