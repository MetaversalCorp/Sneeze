---
title: NODE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Scene.h
  - src/context/scene/Node.cpp
  - include/Map_Object.h
verified: b487fd1
nav:
  prev: api/scene/FABRIC.md
  next: api/network/index.md
---

# `NODE`

A single structural element of the scene tree — the engine's equivalent of a DOM element. Each node belongs to exactly one [`FABRIC`](FABRIC.md), sits at a position in that fabric's tree, and points to a `MAP_OBJECT` payload that carries its actual content (transform, resource reference, texture, glTF model, type). A node can also be the point where a *child fabric* attaches, and it can own a single network fetch for its resource. For the conceptual picture see the [Scene system](../../systems/scene.md); this page is the exact behavior of every public member.

```cpp
class NODE
{
public:
   NODE (FABRIC* pFabric, NODE* pNode_Parent, uint64_t twObjectIx);
   ~NODE ();
   // ... see sections below
private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Belongs to** one `FABRIC` (passed at construction, never reassigned).
- **Lives in** a tree: it has a parent node (or none, if it is the fabric's root) and a list of child nodes that it owns.
- **Points to** a `MAP_OBJECT` — the content payload. The node assigns it in `Initialize`; the payload is freed by the owning [`CONTAINER::Node_Close`](../container/index.md), which owns the flat map-object list.
- **May host** a child fabric via its *attachment* slot (`Fabric_Attachment()`).
- **May own** a resource fetch — a single network `FILE` opened when its map object names a resource (a texture or a glTF/GLB model).

Like the other scene classes, `NODE` is a pimpl handle. Internally `NODE::Impl` also implements [`IFILE`](../network/index.md) so the node can receive its own resource-fetch callbacks.

---

## Lifecycle

Nodes are created in two steps and torn down by the scene:

1. **Construct.** `NODE(pFabric, pNode_Parent, twObjectIx)` links the node into the tree as a side effect: if it has a parent, it adds itself to that parent's child list; if it has no parent, it becomes the fabric's root node (`pFabric->Node_Root(this)`). The object handle is fixed at construction.
2. **Initialize.** `Initialize(pMapObject)` assigns the payload and, based on the payload's `Resource.sReference`, may trigger one asynchronous action:
- if the payload is a **fabric-attachment** (map-object subtype `255`) with a non-empty URL, the node asks the scene to spawn a child fabric on itself (`Fabric_Spawn`);
- otherwise, if the payload names any resource, the node opens a single network fetch for it and dispatches by content on completion — a glTF/GLB blob becomes the map object's render model, anything else is decoded as a texture (see [Resources](#resources-fetch-by-url-dispatch-by-content)).
3. **Close.** Nodes are not deleted directly — they are closed through [`CONTAINER::Node_Close`](../container/index.md), which deletes the `NODE` (running the destructor) and frees its map object. The destructor closes every child (cascading, through the container), closes any attached fabric, releases the resource fetch, and unlinks from the parent (or clears the fabric root).

> **Always create nodes through the container** (`CONTAINER::Node_Root` / `CONTAINER::Node_Open`) and > close them through `CONTAINER::Node_Close`. Those paths assign the object handle, register the > node in the container's handle table, and create/free the paired map object. Constructing or > deleting a `NODE` directly bypasses the registry and the payload bookkeeping.

---

## Threading and pitfalls

**Child list is guarded by a plain `std::mutex` (`m_mutex_pNode`).** `Child`, `Node_Count`, `Node_Add`, and `Node_Remove` all take it. This is a *different* lock from the scene's recursive mutex; node-local child operations and scene-level handle operations are separate layers.

**Child positions are not stable.** `Node_Remove` uses swap-and-pop: it moves the last child into the removed slot and shrinks the vector. So the index you pass to `Child(n)` is **not** a durable identifier — a removal elsewhere in the list can change which node lives at position `n`. Address nodes by object index (via the scene), not by child position, if you need stability.

**A node has exactly one attachment slot.** `Fabric_Add` *overwrites* `m_pFabric_Attachment` and forwards to the fabric's child-fabric list. Calling it twice on the same node replaces the recorded attachment without closing the previous one — the node only remembers the most recent attached fabric. In normal operation a node hosts at most one child fabric; do not attach two.

**Resource loading happens on a network thread.** `OnFileReady` reads the bytes and dispatches by content: a glTF/GLB blob is built into a `GLTF_RENDER_MODEL` (published write-once on the map object via an atomic flag, then read locklessly) and anything else is decoded to RGBA8 pixels written into the map object under its texture mutex, then flagged ready. Either way the product lives on the `MAP_OBJECT`, not the node. The renderer reads through those atomics/mutexes. The node's own state is not otherwise synchronized with the render thread.

**`Parent()` crosses fabric boundaries.** For a normal node it returns the parent node. For a fabric's *root* node (which has no parent node) it returns the fabric's attachment node — the node in the *parent* fabric that this fabric mounts on. This makes upward traversal continuous across fabric seams, but means `Parent()` can return a node owned by a different fabric.

**Teardown cascades into recursive locks.** Closing a node deletes its children (re-entering `CONTAINER::Node_Close` under the container's recursive `m_mxContainer`) and closes any attached fabric (re-entering `SCENE::Fabric_Close` under the scene's recursive `m_mxScene`). This is safe only because both mutexes are recursive — see [SCENE → Threading](SCENE.md#threading-locking-and-pitfalls).

---

## Construction and destruction

```cpp
NODE (FABRIC* pFabric, NODE* pNode_Parent, uint64_t twObjectIx);
~NODE ();
```

### `NODE(pFabric, pNode_Parent, twObjectIx)`
- **Purpose.** Construct a node and link it into its fabric's tree.
- **Parameters.**
- `pFabric` — the owning fabric (required).
- `pNode_Parent` — the parent node, or `nullptr` to make this the fabric's root.
- `twObjectIx` — the composed object handle assigned by the container.
- **Side effect.** Adds itself to the parent's child list, or sets itself as the fabric root.
- **Note.** Create through `CONTAINER::Node_Root` / `CONTAINER::Node_Open`, then call `Initialize`.

### `~NODE()`
- **Purpose.** Tear down the node: close all children (cascading, through the container), close any attached fabric, release the resource fetch, unlink from the parent (or clear the fabric root).
- **Pitfalls.** Invoked by `CONTAINER::Node_Close`, not by you. Runs the teardown cascade under the container's and the scene's recursive locks.

---

## Lifecycle method

```cpp
bool Initialize (MAP_OBJECT* pMapObject);
```

### `bool Initialize (MAP_OBJECT* pMapObject)`
- **Purpose.** Assign the node's content payload and trigger any resource it implies (spawn a child fabric for an attachment-type payload, or fetch and load a resource).
- **Parameters.** `pMapObject` — the content payload; may carry a `Resource.sReference`.
- **Returns.** `true`.
- **Notes.** A payload whose subtype is `255` with a non-empty reference spawns a child fabric on this node; any other payload with a resource reference opens one network fetch that is dispatched by content on completion.

---

## Resources: fetch by URL, dispatch by content

A node has one resource path, not one per type. When its payload carries a non-empty `Resource.sReference` (and is not an attachment), `Initialize` calls `Resource_Request`, which opens a single `FILE` on the container's cache. On completion `OnFileReady` reads the bytes and `Resource_Load` sniffs them: a binary GLB (ASCII `glTF` magic) or a glTF JSON document (leading `{`) is parsed via `DEP::GLTF::Load` and built into a `GLTF_RENDER_MODEL` handed to the map object (`MAP_OBJECT::Gltf_Render_Model`, which takes ownership); anything else is decoded to RGBA8 via stb_image and set on the map object (`MAP_OBJECT::SetTexture`). Both products live on the `MAP_OBJECT`, never on the node. `Resource_Release` (called on close) closes the fetch. A failed fetch (`OnFileFailed`) simply closes the file.

---

## Accessors

```cpp
uint64_t    ObjectIx          () const;
std::string Name              () const;
std::string ClassName         () const;
std::string TypeName          () const;
int         Subtype           () const;
MAP_OBJECT* Map_Object        () const;
FABRIC*     Fabric            () const;
FABRIC*     Fabric_Attachment () const;
NODE*       Parent            () const;
NODE*       Child             (int nPosition) const;
int         Node_Count        () const;
bool        IsPrivate         () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `ObjectIx()` | The node's composed object handle (class in the upper 16 bits, 48-bit index in the low bits). | Fixed at construction; the key for `CONTAINER::Node_Find`. |
| `Name()` | The map object's name as UTF-8. | Decoded from the payload's fixed-size UTF-16 (BMP) name buffer; empty if there is no payload. |
| `ClassName()` | The payload's class as a lowercase string (`"root"`, `"celestial"`, `"terrestrial"`, `"physical"`, `"panel"`, `"light"`). | From `MAP_OBJECT::ClassName(Class())`; empty if there is no payload. |
| `TypeName()` | The class-specific type name. | Only celestial bodies have named types today; other classes fall back to `"type<N>"`. Empty if there is no payload. |
| `Subtype()` | The raw subtype discriminator (`Type.bSubtype`). | `255` marks a fabric-attachment point; `0` if there is no payload. |
| `Map_Object()` | The content payload, or null. | Null until `Initialize`. |
| `Fabric()` | The owning fabric. | Never null; never reassigned. |
| `Fabric_Attachment()` | The child fabric mounted on this node, or null. | At most one; set via `Fabric_Add`. |
| `Parent()` | The parent node — or, for a fabric root, the fabric's attachment node. | Crosses fabric boundaries (see pitfalls). |
| `Child(nPosition)` | The child at `nPosition`, or null if out of range. | Locked. Position is **not** stable across removals. |
| `Node_Count()` | The number of children. | Locked. |
| `IsPrivate()` | The node's private flag. | Used by access control to restrict cross-container visibility. Not lock-protected. |

---

## Mutators

```cpp
void Private       (bool bPrivate);
void Fabric_Add    (FABRIC* pFabric_Child);
void Fabric_Remove (FABRIC* pFabric_Child);
```

### `void Private (bool bPrivate)`
- **Purpose.** Mark the node private (or public). Access control consults this to decide whether other containers may see the node.
- **Parameters.** `bPrivate` — `true` to hide from other containers.

### `void Fabric_Add (FABRIC* pFabric_Child)`
- **Purpose.** Record `pFabric_Child` as this node's attached fabric and register it with the owning fabric's child list.
- **Parameters.** `pFabric_Child` — the child fabric attaching here.
- **Pitfalls.** Overwrites the existing attachment slot — see [Threading and pitfalls](#threading-and-pitfalls). Called from the child fabric's constructor, not by application code.

### `void Fabric_Remove (FABRIC* pFabric_Child)`
- **Purpose.** Clear this node's attachment slot and unregister `pFabric_Child` from the owning fabric's child list.
- **Parameters.** `pFabric_Child` — the child fabric detaching.

---

## Child-node methods (internal)

```cpp
void Node_Add    (NODE* pNode_Child);
void Node_Remove (NODE* pNode_Child);
```

### `void Node_Add (NODE* pNode_Child)`
- **Purpose.** Append a child node. Called from the child's constructor, not by application code.
- **Parameters.** `pNode_Child` — the child to append.
- **Thread-safety.** Takes `m_mutex_pNode`.

### `void Node_Remove (NODE* pNode_Child)`
- **Purpose.** Remove a child node (swap-and-pop).
- **Parameters.** `pNode_Child` — the child to remove.
- **Thread-safety.** Takes `m_mutex_pNode`. No-op if the child is not present.
- **Pitfalls.** Swap-and-pop changes the position of the child that was last in the list — see [Threading and pitfalls](#threading-and-pitfalls).

---

## See also

- [Scene system](../../systems/scene.md) — design, loading flow, limitations.
- [SCENE](SCENE.md) — the root of the model and the fabric registry.
- [FABRIC](FABRIC.md) — the tree a node belongs to and may attach.
- [Network API](../network/index.md) — `FILE` / `IFILE`, used for the resource fetch.
- [Container API](../container/index.md) — owns the node handle table that creates, finds, and closes nodes.

---

[Scene API](index.md) · Prev: [FABRIC](FABRIC.md)
