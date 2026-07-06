# Scene — Scene Object Model (SOM)

The `scene` module implements the browser's internal scene graph. The SOM is a
hierarchically owned graph of FABRICs and NODEs. Content providers (MSF files,
WASM modules) populate it; the compositor and renderer read from it.

SCENE is owned by CONTEXT (not VIEWPORT) — it represents the tab's spatial
state, independent of whether a viewport is active.

## Architecture

```
SCENE (owned by CONTEXT)
 ├── root FABRIC (structural root — root container, no MSF)
 │    └── root NODE (in the root container)
 │         └── primary attach NODE ──► primary FABRIC
 │                                      └── root NODE
 │                                           ├── child NODEs (from MSF/WASM)
 │                                           └── ...
 ├── m_pNode_Primary  (primary attachment node — owned by SCENE)
 └── m_umpFabric      (scene-global fabric map, keyed by twFabricIx)

CONTAINER (one per MSF identity)
 └── m_umpNode        (per-container node handle table, keyed by composed OBJECTIX qwComposed)
     m_apMap_Object   (per-container map-object backing store)
     m_twObjectIx_Next (per-container index allocator)
```

**Node identity is per-container, not scene-global.** The node handle table
lives on CONTAINER (not SCENE), because the same MSF loaded into multiple
fabrics under one container shares one node namespace, and different containers
must not collide. A fabric reaches its node operations through
`pFabric->Container()`.

FABRICs own trees of NODEs. NODEs are structural — all 3D properties
(position, scale, color, bounding volume) live on the referenced MAP_OBJECT.

## Fabric Loading Flow

SCENE orchestrates the full lifecycle of fabric creation:

1. A NODE recognizes it's an attachment point (bSubtype == 255) and calls
   `SCENE::Fabric_Spawn(pNode, sUrl)`, which starts the async MSF fetch.
2. SCENE fetches the MSF file via the root container's network cache
   (all MSF fetches share a single cache — deduplication is automatic).
3. On success, SCENE parses the MSF, verifies signature and chain.
4. SCENE calls `CONTEXT::Container_Open(pMsf)` to get or create the
   CONTAINER for this MSF's identity.
5. SCENE assigns a scene-global `twFabricIx` and creates the FABRIC,
   passing it the index, parsed MSF, and CONTAINER at construction.
6. FABRIC::Initialize() kicks off WASM module fetches using its own
   CONTAINER.

Symmetric teardown (reverse of creation order):

1. NODE calls `SCENE::Fabric_Close(pFabric)` during destruction.
2. SCENE deletes the FABRIC (cascade deletes child nodes and WASM instances),
   erases the fabric from the map.
3. SCENE calls `CONTEXT::Container_Close(pContainer)`.
4. SCENE deletes the MSF.

## SCENE

Root container for the SOM. Owned by CONTEXT. Uses pimpl pattern.
`Initialize(sUrl)` creates the structural root fabric (a plain FABRIC with the
synthetic root container and no MSF), then builds its root node and the primary
attachment node directly. SCENE holds the primary node as `m_pNode_Primary`.

```cpp
SCENE* pScene = pContext->Scene ();
FABRIC* pRoot = pScene->Fabric_Root ();
FABRIC* pPrimary = pScene->Fabric_Primary ();
ENGINE* pEngine = pScene->Engine ();
NETWORK* pNetwork = pScene->Network ();

// Fabric management
pScene->Fabric_Spawn (pNode, sUrl);      // async — triggers MSF fetch
pScene->Fabric_Close (pFabric);          // sync — deletes fabric + closes container
FABRIC* pF = pScene->Fabric_Find (42);   // lookup by scene-global index

// Navigation
pScene->Url (sUrl);                       // swap the root fabric to a new URL
pScene->Reload (bReset);                  // reload the current root fabric URL
const std::string& sUrl = pScene->Fabric_Root ()->Url (); // current URL
```

