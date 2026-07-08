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
├── GLTF_RENDER_MODEL + Gltf_Render_Model_Build (glTF→renderer bridge, GltfMesh.cpp)
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
| `BOX_DATA` | Column-major world transform (`m16`) + color |
| `PANEL_DATA` | Column-major world transform (`m16`, size baked in) + straight-alpha RGBA8 pixels + width/height |
| `MESH_DATA` | One drawable glTF surface: column-major `m16`, borrowed vertex streams (position/normal/texcoord + uint32 indices), metallic-roughness PBR factors, and an optional borrowed decoded RGBA8 base-color texture |
| `GLTF_RENDER_MODEL` | A loaded glTF prepared for rendering — owns the source `DEP::GLTF_MODEL`, the decoded textures, the flattened `aMesh` draw list, and a model-space bounding sphere (`aCenter`, `dRadius`) |
| `CAMERA_DATA` | Eye, look direction, up, FOV, aspect, near/far |
| `LIGHT_DATA` | One scene light: `eType` (`kAMBIENT`/`kDIRECTIONAL`/`kPOINT`/`kSPOT`), position-or-direction (`x,y,z`), colour (`r,g,b`), `dIntensity` (point/spot intensity / ambient radiance / directional irradiance), plus spot aim (`dirX,dirY,dirZ`) and cone (`dOpeningAngle`, `dFalloffAngle`, radians) |
| `UV_SPHERE` | Generated mesh: positions, normals, texcoords, indices |

Every drawable `*_DATA` (sphere/curve/box/panel/mesh) carries `nKey`: the stable
identity of the emitting scene node (its pointer value, stamped by the
compositor's traversal; glTF draws mix in the per-model draw index since one
node emits one draw per primitive). The ANARI backend keys its retained
per-entry state on it — see "Scene reconcile" below.

### Lighting

`SetLights(vector<LIGHT_DATA>)` supplies the frame's lights. The compositor fills
the vector from two sources — `STAR` celestial nodes and explicit
`MAP_OBJECT_LIGHT` nodes (colour, intensity, and subtype flattened per light; see
`Control.md` "Lighting") — and pushes it each frame. `ReconcileLights` (run from
`ReconcileScene` each frame) switches on each entry's `eType` and creates the
matching light:

- `kAMBIENT` → `"ambient"` (`color`, `radiance`)
- `kDIRECTIONAL` → `"directional"` (`direction`, `color`, `irradiance`)
- `kPOINT` → `"point"` (`position`, `color`, `intensity`)
- `kSPOT` → `"spot"` (`position`, `direction`, `color`, `intensity`, `openingAngle`, `falloffAngle`)

When the vector is empty (a scene with no star and no light nodes — e.g. a
planetary system loaded as the primary fabric with its sun in a parent fabric, or
a terrestrial scene like DFW) it falls back to **two** lights: an `"ambient"`
light (`radiance` `3.0`) plus a `"directional"` key light from above-front
(`direction` `{-0.4,-1.0,-0.3}`, `irradiance` `1.0`) so geometry reads with
shape. (Filament's ambient term alone is weak fill without an environment map,
and Halogen may not honor the ANARI `"ambient"` `radiance` at all — the explicit
directional light is what makes starless scenes legible.) Lights update
incrementally: a parameter change (position, colour, intensity) re-applies to the
retained light handle, followed by an unset/set nudge of the world's `"light"`
array to force the finalize; only a count or type change releases and recreates
the light set. Geometry entries are never touched by light changes.

### Panels

`SubmitPanels(vector<PANEL_DATA>)` carries in-scene UI panels (see `Ui_Context.md`
and `Scene.md` `MAP_OBJECT_PANEL`). Each `PANEL_DATA` is just a column-major world
transform (`m16`, size baked in) plus a straight-alpha RGBA8 pixel buffer and its
dimensions — the renderer stays UI-agnostic, treating a panel like a textured box.

The ANARI backend builds one instance per panel from a **shared unit quad** (XY
plane, `+Z` normal, `attribute0` UVs, double-sided so a panel turned away is not
culled; V is flipped vs. position so the top-down UI canvas reads upright). The
panel pixels become an `image2D` array feeding a sampler, and the material is the
**unlit** Halogen extension in `"blend"` mode (`color` = the sampler), so the
panel shows its true RGBA, lighting-independent, with per-texel alpha. Panel
instance transforms are patched per entry in `ReconcileScene`, so a billboarded
panel tracks the camera without any object churn; a panel whose pixel pointer or
canvas dimensions change is released and recreated individually, leaving every
other entry's ANARI objects untouched.

### Meshes (glTF/GLB)

`SubmitMeshes(vector<MESH_DATA>)` carries the geometry of loaded glTF/GLB models.
Each `MESH_DATA` is one drawable surface: a column-major world transform plus
**borrowed** pointers to flat vertex streams (position, optional normal/texcoord,
uint32 indices), metallic-roughness PBR factors, and an optional decoded RGBA8
base-color texture. The caller owns the backing storage for the lifetime of the
submission (same contract as `PANEL_DATA`).

