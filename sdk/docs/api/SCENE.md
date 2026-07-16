# SCENE

Node-tree construction on the fabric's container, reached through [`FABRIC::Scene`](FABRIC.md#scene). This is where a module builds what the user sees: it creates the fabric's root node and adds children. `SCENE` is a zero-cost view over the fabric handle.

The building pattern is: fill a [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md) with a fluent builder, then hand it to `Node_Root` or `Node_Open`. Each returns a live [`NODE`](NODE.md) you can mutate afterward. `Node_Map` is a shortcut that builds an entire subtree from the fabric's `Data` block.

## Methods

### Node_Root

```rust
pub fn Node_Root (&self, pObject: &SNEEZE_ABI_MAPOBJECT) -> NODE
```

- **Parameters:** `pObject` - the map object describing the root node (built with a [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md) builder).
- **Returns:** the created root [`NODE`](NODE.md). Check `NODE::IsValid ()`.
- **Description:** Creates the fabric's single root node from a map object. A fabric has one root; everything else is a descendant of it. The create call copies the map object's bytes, so you may drop or reuse the builder afterward.
- **Example:**

```rust
let mut pRoot = SNEEZE_ABI_MAPOBJECT::Physical ();
pRoot.Name ("Stool").Reference ("assets/Stool.glb");
let pNode = pFabric.Scene ().Node_Root (&pRoot);
```

- **See also:** [`Node_Open`](#node_open), [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md), [`NODE`](NODE.md).

### Node_Open

```rust
pub fn Node_Open (&self, pObject: &SNEEZE_ABI_MAPOBJECT) -> NODE
```

- **Parameters:**
  - `pObject` - the map object describing the child node. Its parent is taken from the object's own parent index, set with [`SNEEZE_ABI_MAPOBJECT::Parent`](MAPOBJECT.md#parent).
- **Returns:** the created child [`NODE`](NODE.md). Check `NODE::IsValid ()`.
- **Description:** Creates a child node from a map object. The parent is whatever parent index you placed in the object's Head - no parent `NODE` handle is required, so you may name any parent index directly (useful when authoring a flat tree by explicit index). When you have the parent as a live `NODE`, set the child's parent from it with `Parent (pParent.Class (), pParent.ObjectIx ())`. The create call copies the map object's bytes.
- **Example:**

```rust
let pScene = pFabric.Scene ();

let mut pRoot = SNEEZE_ABI_MAPOBJECT::Physical ();
pRoot.Name ("Stool").Reference ("assets/Stool.glb");
let pStool = pScene.Node_Root (&pRoot);

let mut pChild = SNEEZE_ABI_MAPOBJECT::Physical ();
pChild.Parent (pStool.Class (), pStool.ObjectIx ()).Name ("Bucket").Reference ("assets/Bucket.glb").Position (0.0, 0.0, 0.428);
let pBucket = pScene.Node_Open (&pChild);
```

- **See also:** [`Node_Root`](#node_root), [`Node_Close`](#node_close).

### Node_Map

```rust
pub fn Node_Map (&self, sPath: &str) -> NODE
```

- **Parameters:** `sPath` - a path into the fabric's `Data` block (`""` = the `Data` object itself).
- **Returns:** the created root [`NODE`](NODE.md) of the built subtree.
- **Description:** Asks the engine to read a node description from the fabric's `Data` block at `sPath` and build the entire subtree from it, without you constructing each map object by hand. This is the "map-managed" path a data-driven fabric uses. See [`DATA`](DATA.md) for reading that same block as raw values.
- **Example:**

```rust
let pRoot = pFabric.Scene ().Node_Map ("Scene");
```

- **See also:** [`DATA`](DATA.md), [`Node_Root`](#node_root).

### Node_Close

```rust
pub fn Node_Close (&self, pNode: NODE) -> bool
```

- **Parameters:** `pNode` - the node to remove.
- **Returns:** `true` on success.
- **Description:** Removes and deletes a node (and its descendants) from the scene. Use it to tear down something you created earlier.
- **Example:**

```rust
pFabric.Scene ().Node_Close (pBucket);
```

- **See also:** [`Node_Open`](#node_open).