`Url(sUrl)` tears down the existing root fabric, resets the scene-global fabric
index, and rebuilds the root fabric from the new URL. `Reload(bReset)` re-issues
`Url()` with the root fabric's current URL (`bReset` requests a cache reset).
Because a scene swap replaces all geometry, `Url()` calls
`VIEWPORT::Scene_Invalidate()` so the renderer rebuilds rather than updating
stale objects (see `Viewport.md`). The scene's URL is not stored on SCENE — it
is read from the root FABRIC, which records its URL at `Initialize()`.

### Backdrop (background colour)

SCENE owns the page background colour and hands it to the renderer through the
compositor. `Background(r, g, b, a)` stores the colour and trips a single atomic
changed-flag; the compositor test-and-clears it once per build via
`Backdrop_Consume(aColor)` and pushes to `RENDERER::SetBackground` only on change
(including scene swaps), never every frame. `Fabric_Root_Create` resets the
backdrop to black at the start of every load, so a fresh page always begins from
a known colour that the primary fabric may then override.

### Primary Presentation

Only the **primary** fabric drives page-wide presentation. When
`OnMsfReady` opens a fabric on the primary attachment node
(`pNode_Attach == m_pNode_Primary`), SCENE calls `Primary_Apply(pMsf)`, which
reads an optional `"primary"` block from the MSF payload:

- `primary.camera.position` (3-element array) and `primary.camera.rotation`
  (4-element quaternion) set the viewport's initial camera pose via
  `VIEWPORT::Camera`.
- `primary.background` (an `"RRGGBB"` hex string) sets the backdrop via
  `Background()`.

All keys are optional; a fabric with no `"primary"` block keeps the default
camera and the black backdrop. Non-primary (attached child) fabrics never touch
presentation.

### Object Identity — OBJECTIX

An object handle is an `OBJECTIX`: a single `uint64_t` (`qwComposed`) that packs
two fields. The upper 16 bits are a `MAP_OBJECT_CLASS` discriminator; the low 48
bits are the object index. Two accessors split it: `ObjectIx()` returns the low
48 bits, `Class()` returns the upper 16 cast to `MAP_OBJECT_CLASS`. Compose the
two with the `OBJECTIX_COMPOSE(eClass, twObjectIx)` macro (in `Map_Object.h`)
rather than writing opaque 64-bit literals.

`OBJECT_HEAD` carries two OBJECTIX values — `Self` and `Parent` — so every node
knows both its own class+index and its parent's. An object's class lives only in
`Self.Class()`; it is never stored anywhere else (see MAP_OBJECT below).

### Node Handle Table (owned by CONTAINER)

**CONTAINER** owns the node handle table (`m_umpNode`) and the map-object
backing store (`m_apMap_Object`). **The table is keyed by the full composed
`OBJECTIX` (`qwComposed` — class + index), not the bare index.** Parent lookups
therefore carry the class as well. The 48-bit index portion is allocated
per-container from `m_twObjectIx_Next`, or honored as supplied when an RMCOBJECT
carries an explicit non-identity index; the class is taken from the RMCOBJECT's
`Head.Self.Class()` and re-composed with the assigned index for the key.

The four node operations are public methods on **CONTAINER**:

| Method | Description |
|--------|-------------|
| `Node_Root(twFabricIx, pRMCObject)` | Create a fabric's root node (fabric must not already have one). Resolves the fabric via `Context()->Scene()->Fabric_Find`. Returns the composed handle. |
| `Node_Open(twParentIx, pRMCObject)` | Add a child node under the node whose composed handle is `twParentIx`. Returns the composed handle. |
| `Node_Close(twObjectIx)` | Remove the node with composed handle `twObjectIx`, cascading to its children and map object |
| `Node_Find(twObjectIx)` | Resolve a composed handle to a `NODE*` |

`Node_Create` (private to CONTAINER) reads `Head.Self.Class()`, `switch`es on it
to construct the matching derived `MAP_OBJECT` (ROOT / CELESTIAL / TERRESTRIAL /
PHYSICAL), re-stamps `Head.Self` with the composed handle, and stores the node
under that key. The handle it returns (and that `NODE::ObjectIx()` later reports)
is the composed value, so callers round-trip it unchanged through `Node_Find` /
`Node_Close`.

