# Viewport — Rendering and Camera

The viewport module owns the rendering pipeline and camera controls for each
context. Each CONTEXT owns one VIEWPORT; the VIEWPORT only renders when an
IVIEWPORT host is attached (enabling headless contexts for inactive tabs).

## Architecture

```
VIEWPORT (Viewport.cpp, pImpl)
├── RENDERER (abstract, declared in Viewport.h)
│   └── RENDERER::ANARI (AnariRenderer.h/cpp — Halogen/Filament backend)
├── VIEW (camera orbit state, declared in include/Viewport.h)
├── INPUT (accumulated mouse/key state, declared in include/Viewport.h)
├── UV_SPHERE (mesh generator, UVSphere.cpp)
├── GLTF_RENDER_MODEL + Gltf_Render_Model_Build / Acquire / Publish / Release (glTF→renderer bridge, GltfMesh.cpp)
└── JOB_COMPOSITOR (pool-cycle job, managed by CONTROL)
```

All source lives in `src/context/viewport/` — no subdirectories.

## VIEWPORT

Owned by CONTEXT. pImpl pattern.

- `Renderer_Initialize()` — deferred, called from compositor agent 0 thread
  (Filament thread affinity). Creates `RENDERER::ANARI`.
- `Renderer_Shutdown()` — called from compositor agent 0 via Execute_Destroy.
- `Activate(IVIEWPORT*)` — creates JOB_COMPOSITOR, posts to POOL_CYCLE.
- `Deactivate()` — cancels compositor job, blocks until renderer shutdown.
- Input: `Input_Mouse()` / `Input_Key()` (accumulated under `m_mxInput`).
- Framebuffer: `FrameBuffer_Write()` / `FrameBuffer_Capture()` /
  `FrameBuffer_Release()` (producer-consumer with mutex).
- Timing: `Accumulate()` tracks per-section durations; `Diagnostics()` logs
  FPS and averages once per second.
- Scene invalidation: `Scene_Invalidate()` (set, called from any thread) /
  `Scene_Invalidate_Consume()` (test-and-clear, called by the compositor) carry
  a request to fully rebuild the renderer scene across threads (atomic flag).

## RENDERER (abstract)

Declared in `Viewport.h` (private header). Virtual interface for all backends.

### Frame Lifecycle

```
SetCamera (CAMERA_DATA)
SetLights (vector<LIGHT_DATA>)
SetSceneLighting (SCENE_LIGHT Ambient, SCENE_LIGHT Directional)
BeginFrame ()
SubmitSpheres (vector<SPHERE_DATA>)
SubmitCurves (vector<CURVE_DATA>)
SubmitBoxes (vector<BOX_DATA>)
SubmitPanels (vector<PANEL_DATA>)
SubmitMeshes (vector<MESH_DATA>)
EndFrame ()
```

After `EndFrame()`, framebuffer is available via `GetFrameBuffer()`.

### Native Surface Rendering

If `SetNativeWindow(hwnd)` is called before `Initialize()`, Filament creates a
Vulkan swapchain on the platform window (zero CPU copies, 60 FPS). The
framebuffer publish path is skipped entirely.

### Data Types

| Type | Purpose |
|------|---------|
| `SPHERE_DATA` | Position, radius, color, optional texture pixels, emissive flag |
| `CURVE_POINT` | Vertex with position and radius |
| `CURVE_DATA` | Polyline (vector of CURVE_POINTs) with color |
| `BOX_DATA` | Column-major world transform (`mWorld`) + color |
| `PANEL_DATA` | Column-major world transform (`mWorld`, size baked in) + straight-alpha RGBA8 pixels + width/height |
| `MESH_DATA` | One drawable glTF surface: column-major `mWorld`, borrowed vertex streams (position/normal/texcoord + uint32 indices), metallic-roughness PBR factors, optional decoded RGBA8 base-color texture, `bUnlit`, and `eAlpha` / `fAlphaCutoff` (glTF MASK/BLEND) |
| `GLTF_RENDER_MODEL` | A loaded glTF prepared for rendering — owns the source `DEP::GLTF_MODEL`, the decoded textures, the flattened `aMesh` draw list, and a model-space bounding sphere (`vCenter`, `dRadius`) |
| `CAMERA_DATA` | Eye, look direction, up, FOV, aspect, near/far |
| `LIGHT_DATA` | One placed (point/spot) light: `eType` (`kPOINT`/`kSPOT`), `vPosition` (world position, `VEC3`), `vDirection` (spot aim, unit `VEC3`), `rgbColor` (`RGB`), `fIntensity`, and spot cone (`fOpeningAngle`, `fFalloffAngle`, radians) |
| `SCENE_LIGHT` | Scene-global ambient or directional light (declared in `Scene.h`): `rgbColor` (`RGB`), `fIntensity` (ambient radiance / directional irradiance), `vDirection` (`VEC3`, directional only) |
| `UV_SPHERE` | Generated mesh: positions, normals, texcoords, indices |

