# NODE

A live scene object, returned by the [`SCENE`](SCENE.md) create calls (`Node_Root`, `Node_Open`, `Node_Map`). Where [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md) is the *template* used to create a node, `NODE` is the *live* object afterward: you mutate its properties through this handle. It carries the node's composed identity (class + object index) and routes mutations by it.

## Local reads (no host call)

These read the handle's own composed identity and do not cross to the host.

### Composed

```rust
pub fn Composed (&self) -> u64
```

- **Parameters:** none.
- **Returns:** the node's composed identity - the class in the high 16 bits, the object index in the low 48 bits, packed into one qword.
- **Description:** The node's complete, globally-unique identity. Useful for logging or comparing nodes.
- **See also:** [`Class`](#class), [`ObjectIx`](#objectix).

### Class

```rust
pub fn Class (&self) -> u16
```

- **Parameters:** none.
- **Returns:** the node's `MAP_OBJECT_CLASS` (e.g. 73 = physical).
- **Description:** The class stamped on the node when it was created, extracted from the high bits of the composed identity.
- **See also:** [`Composed`](#composed), [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md#class-factories).

### ObjectIx

```rust
pub fn ObjectIx (&self) -> u64
```

- **Parameters:** none.
- **Returns:** the node's object index (the low 48 bits of the composed identity).
- **Description:** The class-less object index portion of the identity.
- **See also:** [`Composed`](#composed).

### IsValid

```rust
pub fn IsValid (&self) -> bool
```

- **Parameters:** none.
- **Returns:** `true` unless the creating call failed.
- **Description:** A node is invalid if the create call returned the error sentinel or zero. Check this after `Node_Root` / `Node_Open` / `Node_Map` before using the node.
- **Example:**

```rust
let pNode = pFabric.Scene ().Node_Root (&pRoot);
if !pNode.IsValid ()
{
   pFabric.Console ().Error ("root create failed");
}
```

- **See also:** [`SCENE::Node_Root`](SCENE.md#node_root).

## Property mutators (host calls)

Each of these sends one update to the live node.

### Position

```rust
pub fn Position (&self, dX: f64, dY: f64, dZ: f64)
```

- **Parameters:** `dX`, `dY`, `dZ` - the position, in the fabric's right-handed Z-up world frame (X east, Y north, Z up).
- **Returns:** nothing.
- **Description:** Sets the node's local position relative to its parent.
- **Example:**

```rust
pNode.Position (0.0, 0.0, 0.428);
```

- **See also:** [`Scale`](#scale), [`Bound`](#bound).

### Scale

```rust
pub fn Scale (&self, dScale: f64)
```

- **Parameters:** `dScale` - a single uniform scale factor applied to all axes.
- **Returns:** nothing.
- **Description:** Scales the node uniformly. Use [`Scale_Axes`](#scale_axes) for non-uniform scaling.
- **Example:**

```rust
pNode.Scale (2.0);
```

- **See also:** [`Scale_Axes`](#scale_axes).

### Scale_Axes

```rust
pub fn Scale_Axes (&self, dX: f64, dY: f64, dZ: f64)
```

- **Parameters:** `dX`, `dY`, `dZ` - per-axis scale factors.
- **Returns:** nothing.
- **Description:** Scales the node independently on each axis.
- **Example:**

```rust
pNode.Scale_Axes (1.0, 1.0, 2.0);
```

- **See also:** [`Scale`](#scale).

### Bound

```rust
pub fn Bound (&self, dX: f64, dY: f64, dZ: f64)
```

- **Parameters:** `dX`, `dY`, `dZ` - the three bounding extents.
- **Returns:** nothing.
- **Description:** Sets the node's bounding-box extents (used for framing and culling).
- **Example:**

```rust
pNode.Bound (0.4, 0.4, 0.43);
```

- **See also:** [`Position`](#position).

### Name

```rust
pub fn Name (&self, sName: &str)
```

- **Parameters:** `sName` - the node's internal name.
- **Returns:** nothing.
- **Description:** Sets the node's internal name (for identification/inspection).
- **Example:**

```rust
pNode.Name ("Stool");
```

- **See also:** [`SNEEZE_ABI_MAPOBJECT::Name`](MAPOBJECT.md#name).

### Resource

```rust
pub fn Resource (&self, sUrl: &str)
```

- **Parameters:** `sUrl` - the resource URL (e.g. a GLB model), absolute or relative to the fabric.
- **Returns:** nothing.
- **Description:** Sets the node's resource reference - the model or asset the engine fetches and renders for this node.
- **Example:**

```rust
pNode.Resource ("assets/Stool.glb");
```

- **See also:** [`SNEEZE_ABI_MAPOBJECT::Reference`](MAPOBJECT.md#reference).

### Panel

```rust
pub fn Panel (&self, sRml: &str)
```

- **Parameters:** `sRml` - the RML+CSS source for the panel.
- **Returns:** nothing.
- **Description:** Sets a PANEL-class node's RML+CSS source (the in-world UI document). No effect on non-panel nodes - create the node as class `PANEL` first, via [`SNEEZE_ABI_MAPOBJECT::Panel`](MAPOBJECT.md#class-factories).
- **Example:**

```rust
let mut pObj = SNEEZE_ABI_MAPOBJECT::Panel ();
pObj.Parent (pRoot.Class (), pRoot.ObjectIx ());
let pPanel = pFabric.Scene ().Node_Open (&pObj);
pPanel.Panel ("<rml><body>Hello</body></rml>");
```

- **See also:** [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md).