These are the entry points the WASM host functions call to build a fabric's
branch in WASM-managed mode; the host obtains the CONTAINER from the WASM
environment (`Container(pEnv)`) and calls `pContainer->Node_*`. The table does
not itself enforce container ownership — the host-function layer is responsible
for the access check (see Access Control below). SCENE's `Fabric_Root_Create`
also drives the root fabric's nodes through `m_pFabric_Root->Container()`.

## FABRIC

A spatial fabric branch. Constructor takes `SCENE*`, `CONTAINER*`,
`uint64_t` (fabric index), `NODE*` (attach point), and `MSF*` (parsed manifest).
pImpl pattern. The constructor links the fabric to its attachment node and
parent fabric. `Initialize()` begins loading WASM modules declared in the
MSF payload.

FABRIC does not fetch MSF files, open containers, or manage its own lifecycle
— SCENE handles all of that. FABRIC focuses on its internal state: WASM module
management, node tree ownership, and child fabric linkage.

| Accessor | Description |
|----------|-------------|
| `Scene()` | Owning SCENE |
| `Fabric_Parent()` | Parent fabric in the hierarchy |
| `Node_Root()` | Root NODE of this fabric's subtree |
| `Node_Attach()` | Attachment point in the parent fabric |
| `Container()` | Associated CONTAINER (provided at construction) |
| `Msf()` | Parsed MSF object (non-owning — SCENE creates and deletes) |
| `Url()` | Source URL |
| `FabricIx()` | Scene-global fabric index |

The root fabric is an ordinary FABRIC, distinguished only by having no MSF and
the synthetic root container. SCENE (not the fabric) builds and owns its root
node and primary attachment node; `SCENE::Fabric_Primary()` reaches the primary
fabric through the primary node's `Fabric_Attachment()`.

## NODE

Structural graph element. Constructor takes `FABRIC*` + `NODE*` parent. pImpl
pattern. Two-step construction: constructor links into tree, `Initialize(MAP_OBJECT*)`
assigns the 3D payload.

When a MAP_OBJECT with bSubtype == 255 (attachment point) is initialized, NODE
delegates to `SCENE::Fabric_Spawn()` to begin the async fabric loading process.
On destruction, NODE closes its child nodes through its owning fabric's
CONTAINER (`m_pFabric->Container()->Node_Close(...)`) and calls
`SCENE::Fabric_Close()` for any attached fabric.

An attachment point uses `bSubtype == 255` (not `bType`) precisely so the node
can still carry a meaningful celestial `bType` (e.g. `PLANETSYSTEM`) — the
attachment point holds the orbit data and renders an orbit trail, while also
spawning the child fabric. This lets the same system be either an attachment
(orbiting its parent) or a standalone primary fabric (at the origin).

When a MAP_OBJECT carries a non-empty `Resource.sReference`, NODE::Impl (which
inherits `SNEEZE::IFILE`) fetches it **by URL** and decides what it is **by
content** on completion — there is one fetch path, not one per resource type.
`Resource_Request()` opens the file; `OnFileReady` first probes the per-context
`GLTF_MODEL_CACHE` (`CONTEXT::Gltf_Model_Cache()`, see `Viewport.md`) by URL — a
hit means another node already parsed this asset as glTF, so the shared
immutable model is adopted directly and both the byte copy and the parse are
skipped. On a miss it reads the bytes and calls `Resource_Load`, which sniffs
them: a binary GLB (ASCII `glTF` magic) or glTF JSON (leading `{`) goes through
`Gltf_Load`, which resolves it via `GLTF_MODEL_CACHE::Model_Load` — the first
node for a URL parses (`DEP::GLTF::Load`) and builds a `GLTF_RENDER_MODEL`
(identity placement; per-node transforms compose at compositor submit time),
every other node shares that model; anything else is decoded as an image texture
via stb_image (`Texture_Load`). Both products are published to the **MAP_OBJECT**
(`SetTexture` / `Gltf_Render_Model`), never stored on the node itself. (A
`bSubtype == 255` resource is the exception — it is an attachment-point URL routed
to `SCENE::Fabric_Spawn`, not a fetched asset.)