### Lighting

Lighting arrives on two channels. `SetLights(vector<LIGHT_DATA>)` supplies the
frame's **placed** lights (point and spot). The compositor fills that vector from
two sources — `STAR` celestial nodes (one point light each) and explicit
`MAP_OBJECT_LIGHT` nodes (colour, intensity, and subtype flattened per light; see
`Control.md` "Lighting"). `SetSceneLighting(SCENE_LIGHT Ambient, SCENE_LIGHT
Directional)` supplies the **scene-global** ambient + directional ("sun"),
authored in the primary fabric's `"Primary"` block (see `Scene.md` `SCENE_LIGHT`)
— never placed objects, so a local light node cannot change global illumination.

In `BuildScene` the ANARI backend switches on each placed light's `eType`:

- `kPOINT` → `"point"` (`position`, `color`, `intensity`)
- `kSPOT` → `"spot"` (`position`, `direction`, `color`, `intensity`, `openingAngle`, `falloffAngle`)

and handles the two scene-global lights directly: ambient feeds the renderer's own
ambient term (`ambientColor`, `ambientRadiance`), not a separate ANARI light
object; directional builds one `"directional"` light (`direction`, `color`,
`irradiance`). Either scene-global light with `fIntensity <= 0` is omitted.

Scene lighting is authoritative: there is no fallback. An empty light vector with
zero ambient/directional intensity simply means the scene is unlit — a primary
fabric that wants light authors an ambient or directional in its `"Primary"`
block, and when neither is authored the scene defaults to a full-intensity white
ambient (see `Scene.md`). The scene rebuilds when the placed-light **count**
changes (`m_bSceneDirty` set in `SetLights`) or when either scene-global light
changes (set in `SetSceneLighting`).

### Panels

`SubmitPanels(vector<PANEL_DATA>)` carries in-scene UI panels (see `Ui_Context.md`
and `Scene.md` `MAP_OBJECT_PANEL`). Each `PANEL_DATA` is just a column-major world
transform (`mWorld`, size baked in) plus a straight-alpha RGBA8 pixel buffer and its
dimensions — the renderer stays UI-agnostic, treating a panel like a textured box.

The ANARI backend builds one instance per panel from a **shared unit quad** (XY
plane, `+Z` normal, `attribute0` UVs, double-sided so a panel turned away is not
culled; V is flipped vs. position so the top-down UI canvas reads upright). The
panel pixels become an `image2D` array feeding a sampler, and the material is the
**unlit** Halogen extension in `"blend"` mode (`color` = the sampler), so the
panel shows its true RGBA, lighting-independent, with per-texel alpha. Panel
instance transforms are committed in `UpdateScene`; Halogen
`Instance::commitParameters` applies them with Filament `setTransform` on the
existing entities. The world's instance array is not rebound on a
transform-only update. A rebuild is triggered only when the
panel **count** or a panel's pixel pointer changes.

### Meshes (glTF/GLB)

`SubmitMeshes(vector<MESH_DATA>)` carries the geometry of loaded glTF/GLB models.
Each `MESH_DATA` is one placed draw: a column-major world transform plus
**borrowed** pointers to flat vertex streams (position, optional normal/texcoord,
uint32 indices, optional `JOINTS_0` / `WEIGHTS_0`), an optional per-instance bone
palette (`pfBoneMatrix`, 16 floats per bone, cap 255), metallic-roughness PBR
factors, optional decoded RGBA8 base-color and emissive textures (each with
glTF `wrapS`/`wrapT`, default REPEAT), `bUnlit` (`KHR_materials_unlit` without MToon), `eAlpha` (`kOPAQUE` / `kMASK` / `kBLEND`) with `fAlphaCutoff` for MASK, and a stable instance
identity (`pInstanceOwner` = the scene `NODE*`, `nDrawIx` = slot in that node's
`GLTF_RENDER_MODEL::aMesh`). The caller owns the backing storage for the
lifetime of the submission (same contract as `PANEL_DATA`).

