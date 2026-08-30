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
| `MESH_DATA` | One drawable glTF surface: column-major `mWorld`, borrowed vertex streams (position/normal/texcoord + uint32 indices), metallic-roughness PBR factors, and an optional borrowed decoded RGBA8 base-color texture |
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
instance transforms are patched every frame in `UpdateScene`, so a billboarded
panel tracks the camera without a rebuild; a rebuild is triggered only when the
panel **count** or a panel's pixel pointer changes.

### Meshes (glTF/GLB)

`SubmitMeshes(vector<MESH_DATA>)` carries the geometry of loaded glTF/GLB models.
Each `MESH_DATA` is one placed draw: a column-major world transform plus
**borrowed** pointers to flat vertex streams (position, optional normal/texcoord,
uint32 indices), metallic-roughness PBR factors, an optional decoded RGBA8
base-color texture, and a stable instance identity (`pInstanceOwner` = the scene
`NODE*`, `nDrawIx` = slot in that node's `GLTF_RENDER_MODEL::aMesh`). The caller
owns the backing storage for the lifetime of the submission (same contract as
`PANEL_DATA`).

The producer of that backing storage is the **glTF→renderer bridge**
(`GltfMesh.cpp`): `Gltf_Render_Model_Build(DEP::GLTF_MODEL, matPlacement, out)`
takes a CPU `DEP::GLTF_MODEL` (from `deps/gltf`, see `Gltf.md`) and fills a
`GLTF_RENDER_MODEL`. It walks the default scene's node hierarchy, composing each
node's local transform under `matPlacement` and baking the result into every
emitted `MESH_DATA::mWorld`; decodes each base-color texture to RGBA8 via
`IMAGE::Decode`; **flips UV V in place** on each primitive (glTF V=0-at-top →
ANARI V=0-at-bottom) so every `Mesh_Emit` of that primitive shares one texcoord
pointer; **merges same-material primitives within each mesh** (compatible
attribute sets only — same normals/UVs presence) into one concatenated
surface so kit-style glTFs issue one draw per material in that mesh, not one
per source primitive; and computes a world-space AABB from each primitive's
8-corner bounds (`vCenter`/`dRadius`) so the compositor can frame the model.
Merge runs on the CPU model **before** emit so two nodes that instance the
same mesh still share vertex pointers. Different meshes and different node
transforms are not merged (that would bake instancing away). The
`GLTF_RENDER_MODEL` owns the source model and the decoded textures; its `aMesh`
entries borrow into that storage, so the model must outlive any frame that
submits its meshes.

A built model is stored on the **NODE** (`Gltf_Render_Model` get/set). Nodes that
load the same resolved URL share one CPU model via a process-wide refcounted
cache (`Gltf_Render_Model_Acquire` / `Publish` / `Release`). The compositor emits
each node's `aMesh` at that node's world frame, stamping `pInstanceOwner` +
`nDrawIx` so two nodes sharing CPU buffers still get two ANARI instances.

The ANARI backend uploads **one** `"triangle"` geometry and **one**
`"physicallyBased"` material/surface/group per unique primitive (keyed by vertex
pointers + counts, then texture pointer + PBR factors). Each placed draw is an
`ANARIInstance` with its own transform. Base-color textures are uploaded once per
unique CPU pixel pointer (`mapTexture`) and held by the shared group. New unique
geometry is **admitted** a few uploads per frame (`MAX_MESH_CREATES_PER_FRAME`);
instance-only creates of an already-resident primitive are capped separately
(`MAX_MESH_INSTANCES_PER_FRAME`) so a repeated model is not treated as N GPU
uploads. Unique texture uploads stay at `MAX_TEXTURE_UPLOADS_PER_FRAME`.
`SyncMeshes` matches by instance identity, keeps already-resident draws, and
retires anything absent from the list. Draws not yet admitted stay off the GPU
until a later frame if they are still submitted. Mesh instance transforms are
patched each frame in `UpdateScene` by `pInstanceOwner`+`nDrawIx`, not by vertex
pointer. A full sphere/curve rebuild still goes through `BuildScene`, which uses
the same capped `SyncMeshes` for meshes.

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

`UpdateScene()` only refreshes transforms and position/radius arrays — it does
not notice content changes (colors, materials) when the structure is unchanged.
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
| `Viewport.h` | Private header — RENDERER base, SPHERE_DATA, CURVE_DATA, BOX_DATA, PANEL_DATA, MESH_DATA, GLTF_RENDER_MODEL, Gltf_Render_Model_Build / Acquire / Publish / Release, CAMERA_DATA, UV_SPHERE |
| `AnariRenderer.h` | RENDERER::ANARI declaration |
| `AnariRenderer.cpp` | ANARI implementation (device, scene retention, native surface, shared mesh geometry/group + per-draw instance, sphere/box/curve/panel entries) |
| `GltfMesh.cpp` | glTF→renderer bridge: `Gltf_Render_Model_Build` (hierarchy flatten, UV flip in place, same-material primitive merge, texture decode, AABB-corner bounds) and the URL cache |
| `UVSphere.cpp` | GenerateUVSphere implementation |