```cpp
NODE* pNode = new NODE (pFabric, pParentNode, qwComposed);
pNode->Initialize (pMapObject);

// Iteration
for (int i = 0; i < pParent->Node_Count (); ++i)
{
   NODE* pChild = pParent->Child (i);
}
```

| Accessor | Description |
|----------|-------------|
| `ObjectIx()` | Composed object handle (class in upper 16 bits, 48-bit index in low bits) |
| `Map_Object()` | Associated MAP_OBJECT |
| `Fabric()` | Owning FABRIC |
| `Parent()` | Parent NODE |
| `Child(n)` | Nth child |
| `Node_Count()` | Number of children |
| `IsPrivate()` | Cross-container visibility |
| `Fabric_Attachment()` | Child FABRIC attached at this node (getter) |
| `Fabric_Add(pFabric)` | Attach a child fabric and relay to owning fabric |
| `Fabric_Remove(pFabric)` | Detach a child fabric and relay to owning fabric |

## CONTAINER

CONTAINER is the runtime identity of an MSF provider. `Open()` and `Close()`
take no arguments and manage the refcount for console stream, storage silo,
and WASM store lifecycle. Fabric indexing is scene-global, owned by SCENE.
The **node handle table is owned by CONTAINER** (see "Node Handle Table" above):
`Node_Root` / `Node_Open` / `Node_Close` / `Node_Find` are public CONTAINER
methods, backed by `m_umpNode`, `m_apMap_Object`, and `m_twObjectIx_Next`.

`CONTEXT::Container_Open(MSF*)` derives the CID from the MSF (or creates a
synthetic root CID when MSF is null). `CONTEXT::Container_Close(CONTAINER*)`
decrements the refcount and deletes the container when it reaches zero.

## MAP_OBJECT

Base class for 3D domain objects. Constructed from an `OBJECT_HEAD`, which it
stores in `m_Head`; the rest of the wire payload (`m_Name`, `m_Type`,
`m_Resource`, `m_Transform`, `m_Orbit`, `m_Bound`, `m_Properties`) is copied in
by `Node_Create` after construction. These members mirror the RMCOBJECT
wire-format sub-structs byte-for-byte.

The object's **class is derived, never stored as its own field**:
`MAP_OBJECT::Class()` returns `m_Head.Self.Class()` (the upper 16 bits of the
self OBJECTIX). Spatial properties are read through accessors that consult the
sub-structs — e.g. `Position()`/`Rotation()` from `m_Transform` and `m_Orbit`,
`Radius()` from `m_Bound`, and the `ColorToU32()` family from `m_Properties`.

**Class-tagged sub-structs.** `m_Orbit` (`MAP_OBJECT_ORBIT`) and `m_Properties`
(`MAP_OBJECT_PROPERTIES`) are fixed-size regions whose interpretation depends on
the node's class — each is a `union` with a per-class member (`.Celestial`,
`.Light`) over the same bytes, so the wire size never changes. Orbit is used only
by celestial objects. Properties' celestial member holds `fMass`/`fGravity`/
`fColor`/`fBrightness`/`fReflectivity`; its light member keeps `fColor`/
`fBrightness` at the same offsets (so `ColorToU32()` works for any class) and
repurposes the leading 8 bytes as a spot's `fOpeningAngle`/`fFalloffAngle`. Read
each region through the member the node's class owns.

`MAP_OBJECT_TYPE` is the 8-byte wire sub-struct: `bType` (the celestial type —
see below), `bSubtype` (the object subtype), `bFiction`, and 5 reserved bytes.
`bSubtype == 255` marks an MSF attachment point, leaving `bType` free to carry
the node's celestial type (so an attachment point can also be a `PLANETSYSTEM`
with orbit data).

### Visual Appearance (texture + render model)

A MAP_OBJECT owns the object's **visual products**, fetched by its NODE (see
NODE above) and published here for the compositor to read:

- **Base-color texture** — `SetTexture(pTex, w, h)` / `GetTexture(pTex, w, h)`.
  Decoded RGBA8 pixels held under a mutex; `GetTexture` returns false until ready.