The producer of that backing storage is the **glTF→renderer bridge**
(`GltfMesh.cpp`): `Gltf_Render_Model_Build(DEP::GLTF_MODEL, matPlacement, out)`
takes a CPU `DEP::GLTF_MODEL` (from `deps/gltf`, see `Gltf.md`) and fills a
`GLTF_RENDER_MODEL`. It walks the default scene's node hierarchy, composing each
node's local transform under `matPlacement` and baking the result into every
**rigid** `MESH_DATA::mWorld`; decodes each base-color and emissive texture to RGBA8 via
`IMAGE::Decode`; **converts those RGB channels from sRGB to linear** (Halogen's `image2D`
sampler uploads `UFIXED8` as Filament `RGBA8` linear) and **bakes `baseColorFactor`
/ `emissiveFactor` into a per-material copy** when the factor is not white; an
authored emissive map that fails to decode is not replaced by the raw factor
(Sketchfab often authors `emissiveFactor [1,1,1]` with a nearly-black map);
**promotes OPAQUE
materials to MASK** when the albedo PNG has both near-zero and near-one
alpha (UniVRM often leaves cutout decals marked OPAQUE); **flips UV V in place** on each primitive (glTF V=0-at-top ->
ANARI V=0-at-bottom) so every `Mesh_Emit` of that primitive shares one texcoord
pointer; **merges same-material primitives within each mesh** (compatible
attribute sets only — same normals/UVs/joints presence) into one concatenated
surface so kit-style glTFs issue one draw per material in that mesh, not one
per source primitive; and computes a world-space AABB from each primitive's
8-corner bounds (`vCenter`/`dRadius`) so the compositor can frame the model.
**Skinned** primitives (`JOINTS_0` / `WEIGHTS_0` plus a node `nSkin`) keep
rest-pose positions and normals. Before palettes are packed, `VRMC_node_constraint`
records on the CPU model are evaluated in model space: aim constraints rotate the
destination so `nAxis` points at `nSource` (this changes bind pose); rotation and
roll copy a delta from rest and are a no-op at bind. Bind-pose palettes (`jointGlobal x inverseBind`)
are packed into `aBonePalette` (one vector per skin, 16 floats per bone). Authored
rest local transforms are packed into `aRest` (one matrix per node, composed from
TRS). The
skinned mesh node's own transform is ignored (glTF); `mWorld` is only the Y-up
conversion so GPU skinning runs in model space and the instance transform then
converts to Sneeze world. `Gltf_Render_Model_Pose` samples a clip at time
`dTime` from authored rest TRS (LINEAR / STEP / CUBICSPLINE on translation,
rotation, and scale), re-applies `VRMC_node_constraint` against `aRest` (two
passes: globals then every constraint), and writes packed palettes without
mutating `render.model`. The node tree used while posing is a caller-owned
workspace: children are copied when its size disagrees with the model, then
only TRS is reset each tick. The 4-argument overloads allocate a scratch tree
(tests). Morph weights
and unskinned node motion (baked `MESH_DATA::mWorld`) are not posed. A pose
change writes a new palette (`NODE::BonePalette` / `NODE::Animation_Tick`)
and the compositor overlays that pointer onto the submitted `MESH_DATA`; vertex
buffers are not rewritten. Within-mesh merge runs on the CPU model **before**
emit so two nodes that instance the same rigid mesh still share vertex
pointers. After emit, skinned same-material primitives from different meshes
are concatenated (they already share joint space) so a VRM split into many
meshes does not issue one Filament renderable per mesh-split. Rigid draws on
different meshes are not merged. The `GLTF_RENDER_MODEL` owns the source
model, decoded textures, rest transforms, bind palettes, and any concatenated
skinned streams; its `aMesh` entries borrow into that storage, so the model
must outlive any frame that submits its meshes.

A built model is stored on the **NODE** (`Gltf_Render_Model` get/set). Nodes that
load the same resolved URL share one CPU model via a process-wide refcounted
cache (`Gltf_Render_Model_Acquire` / `Publish` / `Release`). Each node copies
`aBonePalette` at attach so two instances of the same URL can pose independently,
and keeps a working `GLTF_NODE` tree for pose (TRS reset each tick, children
copied once).
`NODE::Animation_Tick` loops clip 0 of that node's model (internal clock, dt
clamped to 0.25 s) into the node's palettes, or a retargeted VRMA clip when
`Resource.aSupplementary` `"vrma"` has loaded; it is a no-op when the model has
no clip or no skins, and the compositor skips it while unique GPU geometry is
still streaming in so pose CPU does not share those hitchy first frames. The compositor calls it before emitting `aMesh`, stamps
`pInstanceOwner` + `nDrawIx` so two nodes sharing CPU buffers still get two
placed identities, and substitutes the node's live palette for skinned draws.

