# FABRIC

The root handle for one open fabric. You receive a `FABRIC` at [`Open`](INSTANCE.md#open); every subsystem you can use hangs off it. It is a tiny copyable value (just the fabric index in a typed wrapper), so pass it around freely.

The subsystem accessors (`Console`, `Storage`, `Data`, `Scene`) return **zero-cost views**: each wraps the same fabric index, allocates nothing, and knows which fabric it routes to. There is one of each subsystem per fabric.

## Methods

### Index

```rust
pub fn Index (&self) -> u64
```

- **Parameters:** none.
- **Returns:** the fabric's index (its handle value).
- **Description:** The stable identity of this fabric for the life of the open. Because one WASM instance can hold several fabrics open at once, use this to key any per-fabric state you retain across `Open`/`Close`.
- **Example:**

```rust
let nKey = pFabric.Index ();
```

- **See also:** [`Open`](INSTANCE.md#open), [`Close`](INSTANCE.md#close).

### Console

```rust
pub fn Console (&self) -> CONSOLE
```

- **Parameters:** none.
- **Returns:** the [`CONSOLE`](CONSOLE.md) view for this fabric.
- **Description:** Developer-console logging, forwarded to the container's log stream.
- **Example:**

```rust
pFabric.Console ().Log ("hello");
```

- **See also:** [`CONSOLE`](CONSOLE.md).

### Storage

```rust
pub fn Storage (&self) -> STORAGE
```

- **Parameters:** none.
- **Returns:** the [`STORAGE`](STORAGE.md) view for this fabric.
- **Description:** Persistent JSON document storage, scoped per persona and organization. Use it to save and restore module state across sessions.
- **Example:**

```rust
let bHas = pFabric.Storage ().Has (eSNEEZE_ABI_SILO_SCOPE::kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER, "settings");
```

- **See also:** [`STORAGE`](STORAGE.md).

### Data

```rust
pub fn Data (&self) -> DATA
```

- **Parameters:** none.
- **Returns:** the [`DATA`](DATA.md) view for this fabric.
- **Description:** The fabric's read-only configuration `Data` tree, served on demand by path. The immutable analog of storage.
- **Example:**

```rust
let sTitle = pFabric.Data ().Get ("Scene.sTitle");
```

- **See also:** [`DATA`](DATA.md).

### Scene

```rust
pub fn Scene (&self) -> SCENE
```

- **Parameters:** none.
- **Returns:** the [`SCENE`](SCENE.md) view for this fabric.
- **Description:** Node-tree construction on the fabric's container - create the root, add children, remove nodes.
- **Example:**

```rust
let mut pRoot = SNEEZE_ABI_MAPOBJECT::Root ();
let pNode = pFabric.Scene ().Node_Root (&pRoot);
```

- **See also:** [`SCENE`](SCENE.md), [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md).

## Snapshot views

Beyond the live subsystems above, `FABRIC` also exposes the fabric's *immutable* configuration - the [Open snapshot](SNAPSHOT.md) the engine pushed at `Open`. The SDK parses that snapshot once, privately; a module never sees the raw JSON. Each accessor below returns a read-only, typed view of one section. `Resource`/`Signature`/`Agent`/`Container` and the `Services`/`Modules` lists borrow directly from the parsed snapshot (no copy, no boundary crossing); `Location` is computed on demand from the resource reference.

| Accessor | Returns | Page |
|----------|---------|------|
| `Location ()` | [`LOCATION`](LOCATION.md) | the fabric URL, split into parts (a `window.location` analog) |
| `Resource ()` | `&RESOURCE` | [RESOURCE](RESOURCE.md) - the launching resource's identity |
| `Container ()` | `&CONTAINER` | [CONTAINER](CONTAINER.md) - container identity and trust |
| `Signature ()` | `&SIGNATURE` | [SIGNATURE](SIGNATURE.md) - MSF verification detail |
| `Agent ()` | `&AGENT` | [AGENT](AGENT.md) - host/engine identity (a `navigator` analog) |
| `Services ()` | `&[SERVICE]` | [SERVICE](SERVICE.md) - declared services |
| `Modules ()` | `&[MODULE]` | [MODULE](MODULE.md) - declared WASM modules |

```rust
pFabric.Console ().Log (pFabric.Container ().Name ());
pFabric.Console ().Log (pFabric.Location ().Host ());

for pModule in pFabric.Modules ()
{
   pFabric.Console ().Log (pModule.Url ());
}
```

## See also

- [API overview](overview.md) - how `FABRIC` roots the whole object model.
- [the Open snapshot](SNAPSHOT.md) - the immutable configuration behind the snapshot views.