The producer of that backing storage is the **glTF→renderer bridge**
(`GltfMesh.cpp`): `Gltf_Render_Model_Build(DEP::GLTF_MODEL, matPlacement, out)`
takes a CPU `DEP::GLTF_MODEL` (from `deps/gltf`, see `Gltf.md`) and fills a
`GLTF_RENDER_MODEL`. It walks the default scene's node hierarchy, composing each
node's local transform under `matPlacement` and baking the result into every
emitted `MESH_DATA::m16`; decodes each base-color texture to RGBA8 via
`IMAGE::Decode`; resolves materials; and computes a world-space AABB reduced to a
center + bounding-sphere radius (`aCenter`/`dRadius`) so the compositor can frame
the model. The `GLTF_RENDER_MODEL` owns the source model and the decoded
textures; its `aMesh` entries borrow into that storage, so the model must outlive
any frame that submits its meshes. A `GLTF_RENDER_MODEL` is stored on the
`MAP_OBJECT` (any class — celestial, terrestrial, or physical — may carry one;
see `Scene.md`), and the compositor emits its `aMesh` at the node's world frame.

The ANARI backend builds one `"triangle"` geometry + `"physicallyBased"` material
+ instance per `MESH_DATA`. When a mesh has a base-color texture, the pixels feed
an `image2D` sampler bound to the material. Mesh instance transforms are patched
per entry in `ReconcileScene`; a draw whose vertex pointer or texture pointer
changes (model reload, texture swap) is released and recreated individually —
the other draws keep their GPU buffers.

## RENDERER::ANARI

Concrete ANARI backend. Constructor takes library name (e.g. `"halogen"`).
Timing exposed via `GetLastSubmitSeconds()` / `GetLastRenderSeconds()`.

### Scene reconcile

Retained state lives in `SCENE_STATE`: one `unordered_map<nKey, *_ENTRY>` per
drawable type, each entry owning that drawable's ANARI objects (geometry,
material, sampler, instance) plus its last-committed values. `ReconcileScene()`
runs every `EndFrame()` and diffs the frame's submission against the maps:

- **Unknown key** → `*_Build` creates that entry's objects (per-type builders;
  shared unit sphere/box/quad arrays are created on first use by the
  `Unit*_Ensure` helpers).
- **Known key, identity changed** (mesh vertex/texture pointer, panel pixel
  pointer/dimensions, sphere textured-ness or texture pointer) → that entry
  alone is released and rebuilt.
- **Known key, identity unchanged** → delta update: transforms are memcmp'd
  against the last-committed `m16`, sphere centre/radius compared by value,
  curve control points fingerprinted (FNV-1a) — nothing is committed when
  nothing changed, so a static scene issues zero ANARI work.
- **Keys absent from the submission** are swept and their objects released.

Membership changes re-snapshot only cheap handle objects: the world
`"instance"` array is rebuilt in submission order, and when analytic
sphere/curve membership changed, the shared surface group **and its instance
are recreated** (never mutated in place — `Group::finalize` would drop removed
surfaces before `World::finalize` has destroyed the renderables that still use
their Filament material instances, tripping Filament's in-use assert; the
World's own references from its last finalize keep the old chain alive until
then). One `World::finalize`, no geometry churn for surviving entries. A pure
transform move instead nudges the world's instance array (unset/set of the
identical handle) because instances are not change-observed by Halogen's World.
When the scene empties, the world's `"instance"` parameter is cleared so
nothing stays on screen.

### Scene Invalidation

The reconcile does not notice content changes that leave identity and transform
intact (e.g. a sphere/box/curve colour edit, or panel pixel content redrawn into
the same buffer). When the whole scene is swapped (e.g. `SCENE::Url()` loads a
different fabric), the renderer must rebuild from scratch instead of updating
stale objects.

`RENDERER::InvalidateScene()` (virtual on the abstract base) sets a dirty flag;
the next `EndFrame()` releases and rebuilds the scene, then clears the flag.
The flag is delivered across threads: SCENE (UI thread) calls
`VIEWPORT::Scene_Invalidate()`, the compositor agent reads it via
`VIEWPORT::Scene_Invalidate_Consume()` and forwards to `InvalidateScene()`
before the frame.

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
| `Viewport.h` | Private header — RENDERER base, SPHERE_DATA, CURVE_DATA, BOX_DATA, PANEL_DATA, MESH_DATA, GLTF_RENDER_MODEL, Gltf_Render_Model_Build, CAMERA_DATA, UV_SPHERE |
| `AnariRenderer.h` | RENDERER::ANARI declaration |
| `AnariRenderer.cpp` | ANARI implementation (device, scene retention, native surface, mesh/sphere/box/curve/panel entries) |
| `GltfMesh.cpp` | glTF→renderer bridge: `Gltf_Render_Model_Build` (hierarchy flatten, texture decode, bounds) |
| `UVSphere.cpp` | GenerateUVSphere implementation |