The ANARI backend uploads **one** `"triangle"` geometry and **one**
material/surface/group per unique primitive (keyed by vertex
pointers + counts, including joints/weights, then texture pointer + PBR factors
+ `bUnlit` + `eAlpha`). Unlit draws (`KHR_materials_unlit` without MToon) use Halogen `"unlit"`
(`color` = sampler or vec4). MToon and everything else use `"physicallyBased"`
(`baseColor` / metallic / roughness / emissive, each of `baseColor` and `emissive`
a sampler when the corresponding map is present; wrap is ANARI `wrapMode1` /
`wrapMode2` from the glTF sampler, REPEAT when the texture omits one). MASK sets Halogen `alphaMode`
`"mask"` and `alphaCutoff`; BLEND sets `"blend"`. VRM face/hair decals are usually
MASK cutouts -- without that, the PNG's black RGB in transparent texels draws
as solid black. After decode, an OPAQUE material whose albedo PNG has both
near-zero and near-one alpha is promoted to MASK (UniVRM often leaves
`alphaMode` OPAQUE on cutouts).
Skinned geometry sets vendor `vertex.joint` (`ANARI_UINT32_VEC4`) and
`vertex.weight` (`ANARI_FLOAT32_VEC4`). Each placed node shares **one**
`bone.matrix` array per skin (`pInstanceOwner` + `nSkin`, cap 255 bones), so
clothing/face/hair draws of the same skeleton upload one palette. Those same
draws also share **one** `ANARIInstance`: `SyncMeshes` clusters skinned
submit entries by owner+skin, borrows each unique surface from `mapGroup`, and
puts them in one multi-surface group. Rigid draws stay one instance per
primitive. Halogen advertises skinning as `HALOGEN_GEOMETRY_SKINNING` and
applies palettes with Filament `setBones`. `World::finalize` creates one
Filament entity per surface. Adding copies rebinds the world instance array;
when that list only grew, Halogen appends entities for the new copies and
leaves the existing crowd in the scene. Group-surface growth on a kept
instance (the first VRM filling in unique draws) also appends Filament
entities. A shrink or reorder still rebuilds. Halogen flushes Filament's command stream during
skinned creates and sizes the engine command arena for crowds
(`minCommandBufferSizeMB` 32, `driverHandleArenaSizeMB` 64). A transform-only
instance commit does not re-upload palettes. `UpdateScene` memcmp's each unique
palette, maps that array once, then unset/sets `bone.matrix` on the one
instance per skeleton so Halogen calls `setBones`. Placement uses the same
instance commit (`setTransform`). Neither path rebinds the world's instance
array. Base-color textures are
uploaded once per unique CPU pixel pointer (`mapTexture`) and held by the shared
group. New unique geometry is **admitted** four uploads per frame while any unique
mesh is still pending (`nAdmitGeometry = 4`, matching texture cap). Skinned copies
stay at **one instance per create frame**. After any geometry, instance, or
texture create, `EndFrame` skips `anariRenderFrame` and presents on the next
tick so Filament does not stack `Builder.skinning` work until presents drop
to 1 Hz. A skipped present stays present-only until one lands. Unique texture
uploads track the geometry cap. While unique
geometry or copies are still streaming, the compositor **skips `Animation_Tick`**
(nodes keep the bind-pose palettes copied at attach); pose starts on the first
frame after `Mesh_Streaming()` is false. `VIEWPORT::Mesh_Notify` (fetch thread, when
a node publishes a model) covers the first of those frames before the
renderer has counted pending unique meshes.
`SyncMeshes` matches by instance identity (owner+skin for skinned batches,
owner+`nDrawIx` for rigid). The first copy of a skeleton grows as unique
geometry uploads; further copies wait until every surface is already
resident, then create one complete instance. Draws not yet admitted stay off
the GPU until a later frame if they are still submitted. Growing a skinned
group patches that group's surface list in place (no new instance, no world
rebind). `EndFrame` ages retired ANARI objects two presented frames before
`anariRelease`. Albedo maps
larger than 1024 on a side are box-filtered down at CPU build so GPU copies
are 4-16x smaller. Mesh instance
transforms are patched each frame in `UpdateScene` by
that same identity key (one hash lookup per resident instance, not a scan of
the submit list), not by vertex pointer -- instance commit only, no world
rebind. A full sphere/curve rebuild still goes
through `BuildScene`, which uses the same capped `SyncMeshes` for meshes.

## RENDERER::ANARI