- **glTF/GLB render model** — `Gltf_Render_Model(GLTF_RENDER_MODEL*)` (setter,
  takes ownership) / `Gltf_Render_Model()` (getter, returns null until built).
  The pointer is published write-once via an atomic acquire/release flag (the
  built model is immutable, so no lock is needed to read it) and freed when the
  MAP_OBJECT is destroyed. `GLTF_RENDER_MODEL` is defined in `Viewport.h` (see
  `Viewport.md`); `Map_Object.h` forward-declares it.

Both accessors live on the **base** MAP_OBJECT, so a model can sit at **any**
class level — celestial, terrestrial, or physical. The compositor renders a
node's model wherever it exists, independent of class (see `Control.md`).

### Derived Types

The derived class is chosen by `Node_Create` switching on `Head.Self.Class()`,
and must agree with the class packed into the handle:

| Type | Class | Notes |
|------|-------|-------|
| `MAP_OBJECT_ROOT` | `MAP_OBJECT_CLASS_ROOT` (70) | Used by the scene's built-in root fabric (its root and primary nodes) |
| `MAP_OBJECT_CELESTIAL` | `MAP_OBJECT_CLASS_CELESTIAL` (71) | Orbital bodies and frames |
| `MAP_OBJECT_TERRESTRIAL` | `MAP_OBJECT_CLASS_TERRESTRIAL` (72) | — |
| `MAP_OBJECT_PHYSICAL` | `MAP_OBJECT_CLASS_PHYSICAL` (73) | — |
| `MAP_OBJECT_PANEL` | `MAP_OBJECT_CLASS_PANEL` (74) | In-scene RmlUi panel (textured quad) |
| `MAP_OBJECT_LIGHT` | `MAP_OBJECT_CLASS_LIGHT` (75) | Scene light node (ambient / directional / point / spot) |

Every derived constructor takes an `OBJECT_HEAD` and forwards it to the base.

### MAP_OBJECT_CELESTIAL

Contains orbital mechanics data via the `ORBIT_POSITION` struct (defined in
`Map_Object.h`). File-local static functions `SolveKepler`, `QuatMultiply`, and
`RotateByQuat` in `Map_Object.cpp` compute orbital positions from `m_Orbit` and
`m_Transform`. The compositor calls `PositionAtTick()` for animation.

The celestial type is stored in `m_Type.bType`, valued from the
`MAP_OBJECT_TYPE_TYPE_CELESTIAL_*` enum (NONE, UNIVERSE, ... STARSYSTEM=9,
STAR=10, PLANETSYSTEM=11, PLANET=12, MOON=13, DEBRIS=14, SURFACE=17, etc.). The
compositor and `Rotation()` branch on this value.

### MAP_OBJECT_LIGHT

A scene light node. It carries no geometry — the compositor reads it during
traversal and emits an ANARI light at the node's world placement. A light reads
the class-tagged `Properties.Light` member (see the class-tagged Properties note
below): the **colour** is packed into `Properties.Light.fColor` as `0xRRGGBB`, the
**intensity** into `Properties.Light.fBrightness`, and — for a spot — the cone into
`fOpeningAngle` / `fFalloffAngle` (degrees). The **kind** is the node's
`Type.bType`, valued from `MAP_OBJECT_TYPE_TYPE_LIGHT_*`:

| Subtype | Value | ANARI light | Uses |
|---------|-------|-------------|------|
| `MAP_OBJECT_TYPE_TYPE_LIGHT_AMBIENT` | 1 | `"ambient"` | Uniform fill; no position |
| `MAP_OBJECT_TYPE_TYPE_LIGHT_DIRECTIONAL` | 2 | `"directional"` | The world-space vector is a direction |
| `MAP_OBJECT_TYPE_TYPE_LIGHT_POINT` | 3 | `"point"` | Position from the node's world transform; `1/r²` falloff |
| `MAP_OBJECT_TYPE_TYPE_LIGHT_SPOT` | 4 | `"spot"` | Position + aim down the node's local -Z (rotated by its transform); cone from `fOpeningAngle`/`fFalloffAngle` |

