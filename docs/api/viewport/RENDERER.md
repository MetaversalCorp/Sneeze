---
title: "VIEWPORT::RENDERER (class reference)"
tier: API
audience: [integrator, contributor]
sources:
  - include/Viewport.h
  - src/context/viewport/Viewport.h
  - src/context/viewport/AnariRenderer.h
  - src/context/viewport/AnariRenderer.cpp
  - src/context/viewport/GltfMesh.cpp
verified: b487fd1
nav:
  prev: api/viewport/VIEWPORT.md
  next: api/msf/index.md
---

# `VIEWPORT::RENDERER`

> **Internal / abstract class.** `VIEWPORT::RENDERER` is *forward-declared* in the > public header (`include/Viewport.h`) but **defined privately** in > `src/context/viewport/Viewport.h`; its only concrete implementation, > `RENDERER::ANARI`, lives entirely in `src/context/viewport/AnariRenderer.*`. It > is not part of the surface an application links against and its full definition > is not visible outside the engine. It is documented here because it is essential > to understanding [`VIEWPORT`](VIEWPORT.md): the renderer is the abstraction that > the entire viewport design exists to feed, and contributors working on rendering > need its contract spelled out.

`RENDERER` is the abstract interface every rendering backend implements. It models one frame as a fixed sequence — set the camera, optionally set the backdrop and the lights, begin, submit geometry, end, read back — over a backend the viewport never names directly. The engine ships one implementation, `RENDERER::ANARI`, built on the ANARI rendering abstraction. For the conceptual picture see the [Viewport system](../../systems/viewport.md); this page is the exact interface.

The interface has two kinds of method. The **required** ones are pure virtual (`= 0`) — a backend must implement them. The rest have a default no-op body, so a minimal backend can render spheres and curves and ignore the richer geometry (boxes, panels, meshes), the backdrop, and the lights; the ANARI backend overrides all of them.

```cpp
class VIEWPORT::RENDERER
{
public:
   class ANARI;                 // the concrete implementation (private)

   virtual ~RENDERER () = default;

   virtual void SetNativeWindow (void* pHandle) { (void) pHandle; }
   virtual bool IsRenderingToNativeSurface () const { return false; }

   virtual bool Initialize (int nWidth, int nHeight) = 0;
   virtual void Resize (int nWidth, int nHeight) = 0;

   virtual void SetCamera     (const CAMERA_DATA& pCamera) = 0;
   virtual void SetBackground (float dRed, float dGreen, float dBlue, float dAlpha) {}
   virtual void SetLights        (const std::vector<LIGHT_DATA>& aLight_Data) {}
   virtual void SetSceneLighting (const SCENE_LIGHT& Ambient, const SCENE_LIGHT& Directional) {}
   virtual void BeginFrame    () = 0;
   virtual void SubmitSpheres (const std::vector<SPHERE_DATA>& aSphere_Data) = 0;
   virtual void SubmitCurves  (const std::vector<CURVE_DATA>&  aCurve_Data)  = 0;
   virtual void SubmitBoxes   (const std::vector<BOX_DATA>&    aBox_Data)   {}
   virtual void SubmitPanels  (const std::vector<PANEL_DATA>&  aPanel_Data) {}
   virtual void SubmitMeshes  (const std::vector<MESH_DATA>&   aMesh_Data)  {}
   virtual void EndFrame () = 0;

   virtual void InvalidateScene () {}

   virtual const uint32_t* GetFrameBuffer () const = 0;
   virtual int GetWidth () const = 0;
   virtual int GetHeight () const = 0;

   virtual double GetLastSubmitSeconds () const { return 0.0; }
   virtual double GetLastRenderSeconds () const { return 0.0; }
};
```

There is deliberately **no tonemapping control** on the interface. A `SetToneMapping` toggle was tried and removed; tone mapping is a fixed property of the backend, not something the viewport or a host can flip.

---

## Role and ownership