Concrete ANARI backend. Constructor takes library name (e.g. `"halogen"`).
Scene retention: ANARI objects created once via `BuildScene()`, updated via
`UpdateScene()`. `SceneNeedsRebuild()` detects structural changes (sphere/curve
counts, texture presence). When there is no geometry, `BuildScene()` clears the
world's `"instance"` parameter so a transition to an empty scene leaves nothing
on screen. Timing exposed via `GetLastSubmitSeconds()` / `GetLastRenderSeconds()`.

Destructor: `ReleaseScene()` only queues an empty world. Filament drops
Renderables on `anariRenderFrame`, so teardown renders that empty frame
(`ANARI_WAIT`) and `DrainRetired()` before releasing the native surface and
device. Skipping that flush after a heavy mesh fabric leaves the HWND's
swapchain alive; the next context's `nativeSurface` on the same window comes
up blank.

### Scene Invalidation

`UpdateScene()` only refreshes transforms, bone palettes, and position/radius
arrays -- instance transform and bone commits do not rebind the world -- and
it does not notice content changes (colors, materials) when the
structure is unchanged.
When the whole scene is swapped (e.g. `SCENE::Url()` loads a different fabric),
the renderer must rebuild from scratch instead of updating stale objects.

`RENDERER::InvalidateScene()` (virtual on the abstract base) sets a dirty flag;
the next `EndFrame()` releases and rebuilds the scene, then clears the flag.
The flag is delivered across threads: SCENE (UI thread) calls
`VIEWPORT::Scene_Invalidate()`, the compositor agent reads it via
`VIEWPORT::Scene_Invalidate_Consume()` before traversal (so learned extents
from the previous fabric are discarded) and forwards to `InvalidateScene()`.

## VIEW (Camera Orbit)

Struct declared in `include/Viewport.h`. Each VIEWPORT owns one VIEW.

```cpp
VIEWPORT::VIEW& view = pViewport->View ();
```

| Input | Action |
|-------|--------|
| Left drag | Orbit (rotate theta/phi) |
| Right drag | Pan (translate target) |
| Scroll wheel | Zoom (adjust distance) |

Spherical-to-Cartesian conversion from `dTheta`, `dPhi`, `dDistance` looking
at `(dTargetX, dTargetY, dTargetZ)`. Zoom distance is clamped to
`[MIN_DISTANCE, MAX_DISTANCE]` = `[0.001, 1e14]`. Units are the compositor's
per-scene render space, not AU (see Control.md "Rendering pipeline / scaling" —
each scene's root-anchored bounding sphere is fitted to `TARGET_EXTENT` = 5.0
render units). `MOUSE_SENSITIVITY` is `0.0025` and `SCROLL_FACTOR` is `1.075`
(both halved from earlier values to make orbit/zoom less jumpy).

## INPUT

POD struct accumulating raw input state per viewport: mouse deltas, scroll,
button state, key state. Written by the host application via `Input_Mouse()` / `Input_Key()`.
Consumed by `Input_Consume()` (resets accumulated deltas). Protected by
`m_mxInput` (std::mutex).

## UV_SPHERE

`GenerateUVSphere(nStacks, nSlices)` produces a UV_SPHERE struct with
positions, normals, texcoords, and indices for a unit sphere. Used by the
ANARI renderer for textured planet rendering.

## Files

| File | Contents |
|------|----------|
| `Viewport.cpp` | VIEWPORT::Impl (activate/deactivate, input, framebuffer, timing) |
| `Viewport.h` | Private header — RENDERER base, SPHERE_DATA, CURVE_DATA, BOX_DATA, PANEL_DATA, MESH_DATA, GLTF_RENDER_MODEL, Gltf_Render_Model_Build / Pose / Acquire / Publish / Release, CAMERA_DATA, UV_SPHERE |
| `AnariRenderer.h` | RENDERER::ANARI declaration |
| `AnariRenderer.cpp` | ANARI implementation (device, scene retention, native surface, shared mesh geometry/group, shared per-node-per-skin bone array + per-draw instance, sphere/box/curve/panel entries) |
| `GltfMesh.cpp` | glTF->renderer bridge: `Gltf_Render_Model_Build` (hierarchy flatten, UV flip in place, same-material primitive merge, bind-pose constraints, rest-transform cache, GPU-skin palettes, sRGB-to-linear albedo + factor bake, AABB-corner bounds), `Gltf_Render_Model_Pose` (workspace TRS reset, clip sample, two constraint passes, packed palettes), `Gltf_Vrma_Retarget` (VRMA humanoid clip onto a dest VRM), and the URL cache |
| `UVSphere.cpp` | GenerateUVSphere implementation |