A point (or spot) light authored at unit scale keeps its illumination invariant
when the node is embedded (and scaled) inside another fabric and again when the
whole scene is fitted to the render volume — the compositor multiplies its
intensity by `(worldScale · renderScale)²` at the flatten seam (see `Control.md`
"Lighting"). Ambient and directional lights have no falloff and pass through
unscaled. Author a light once at unit scale and drop it in anywhere.

### MAP_OBJECT_PANEL

An in-scene UI panel — an RmlUi RML+CSS document rasterized to a textured quad.
It owns a `DEP::UI_PANEL` (see `Ui_Context.md`) and exposes
`Source(const std::string&)` (sets the panel's RML+CSS document; if never set, a
built-in default document is used), `Render(ENGINE*, w, h)`, plus
`Pixels()/Width()/Height()`. The compositor calls `Render` during traversal (on
the render thread; cheap when unchanged) and hands the pixels to the renderer as
an unlit, alpha-blended quad.

WASM modules create a panel node in one call via the `Scene` host function
`Node_Panel(twParentIx, objPtr, objLen, srcPtr, srcLen) -> twObjectIx`: it reads
an `RMCOBJECT` (forcing its class to `MAP_OBJECT_CLASS_PANEL`), creates the child
node under `twParentIx`, then sets the panel's source from the second
pointer/length pair. The source string is passed separately because RML+CSS far
exceeds the 128-byte `Resource.sReference` wire field.

By design a panel rides the universal TRS like any other node: its world size is
authored in `Bound.d3Max[0,1]` (metres) and its placement in the node's
transform. The compositor rasterizes the RmlUi document during traversal and
hands the pixels to the renderer as an unlit, alpha-blended quad (see
`Control.md`).

## Fabric Ownership Modes

A fabric's scene branch can be populated by one of two mutually exclusive
authorities. This choice is per-fabric and is determined by the WASM code
at runtime.

### Mode A: WASM-Managed

The WASM code builds the scene graph directly through host function calls.
It calls `Node_Root` to establish the root node, then `Node_Open` to add
children. The WASM module is the sole authority over the fabric's branch —
it creates, modifies, and deletes nodes as it sees fit.

### Mode B: Map-Managed

The WASM code instructs the browser to connect to a map service (using
connection info from the MSF payload) and delegates scene population to
the browser. The browser manages the root node and all map objects on the
fabric's behalf. The WASM code does not directly create or modify nodes
in the branch — if it wants something changed, it sends a request to the
map service, which pushes the change back through the browser.

### Why Mutually Exclusive

In a web browser, the DOM is passive. JavaScript is the only writer, so
there is never a conflict between the page's source data and runtime
mutations. In a metaverse browser, a map service is an *active,
authoritative source*. It pushes updates, tracks server-side state, and
expects its objects to exist. If WASM code deletes or mutates a map-managed
object behind the service's back, the next update from the service targets
a node that no longer exists — or worse, one whose state has diverged from
the server's model. Two writers on the same branch is a conflict by
definition.

The rule is: **the entity that creates owns the mutations.** Map-managed
branches are read-only from WASM's perspective. WASM-managed branches have
no map service involved.

### The Duplicate Index Problem

When the same MSF is loaded into two fabrics that share the same container
(same organizational identity), both fabrics run the same WASM modules.
In WASM-managed mode, the node handle table (`m_umpNode`) is per-container
and shared across every fabric in that container.

If the MSF describes a template — say a poker table with 8 seats — each
fabric instantiates that template. The WASM module sends the same objects
with the same template indices both times. But the container's handle map
cannot hold two different nodes under the same key.

Moving the table from scene-global to per-container narrowed the collision
scope (two unrelated containers no longer collide) but did not eliminate it
for the same-MSF-twice-in-one-container case.

In map-managed mode, this problem does not arise: the browser controls
the indexing and assigns unique handles per fabric. In WASM-managed mode,
a solution is needed — possible approaches include namespacing indices by
fabric (composite key of `twFabricIx` + `twObjectIx`) or having the
container assign globally unique handles that are returned to the WASM
module. This is an open design question.

### MSF as Configuration