- **Owned by** the [`VIEWPORT`](VIEWPORT.md) (its private implementation), created by `VIEWPORT::Renderer_Initialize` and destroyed by `Renderer_Shutdown`.
- **Driven by** the engine's compositor agent, which calls the frame methods in a fixed order each frame. Nothing else touches it.
- **Single-thread-affine.** Every method — construction, the frame sequence, and destruction — runs on compositor agent 0 (creation/destruction) or the compositor pool (per-frame). The backend crashes if used from multiple threads.

---

## Threading and pitfalls

**Strict single-thread use.** The renderer must be created, called, and destroyed on the compositor's lifecycle thread. The viewport guarantees this; a contributor adding a backend must preserve it. The constraint is a hard property of the underlying device, not a convention.

**The frame methods are an ordered protocol.** Each frame the compositor calls `SetCamera`, then `SetLights` and `SetSceneLighting` (and `SetBackground` when the scene reports a new backdrop), then `BeginFrame` → `SubmitSpheres` → `SubmitCurves` → `SubmitBoxes` → `SubmitPanels` → `SubmitMeshes` → `EndFrame`. After `EndFrame` the framebuffer (readback path) is valid. `BeginFrame` clears the backend's submission lists; submitting outside a begin/end pair is undefined. `SetCamera`, `SetLights`, `SetSceneLighting`, and `SetBackground` act on retained state and sit outside the begin/end pair.

**The framebuffer pointer is only valid on the readback path.** `GetFrameBuffer` returns the mapped pixels when *not* rendering to a native surface, and null when it is. Check `IsRenderingToNativeSurface()` (or simply a null return) before reading.

