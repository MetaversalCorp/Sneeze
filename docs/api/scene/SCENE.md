---
title: SCENE (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Scene.h
  - src/context/scene/Scene.cpp
verified: b487fd1
nav:
  prev: api/scene/index.md
  next: api/scene/FABRIC.md
---

# `SCENE`

The root of the scene object model. One `SCENE` exists per [`CONTEXT`](../context/index.md) (per browsing session). It owns the root fabric and the scene-global **fabric table**, owns the page-wide **backdrop**, and drives **primary presentation** (the initial camera pose and background colour a primary fabric declares). The node handle table is *not* on the scene — it belongs to the [`CONTAINER`](../container/index.md). For the conceptual picture see the [Scene system](../../systems/scene.md) page; this page is the exact behavior of every public member.

```cpp
class SCENE
{
public:
   explicit SCENE (CONTEXT* pContext);
   ~SCENE ();

   bool Initialize (const std::string& sUrl);

   ENGINE*  Engine         () const;
   CONTEXT* Context        () const;
   NETWORK* Network        () const;
   FABRIC*  Fabric_Root    () const;
   FABRIC*  Fabric_Primary () const;
   RGBA        Background   () const;
   SCENE_LIGHT Ambient      () const;
   SCENE_LIGHT Directional  () const;

   bool Url         (const std::string& sUrl);
   void Background  (const RGBA& rgbaBackground);
   void Ambient     (const SCENE_LIGHT& Light);
   void Directional (const SCENE_LIGHT& Light);

   bool    Background_Consume (RGBA& rgbaBackground);
   void    Fabric_Spawn     (NODE* pNode_Attach, const std::string& sUrl);
   FABRIC* Fabric_Open      (NODE* pNode_Attach, MSF* pMsf, const std::string& sUrl);
   FABRIC* Fabric_Close     (FABRIC* pFabric);
   FABRIC* Fabric_Find      (uint64_t twFabricIx) const;

   void OnMsfReady  (NODE* pNode_Attach, FILE* pFile);
   void OnMsfFailed (NODE* pNode_Attach, FILE* pFile);

private:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Owned by** a `CONTEXT`, constructed with a back-pointer to it.
- **Owns** the root `FABRIC` (and, transitively through the loading cascade, every fabric, node, and map object in the scene).
- **Registers** every fabric by a scene-global index (`m_umpFabric`, allocated from `m_twFabricIx_Next`). It does **not** register nodes — the node handle table and the map-object backing store live on the [`CONTAINER`](../container/index.md).
- **Owns** the page background colour (`m_rgbaBackground` plus an atomic changed-flag `m_bBackdrop_Changed`) and the scene-global ambient / directional light (`m_Scene_Light_Ambient` / `m_Scene_Light_Directional`), and remembers the primary node (`m_pNode_Primary`).
- **Reaches** engine services through the owner chain — `SCENE` holds no cached engine or network pointer; it asks its context.

---

## Threading, locking, and pitfalls

This is the part to read before calling anything. The scene is touched from several threads — the engine control thread, network fetch threads delivering MSF/WASM/resource data, and the render thread — so its members carry real concurrency hazards.

**A single recursive mutex guards the fabric table.** `m_mxScene` is a `std::recursive_mutex`. It is held by `Fabric_Open`, `Fabric_Close`, and `Fabric_Find`. (The node handle table is guarded separately by the container's own `m_mxContainer`, not by `m_mxScene`.)

**Why recursive, not plain?** Because closing a fabric *cascades back into the same locked method on the same thread*. `Fabric_Close` deletes the fabric, whose destructor closes its root node through the container; closing a node deletes its children and closes any fabric attached to it (`Fabric_Close` again). A plain mutex would self-deadlock on the first re-entrant call. The recursion is deliberate and load-bearing — do not "simplify" it to a `std::mutex`.

**`Fabric_Find` returns an unguarded pointer.** It looks the fabric up under the lock and returns the `FABRIC*`, but nothing stops that fabric from being closed (and freed) after the lock is released and before you use the pointer. A capture/release reference scheme is planned for host calls; until then, do not retain the result across anything that could trigger a fabric close.

**Fetch callbacks run on fetch threads.** `OnMsfReady` / `OnMsfFailed` are invoked from the network layer's completion path, not the caller's thread, and they mutate the scene. They take the lock for the fabric-table mutations they perform.

**The backdrop is lock-free.** `Background` stores an `RGBA` and trips an atomic flag; `Background_Consume` test-and-clears that flag. There is no mutex — the compositor reads the colour only in the same call that observes the flag set, and writes are rare (a fresh load and any `primary.background` override).

**Teardown is not render-synchronized.** Tearing the tree down and rebuilding it is not coordinated with a render-thread traversal in progress, and it does not cancel an in-flight MSF fetch. See [Current limitations](../../systems/scene.md#current-limitations).

---

## Construction and destruction

```cpp
explicit SCENE (CONTEXT* pContext);
~SCENE ();
```

**`SCENE(pContext)`** Constructs an empty scene owned by `pContext`. Does not build any fabric — call `Initialize`. `pContext` must outlive the scene.

**`~SCENE()`** Destroys the implementation, which destroys the root fabric and triggers the full teardown cascade (all fabrics, nodes, and map objects). A leaked-fabric count is logged if anything remains registered after the cascade.

---

## Navigation

```cpp
bool Initialize (const std::string& sUrl);
bool Url        (const std::string& sUrl);
```

### `bool Initialize (const std::string& sUrl)`
- **Purpose.** Reset the backdrop to black, build the scene's root fabric, and begin loading the fabric at `sUrl`. Internally, through the root fabric's container, it creates the root node and a "primary" node whose payload carries `sUrl` in its resource reference and the sentinel subtype `255`; creating that node is what kicks off the asynchronous fabric load. The scene remembers the primary node.
- **Parameters.** `sUrl` — the address of the fabric to load. May be empty to build an empty root.
- **Returns.** `true` if the root fabric and its two nodes were created; `false` otherwise.
- **Notes.** Call once, after construction. The actual fabric content arrives later via the asynchronous loading flow.

### `bool Url (const std::string& sUrl)`
- **Purpose.** The declared navigation mutator: swap the root fabric to a new address, replacing all loaded content. This is the navigation seam an integrator conceptually uses — reached through the owning [`CONTEXT`](../context/index.md), not called directly.
- **Parameters.** `sUrl` — the new address.
- **Returns.** `true` if the new root fabric was created; `false` otherwise.
- **Pitfalls.** Swapping the root fabric runs the same teardown cascade as destruction, which is not synchronized with an in-progress render traversal and does not cancel an in-flight MSF fetch; tearing down while a fabric MSF is still loading is a known hazard. See [Current limitations](../../systems/scene.md#current-limitations).

---

## Accessors

```cpp
ENGINE*  Engine         () const;
CONTEXT* Context        () const;
NETWORK* Network        () const;
FABRIC*  Fabric_Root    () const;
FABRIC*  Fabric_Primary () const;
```

### `ENGINE* Engine () const`
- **Purpose / Returns.** The owning engine, resolved through the context. Never null for a live scene.

### `CONTEXT* Context () const`
- **Purpose / Returns.** The owning context.

### `NETWORK* Network () const`
- **Purpose / Returns.** The context's network subsystem, used for every fetch the scene and its nodes perform.

### `FABRIC* Fabric_Root () const`
- **Purpose / Returns.** The structural root fabric, or null before `Initialize`.

### `FABRIC* Fabric_Primary () const`
- **Purpose.** The primary loaded fabric — the fabric mounted on the scene's primary node (the one named by the navigation URL).
- **Returns.** The attached fabric (resolved as `m_pNode_Primary->Fabric_Attachment()`), or null if nothing has finished loading on the primary node yet.

---

## Presentation (backdrop)

```cpp
RGBA Background       () const;
void Background       (const RGBA& rgbaBackground);
bool Background_Consume (RGBA& rgbaBackground);
```

The scene owns the page-wide background colour and feeds it to the renderer through the compositor. For the design — how the primary fabric's `"primary"` block sets the initial camera pose and background — see [Scene system → Backdrop and primary presentation](../../systems/scene.md#backdrop-and-primary-presentation).

### `RGBA Background () const`
- **Purpose.** The current background colour (`RGBA`, components in `[0, 1]`).

### `void Background (const RGBA& rgbaBackground)`
- **Purpose.** Set the background colour and trip the changed-flag so the compositor pushes it on the next build. Called by `Initialize` (reset to black at the start of every load) and by `Primary_Apply` when a primary fabric declares a `primary.background`.
- **Parameters.** `rgbaBackground` — the colour, components in `[0, 1]`.

### `bool Background_Consume (RGBA& rgbaBackground)`
- **Purpose.** Test-and-clear the changed-flag; if it was set, copy the colour into `rgbaBackground`. The compositor calls this once per build and only pushes to `RENDERER::SetBackground` when it returns `true`, so the colour is sent on change (including scene swaps), never every frame.
- **Parameters.** `rgbaBackground` — filled with the current colour when the return is `true`.
- **Returns.** `true` if the backdrop changed since the last consume; `false` otherwise (in which case `rgbaBackground` is left untouched).
- **Notes.** Internal — driven by the compositor, not application code.

---

## Scene-global lighting

```cpp
SCENE_LIGHT Ambient      () const;
SCENE_LIGHT Directional  () const;
void        Ambient      (const SCENE_LIGHT& Light);
void        Directional  (const SCENE_LIGHT& Light);
```

Ambient and directional ("sun") light are properties of the **whole scene**, not nodes in the graph — so a local object can never alter global illumination. Both are authored by the primary fabric's `"primary"` block (`Primary_Apply`) and read each frame by the compositor, which forwards them to [`RENDERER::SetSceneLighting`](../viewport/RENDERER.md). A [`SCENE_LIGHT`](#scene_light) carries an `fIntensity`, an `rgbColor` (`RGB`), and — for the directional — a `vDirection` (`VEC3`, the unit vector the light travels along). The directional is authored like a spot node: the `"primary"` block gives a `rotation` quaternion that rotates the identity forward (+X) to produce `vDirection` (default +X). An `fIntensity` of `0` is simply an off light and is fully authorable; when the primary fabric authors neither ambient nor directional, the scene defaults the ambient to full-intensity white so the scene is not black.

### `SCENE_LIGHT Ambient () const` / `SCENE_LIGHT Directional () const`
- **Purpose.** The scene's current ambient / directional light (a copy).

### `void Ambient (const SCENE_LIGHT& Light)` / `void Directional (const SCENE_LIGHT& Light)`
- **Purpose.** Set the scene's ambient / directional light. Intended for the primary fabric during `Primary_Apply`.

#### `SCENE_LIGHT`

```cpp
struct SCENE_LIGHT
{
   float fIntensity = 0.0f;
   RGB   rgbColor   = { 1.0f, 1.0f, 1.0f };
   VEC3  vDirection = { 0.0, 0.0, -1.0 };   // directional only
};
```

---

## Fabric management (internal)

Engine- and host-facing. An application does not normally call these.

```cpp
void    Fabric_Spawn (NODE* pNode_Attach, const std::string& sUrl);
FABRIC* Fabric_Open  (NODE* pNode_Attach, MSF* pMsf, const std::string& sUrl);
FABRIC* Fabric_Close (FABRIC* pFabric);
FABRIC* Fabric_Find  (uint64_t twFabricIx) const;
```

### `void Fabric_Spawn (NODE* pNode_Attach, const std::string& sUrl)`
- **Purpose.** Begin loading the fabric named at `sUrl`, to be attached to `pNode_Attach` when it arrives. Starts an asynchronous MSF fetch via the network layer.
- **Parameters.** `pNode_Attach` — the node the loaded fabric will mount on; `sUrl` — the fabric address. No-op if `sUrl` is empty.
- **Returns.** Nothing. The fetch is fire-and-forget today; there is no handle to cancel it and no status returned (a known gap — see the system page).

### `FABRIC* Fabric_Open (NODE* pNode_Attach, MSF* pMsf, const std::string& sUrl)`
- **Purpose.** Open a [`CONTAINER`](../container/index.md) for `pMsf`, construct and register a new fabric bound to it, and initialize it (which begins WASM module fetches).
- **Parameters.** `pNode_Attach` — attachment node (null for the root fabric); `pMsf` — the parsed, verified MSF (null for the root fabric); `sUrl` — the fabric's URL.
- **Returns.** The new `FABRIC*`, or null on failure.
- **Ownership.** On success the scene owns the fabric (registered in the fabric table). The fabric takes responsibility for `pMsf`, which is deleted when the fabric is closed.
- **Pitfalls.** If `Initialize` fails the fabric is immediately closed and null returned.

### `FABRIC* Fabric_Close (FABRIC* pFabric)`
- **Purpose.** Destroy a fabric, unregister it, release its container reference, and delete its MSF.
- **Parameters.** `pFabric` — the fabric to close. Must be non-null.
- **Returns.** Always `nullptr`, so callers can write `p = Fabric_Close(p);` to close and clear in one line.
- **Pitfalls.** Deleting the fabric cascades into node teardown that re-enters the scene lock on the same thread — the recursive mutex is what makes this safe.

### `FABRIC* Fabric_Find (uint64_t twFabricIx) const`
- **Purpose.** Look up a registered fabric by its scene-global index.
- **Parameters.** `twFabricIx` — the fabric index.
- **Returns.** The `FABRIC*`, or null if no fabric has that index.
- **Pitfalls.** The returned pointer is **not** lifetime-guarded; see [Threading and pitfalls](#threading-locking-and-pitfalls).

---

> **The node handle table is on `CONTAINER`, not `SCENE`.** `Node_Root` / `Node_Open` / `Node_Close` / `Node_Find` (and the private `Node_Create`) are public methods on [`CONTAINER`](../container/index.md), backed by its `m_umpNode` / `m_apMap_Object` / `m_twObjectIx_Next` and guarded by `m_mxContainer`. Node identity is per-container, not scene-global. Scene code reaches these through `pFabric->Container()`. See the [Container API](../container/index.md).

---

## Internal callbacks

```cpp
void OnMsfReady  (NODE* pNode_Attach, FILE* pFile);
void OnMsfFailed (NODE* pNode_Attach, FILE* pFile);
```

Delegated from the file-local MSF fetch helper; invoked on a network fetch thread.

### `void OnMsfReady (NODE* pNode_Attach, FILE* pFile)`
- **Purpose.** The MSF for a spawned fabric has arrived. Reads the file, constructs and parses an `MSF`, verifies its signature and certificate chain, then opens the fabric on `pNode_Attach`. If the fabric mounted on the primary node, it applies the MSF's `"primary"` presentation block (initial camera pose and background colour). Logs success (and the source's trust level) or the relevant failure.
- **Parameters.** `pNode_Attach` — the node the fabric attaches to; `pFile` — the completed network file holding the MSF bytes.
- **Returns.** Nothing.

### `void OnMsfFailed (NODE* pNode_Attach, FILE* pFile)`
- **Purpose.** The MSF fetch failed. Logs the failure to the originating source's console stream.
- **Parameters.** As above; `pFile` carries the failed request's URL.
- **Returns.** Nothing.

---

## See also

- [Scene system](../../systems/scene.md) — design, loading flow, limitations.
- [FABRIC](FABRIC.md) and [NODE](NODE.md) — the other scene classes.
- [Container API](../container/index.md) — what `Fabric_Open` opens.
- [Network API](../network/index.md) — `FILE`/`IFILE` used by the fetch callbacks.

---

[Scene API](index.md) · Next: [FABRIC](FABRIC.md)