The MSF file is not an instruction set that the browser acts on
autonomously. Services and connection info are listed in the MSF payload,
but the browser does not initiate connections on its own. The MSF is a
configuration file for the WASM modules — they read it, decide what to do,
and issue explicit API calls to the browser (e.g., "connect to this map
service" or "create this node"). The browser is the execution environment;
the WASM code is the driver.

## Threading

SCENE uses `m_mxScene` (recursive_mutex) to protect the fabric map
(`m_umpFabric` / `m_twFabricIx_Next`). The guard is held during `Fabric_Open`,
`Fabric_Close`, `Fabric_Find`, and the fabric-creation block in `OnMsfReady`.

CONTAINER uses `m_mxContainer` (recursive_mutex) to protect its node handle
table (`m_umpNode` / `m_apMap_Object` / `m_twObjectIx_Next`). The guard is held
during the `Node_Root` / `Node_Open` / `Node_Close` operations.

MSF fetches route through the NETWORK fetch pool. The lock ordering contract
between `m_mxNetwork` and `m_mxAsset` (documented in `Network.md`) governs
all fetch completion callbacks, including those triggered by SCENE's
`MSF_FETCH` listener.

## Known Limitations

These bound when `Url()` / `Reload()` are safe to call:

- **In-flight MSF fetches are not cancelled.** A `MSF_FETCH` started by an
  attachment node holds a raw pointer to that node. A spawning node does not
  own or cancel its `MSF_FETCH` the way it owns its texture fetch, so if the
  node is destroyed before the fetch completes — which `Url()` / `Reload()` do,
  since they tear down the whole tree — the completion callback runs against a
  freed node. `Url()` / `Reload()` are therefore unsafe while a fabric MSF is
  still loading.
- **Teardown is not synchronized with compositor traversal.** `Url()` /
  `Reload()` wipe the fabric/node tree on the calling thread while the
  compositor may be traversing it on its agent thread. There is no shared read
  guard yet, so navigating during active rendering can crash.
- **Renderer rebuild is coarse.** `VIEWPORT::Scene_Invalidate()` forces a full
  renderer rebuild and is called only on navigation. Structural changes the
  renderer cannot self-detect (same object counts, different content) are not
  signalled incrementally. The intended design is a scene revision counter the
  compositor reads under the same read guard that fixes traversal safety, so
  rebuild-detection and traversal-safety become one mechanism.

## Access Control

`AccessControl.h` provides `CanRead()` / `CanWrite()` functions for WASM host
functions. Browser internals pass `nullptr` as owner and bypass all checks.
Write operations require `pFabric->Container() == pContainer` — the caller
must own the fabric to modify it. Read operations are unrestricted.

The CONTAINER node handle table (`Node_Root` / `Node_Open` / `Node_Close`) does
not enforce ownership on its own. The WASM host-function layer must call
`CanWrite()` before mutating a fabric's branch — the container methods trust
their caller.

## Files

| File | Contents |
|------|----------|
| `Scene.cpp` | SCENE + Impl (pimpl, fabric map, root fabric + primary node, MSF_FETCH, Fabric_Spawn/Open/Close/Find, Url/Reload). `Fabric_Root_Create` drives the root node/primary attach node through `m_pFabric_Root->Container()`. |
| `Fabric.cpp` | FABRIC + Impl (WASM module lifecycle, node linkage, child fabrics; closes its root node via `Container()->Node_Close`) |
| `Node.cpp` | NODE + Impl (tree ops; resource fetch via IFILE with content-sniff dispatch to texture/glTF load; delegates fabric ops to SCENE; closes child nodes via `Container()->Node_Close`) |
| `../Container.cpp` | CONTAINER + Impl — owns the per-container node handle table and the `Node_Root/Open/Close/Find` + private `Node_Create` operations |
| `Map_Object.h` | MAP_OBJECT hierarchy, ORBIT_POSITION struct, MAP_OBJECT_CLASS enum, celestial type enum, OBJECTIX (+ OBJECTIX_COMPOSE), RMCOBJECT wire structs |
| `Map_Object.cpp` | MAP_OBJECT methods (incl. texture + glTF render-model accessors), SolveKepler, QuatMultiply, RotateByQuat |
| `AccessControl.h/cpp` | CanRead/CanWrite enforcement |