**Retained scene + invalidation.** The concrete backend retains its scene across frames for speed and only refreshes transforms on a normal frame. Structural changes trigger a rebuild automatically — a changed count of spheres, curves, boxes, panels, or meshes; a sphere gaining or losing its texture; a panel's pixel pointer changing; a mesh's vertex or texture pointer changing; or the light count changing. Whole-scene swaps must be signalled with `InvalidateScene`, which the compositor forwards from [`VIEWPORT::Scene_Invalidate`](VIEWPORT.md#scene-invalidation).

---

## Setup methods

### `virtual void SetNativeWindow (void* pHandle)`
- **Purpose.** Provide a native window handle to render directly into, before `Initialize`. The base does nothing; a backend that supports direct presentation stores the handle.
- **Parameters.** `pHandle` — an opaque platform window handle.

### `virtual bool IsRenderingToNativeSurface () const`
- **Purpose.** Report whether the backend is presenting directly to a native surface (true) or rendering offscreen for readback (false).
- **Returns.** `false` from the base; the ANARI backend returns `true` only when a native window was supplied *and* the device advertised native-surface support at initialization.

### `virtual bool Initialize (int nWidth, int nHeight) = 0`
- **Purpose.** Bring the backend up at the given size (load the device, create the camera/world/renderer/frame, opt into a native surface if available).
- **Parameters.** Initial drawable dimensions.
- **Returns.** `true` on success.

### `virtual void Resize (int nWidth, int nHeight) = 0`
- **Purpose.** Re-size the render target.
- **Parameters.** New dimensions.

---

## Camera, backdrop, and lights

### `virtual void SetCamera (const CAMERA_DATA& pCamera) = 0`
- **Purpose.** Set the camera for the next frame from a `CAMERA_DATA` (eye position, look direction, up vector, vertical FOV, aspect, near/far).

### `virtual void SetBackground (float dRed, float dGreen, float dBlue, float dAlpha)`
- **Purpose.** Set the backdrop colour the frame clears to. The base does nothing; the ANARI backend sets the renderer's `background` parameter. The compositor calls this only when the [scene](../scene/index.md) reports a new backdrop (`SCENE::Background_Consume`), so an unchanged backdrop costs nothing.
- **Parameters.** Straight RGBA components in `[0, 1]`.

### `virtual void SetLights (const std::vector<LIGHT_DATA>& aLight_Data)`
- **Purpose.** Replace the frame's set of **placed** lights — point and spot lights that live at a position in the scene. The base does nothing; the ANARI backend stores the vector and, when its size changes, marks the scene dirty so the lights are rebuilt. Each `LIGHT_DATA` carries a type (`kPOINT` / `kSPOT`), a position, an RGB colour, an intensity, and — for a spot — an aim direction and cone angles. Scene-global ambient and directional light are **not** carried here; they arrive on their own channel via `SetSceneLighting`. See [Lighting](#lighting) for how the backend turns each into an ANARI light.

### `virtual void SetSceneLighting (const SCENE_LIGHT& Ambient, const SCENE_LIGHT& Directional)`
- **Purpose.** Set the scene-global ambient and directional ("sun") light. These are properties of the whole scene, authored by the primary fabric (see [`SCENE::Ambient`](../scene/SCENE.md) / `SCENE::Directional`), not placed objects in the graph — so a local object can never alter global illumination. The base does nothing; the ANARI backend stores both `SCENE_LIGHT`s, marking the scene dirty when either changes. `Ambient` feeds the renderer's own ambient term (`ambientColor` / `ambientRadiance`); `Directional` builds a single ANARI `"directional"` light. Either is omitted when its `fIntensity` is zero. See [Lighting](#lighting).

## Per-frame methods

### `virtual void BeginFrame () = 0`
- **Purpose.** Start a frame; clears the pending geometry submission lists.

### `virtual void SubmitSpheres (const std::vector<SPHERE_DATA>& aSphere_Data) = 0`
- **Purpose.** Append spheres to the frame. A `SPHERE_DATA` carries position, radius, RGB color, an optional texture (pixels + dimensions), and an emissive flag. Used for celestial bodies.

### `virtual void SubmitCurves (const std::vector<CURVE_DATA>& aCurve_Data) = 0`
- **Purpose.** Append curves (polylines) to the frame. A `CURVE_DATA` is a list of `CURVE_POINT`s (position + per-point radius) plus an RGB color, drawn as tubes (used for orbit trails).

### `virtual void SubmitBoxes (const std::vector<BOX_DATA>& aBox_Data)`
- **Purpose.** Append oriented boxes to the frame. Each `BOX_DATA` is a column-major world transform (with the box dimensions and pivot baked in) plus an RGB colour, rendered as a shared unit cube instanced per box. The base does nothing. Used as the fallback visual for a physical node that carries no model.

### `virtual void SubmitPanels (const std::vector<PANEL_DATA>& aPanel_Data)`
- **Purpose.** Append in-scene UI panels — textured, alpha-blended quads. Each `PANEL_DATA` is a column-major world transform (quad size baked in) plus a straight-alpha RGBA8 pixel buffer and its dimensions. The base does nothing; the backend draws each as an unlit, blended quad so the panel shows its true pixels regardless of scene lighting. The pixel pointers are borrowed and must outlive the frame.

### `virtual void SubmitMeshes (const std::vector<MESH_DATA>& aMesh_Data)`
- **Purpose.** Append the drawable surfaces of loaded glTF/GLB models. Each `MESH_DATA` is one indexed triangle mesh: a column-major world transform, borrowed vertex streams (position, optional normal/texcoord, uint32 indices), metallic-roughness material factors, and an optional decoded RGBA8 base-colour texture. The base does nothing; the backend builds one physically-based surface per mesh. All the borrowed pointers must outlive the frame.

### `virtual void EndFrame () = 0`
- **Purpose.** Build or update the retained scene from the submitted geometry and lights, render the frame, and (on the readback path) map the pixels back to CPU memory. After this call the framebuffer is available.

### `virtual void InvalidateScene ()`
- **Purpose.** Force a full scene rebuild on the next `EndFrame` (used after a whole-scene swap). The base does nothing; a retaining backend sets a dirty flag.

---

## Framebuffer and dimensions

### `virtual const uint32_t* GetFrameBuffer () const = 0`
- **Purpose.** Return the last frame's RGBA pixels for readback.
- **Returns.** The pixel pointer on the readback path; null when rendering to a native surface.

### `virtual int GetWidth () const = 0` / `virtual int GetHeight () const = 0`
- **Purpose / Returns.** The render target's current width and height.

---

## Timing

### `virtual double GetLastSubmitSeconds () const` / `virtual double GetLastRenderSeconds () const`
- **Purpose.** Report the wall-clock seconds the last frame spent building/updating the scene (submit) and rendering it (render), for the viewport's diagnostics.
- **Returns.** `0.0` from the base; real values from a backend that measures them.

---

## The concrete implementation: `RENDERER::ANARI`

`RENDERER::ANARI` (in `AnariRenderer.h/.cpp`) is the engine's only backend. Its constructor takes the owning `ENGINE*` and the ANARI library name (e.g. `"halogen"`); `VIEWPORT::Renderer_Initialize` reads that name from the engine host. It implements the interface above over the ANARI API backed by a PBR device. Key internal behavior a contributor should know:

- **Scene retention.** ANARI objects (geometry, materials, surfaces, instances, the world, the lights) are created once in an internal `BuildScene` and kept across frames. A normal frame calls `UpdateScene`, which only refreshes instance transforms and curve positions. A frame triggers a full `ReleaseScene`/`BuildScene` when the scene is dirty (`InvalidateScene` or a light-count change) or when `SceneNeedsRebuild` detects a structural change (counts of any geometry kind, a sphere's texture presence or pointer, a panel's pixel pointer, a mesh's vertex or texture pointer).
- **Two presentation paths.** With a native window and device support it renders directly to a swapchain (`IsRenderingToNativeSurface()` true, no readback); otherwise it renders to an offscreen ANARI frame and maps `channel.color` back into a CPU pixel buffer that `GetFrameBuffer` exposes.
- **Textured vs. analytic spheres.** A textured sphere is built as a UV-mapped triangle mesh (a shared unit sphere generated once by `GenerateUVSphere`) with per-vertex colors sampled from the texture and cached by pixel pointer; an untextured sphere uses ANARI's analytic `"sphere"` geometry. Emissive spheres (stars) brighten their sampled colors.
- **Boxes.** Each box is a shared unit cube (`GenerateUnitBox`) instanced with the box's baked transform and a physically-based material tinted by its colour.
- **Panels.** Each panel is a shared unit quad, double-sided, textured from its pixel buffer through an `image2D` sampler, with the Halogen **unlit** material in `"blend"` mode so the panel reads as flat UI regardless of lighting.
- **Meshes.** Each glTF/GLB mesh becomes a `"triangle"` geometry and a `"physicallyBased"` material; a base-colour texture, when present, feeds an `image2D` sampler bound to the material.
- **Empty scenes.** When no geometry is submitted, the world's instance parameter is unset so the screen clears rather than retaining stale objects.
- **No tonemapping toggle.** The backend never exposes a tone-mapping switch; tone mapping is a fixed property of the device.

`RENDERER::ANARI` forward-declares the ANARI handle types it stores, so including its header does not drag the ANARI SDK into every translation unit.

## Lighting

Lighting arrives on two channels. **Placed lights** come through `SetLights` as a vector of `LIGHT_DATA`; **scene-global ambient and directional** come through `SetSceneLighting` as two `SCENE_LIGHT`s. `BuildScene` handles them separately.

Placed lights — `BuildScene` turns each `LIGHT_DATA` into an ANARI light by switching on its `eType`:

- `kPOINT` → an ANARI `"point"` light (`position`, `color`, `intensity`).
- `kSPOT` → an ANARI `"spot"` light (`position`, `direction`, `color`, `intensity`, `openingAngle`, `falloffAngle`).

The compositor fills that vector from the scene: every star contributes a point light at its world position, and explicit light nodes ([`MAP_OBJECT_LIGHT`](../scene/index.md)) contribute a point or spot light per their subtype. (Ambient and directional are no longer light *nodes* — they are scene properties, below.)

Scene-global lighting — the two `SCENE_LIGHT`s from `SetSceneLighting` are the scene's own ambient and directional ("sun"), authored by the primary fabric:

- **Ambient** feeds the renderer's ambient term (`ambientColor` / `ambientRadiance`) rather than a separate ANARI light object (Halogen ignores an ANARI `"ambient"` light).
- **Directional** builds one ANARI `"directional"` light (`direction`, `color`, `irradiance`).

Each is omitted when its `fIntensity` is zero, so a scene with no authored global light simply has none — scene lighting is authoritative and there is no automatic fallback. A primary fabric that wants light authors an ambient or directional in its `"primary"` block; when neither is authored, `SCENE` defaults the ambient to full-intensity white.

---

## Data types

These structs (declared in the private `Viewport.h`) are the renderer's vocabulary for a frame.

| Type | Fields |
|---|---|
| `CAMERA_DATA` | Eye position, look direction, up vector, vertical FOV, aspect, near, far. |
| `LIGHT_DATA` | A placed (point/spot) light: `eType` (`kNONE` / `kPOINT` / `kSPOT`), a world position `vPosition` (`VEC3`), an RGB colour `rgbColor` (`RGB`), `fIntensity`, and — for a spot — an aim `vDirection` (`VEC3`) plus `fOpeningAngle` / `fFalloffAngle` (radians). |
| `SCENE_LIGHT` | A scene-global ambient or directional light: `rgbColor` (`RGB`), `fIntensity` (ambient radiance / directional irradiance), and `vDirection` (`VEC3`, directional only). Declared in [`Scene.h`](../scene/SCENE.md). |
| `SPHERE_DATA` | Center `x,y,z`, radius, RGB color, optional texture (pixels + width/height), emissive flag. |
| `CURVE_POINT` | Position `x,y,z` and radius. |
| `CURVE_DATA` | A vector of `CURVE_POINT`s and an RGB color. |
| `BOX_DATA` | A column-major world transform `mWorld` (dimensions and pivot baked in) and an RGB colour. |
| `PANEL_DATA` | A column-major world transform `mWorld` (size baked in), a borrowed straight-alpha RGBA8 pixel buffer, and its width/height. |
| `MESH_DATA` | One glTF/GLB surface: column-major `mWorld`, borrowed vertex streams (position, optional normal/texcoord, uint32 indices, vertex count), metallic-roughness factors (`rgbaBaseColor`, `fMetallic`, `fRoughness`, `rgbEmissive`), and an optional borrowed decoded RGBA8 base-colour texture. |
| `GLTF_RENDER_MODEL` | A loaded model prepared for rendering: owns the source `DEP::GLTF_MODEL` and the decoded textures, holds the flattened `aMesh` draw list (one `MESH_DATA` per primitive, node hierarchy baked into each `mWorld`), and a model-space bounding sphere (`vCenter`, `dRadius`). Built by `Gltf_Render_Model_Build`. |
| `UV_SPHERE` | Generated unit-sphere (or unit-box) mesh: positions, normals, texcoords, indices. |

`GLTF_RENDER_MODEL` and its builder `Gltf_Render_Model_Build (DEP::GLTF_MODEL, const MAT4& matPlacement, GLTF_RENDER_MODEL& out)` are the glTF→renderer bridge (`GltfMesh.cpp`): the builder flattens the model's default scene, decodes base-colour textures to RGBA8, resolves materials, and computes bounds. A model is stored on a [`MAP_OBJECT`](../scene/index.md) (any class may carry one) and the compositor submits its `aMesh` at the node's world frame. Because a `MESH_DATA` borrows into the model's storage, the `GLTF_RENDER_MODEL` must outlive any frame that submits it.

---

## See also

- [Viewport system](../../systems/viewport.md) — design, the frame loop, threading, limitations.
- [VIEWPORT](VIEWPORT.md) — owns and drives the renderer.
- [Scene API](../scene/index.md) — the model traversed to produce the per-frame `SPHERE_DATA` / `CURVE_DATA` / `BOX_DATA` / `PANEL_DATA` / `MESH_DATA` / `LIGHT_DATA`.

---

[Viewport API](index.md) · Prev: [VIEWPORT](VIEWPORT.md) · Next: [MSF API](../msf/index.md)
