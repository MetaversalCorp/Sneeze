---
title: Scene Object Reference
tier: Guides
audience: [author]
sources:
  - include/Map_Object.h
  - src/context/scene/Map_Object.cpp
  - src/context/scene/Node.cpp
  - src/sneeze/control/Compositor.cpp
verified: b3d15ea
nav:
  prev: guides/authoring-dynamic-scenes.md
  next: guides/authoring-wasm-host-api.md
---

# Scene Object Reference

This is the catalogue of everything the engine can draw. Both authoring paths -- the [static data tree](authoring-static-scenes.md) and the [dynamic WASM path](authoring-dynamic-scenes.md) -- ultimately produce the same thing: a tree of nodes, each carrying a **map object** that describes one thing in space. This page tells you, class by class, exactly what each kind of object turns into on screen, which of its fields the renderer reads, and the rules and surprises that come with it.

Read it as the "what will actually appear" companion to the two how-to pages. When one of them says "a physical node renders as a box" or "a celestial body is invisible by itself," this is the page that explains precisely why and how. Everything here is verified against the compositor -- the part of the engine that walks the node tree each frame and emits draw commands -- so it reflects what the code does today, not what is planned.

---

## The coordinate frame

Every position, rotation, and bound on this page is expressed in the engine's world frame, so it is worth stating that frame once, up front.

The world is **right-handed and Z-up**:

- **+X is right (east).**
- **+Y is forward (north).**
- **+Z is up.**

The ground is the XY plane and height is measured along +Z. The frame obeys the right-hand rule (X × Y = Z), the same convention CAD, GIS, and physical-simulation tools use: a floor plan lives in XY, and things get taller in +Z.

**Placement.** A node's `Transform.Position` is `[x, y, z]` in metres, its `Transform.Rotation` is a quaternion `[x, y, z, w]`, and its `Transform.Scale` is `[x, y, z]`. All three are relative to the parent (see [Transforms compose down the tree](#transforms-compose-down-the-tree)).

**Identity orientation.** At the identity rotation `[0, 0, 0, 1]` a directional object faces **+X** with its top toward **+Z**. That is where a camera or a spot/directional light starts: an unrotated camera looks level along +X (no roll, up is +Z), and an unrotated spot or directional light shines toward +X. You rotate from there to aim it. A point light is omnidirectional, so orientation does not apply to it.

**Imported models.** glTF and GLB files are authored Y-up, as their format specifies. The engine rotates each model into the world frame as it loads, so a loaded model's own "up" ends up along +Z with no action from you -- you place and aim it with its node `Transform` like anything else.

---

## The mental model: object, class, type

Every node references one **map object**. A map object has a fixed set of fields (identity, name, transform, bounds, appearance, and so on) and belongs to one **class**. The class is the top-level "what kind of thing is this," and it is baked into the object's composed id (the class letter in a `"P-1"` id, or the high bits of a composed integer in code). The class decides how the compositor treats the object:

| Class | Letter | Value | What it is |
|---|---|---|---|
| Root | `R` | 70 | The single top frame of a fabric. Draws nothing itself. |
| Celestial | `C` | 71 | Stars, planets, moons, orbital systems, and their surfaces. |
| Terrestrial | `T` | 72 | Ground/terrain-scale objects. Draws only if it carries a model. |
| Physical | `P` | 73 | Ordinary objects -- models, props, boxes. |
| Panel | (none) | 74 | An in-scene UI surface. Code-only; cannot be authored in JSON. |
| Light | `L` | 75 | A scene light. |

Within a class, the **type** (`Type.bType`) sub-classifies further -- which kind of celestial body, or which kind of light. Type values are class-specific: `bType` = 3 means a point light on a light node, but "galaxycluster" on a celestial node. Always read a type value in the context of its class.

One field applies to every class and overrides class behaviour, so it comes first.

---

## Models: any class can carry geometry

Before the per-class rules, the one universal rule: **if a node's map object has a loaded glTF/GLB model, the compositor draws that model, regardless of the node's class.** A model can sit on a celestial, terrestrial, or physical node -- the geometry renders the same way in all three cases, transformed by the node's world position.

You attach a model by putting a `.glb` URL in the object's `Resource.sReference`. When the fabric loads, the engine fetches that URL, sniffs the bytes, recognizes a glTF model (a binary GLB begins with the ASCII magic `glTF`; a glTF JSON document begins with `{`), decodes it, and hands the built model to the map object. From then on the compositor emits that model's meshes every frame.

Rules that come with models:

- **GLB (self-contained) only.** The decoder takes a single byte blob. A binary `.glb` with embedded geometry and textures works; a glTF JSON with embedded (data-URI) buffers works. A `.gltf` that references *separate* `.bin` or image files by relative path will **not** resolve those files -- ship `.glb`.
- **A model extends the scene's framing.** The engine auto-scales the whole scene so it fits the view (see [Automatic framing](#automatic-framing-render-scale) below). A model's bounding sphere counts toward that, so a large model pushes the auto-frame out like any other geometry.
- **The `action:` escape.** If `Resource.sReference` begins with the literal text `action:`, the node is treated as an **invisible logic volume** -- it is never fetched and never drawn, even if it is a physical node that would otherwise fall back to a box. Use this prefix for colliders and trigger regions you do not want to see.

Everything below is what happens when a node does *not* have a model (or, for celestial and light nodes, in addition to any model).

---

## Root

A root node (`R`) is the single top of a fabric's tree. It carries no geometry and draws nothing -- it exists purely as the frame every other node hangs under. Its transform still matters: move or rotate the root and the entire fabric moves or rotates with it, because every descendant composes its own transform onto the root's.

In the static path the payload's `data.scene` object *is* the root. In the dynamic path you create it with `Node_Root`. You will usually give the root an identity and a name and nothing else.

---

## Physical

A physical node (`P`) is the workhorse -- ordinary objects placed in space. It renders one of two ways, depending on whether it has a model:

- **With a GLB model:** it renders that model (per the universal model rule above).
- **With no model:** it falls back to a **grounded box** sized by its `Bound.d3Max` -- width, depth, height in metres (`[x, y, z]`). The box sits with its base on the node's `z = 0` plane and rises along +Z by its height (it is grounded, not centred). The box is given an **automatic colour** derived from the node's index, so adjacent boxes are visually distinct without you setting anything.

The single most important rule: **a physical box needs a non-zero `Bound.d3Max`.** A model-less node with a zero bound produces a zero-size box, which is invisible. If a node with a model *fails* to load its model, it also falls back to the box -- which is why it is worth giving even model nodes a sensible `Bound` as a fallback.

Physical nodes do **not** sample textures. If you set an image URL on a physical node's `Resource.sReference`, the engine fetches it but nothing draws it -- a physical object's appearance comes from its GLB, or from the automatic box colour. (Texturing is a celestial-surface feature; see below.)

---

## Terrestrial

A terrestrial node (`T`) is intended for ground- and terrain-scale objects. Today it has **no dedicated rendering** of its own: the compositor has no terrestrial branch. A terrestrial node is therefore visible only through the universal model rule -- if it carries a GLB model, that model draws; if it does not, the terrestrial node draws nothing (there is no box fallback for terrestrial, only for physical).

In short: use a terrestrial node when you want a model placed at the terrestrial level of the hierarchy. If you just want a visible placeholder, use a physical node, which has the box fallback.

---

## Celestial

Celestial nodes (`C`) are the richest and least obvious class -- they build stars, planets, moons, and the orbits they travel. The key idea, and the thing that surprises everyone first, is that **a celestial body is not one node.** A single planet is authored as three nested nodes, each with a different job:

```text
System   (the orbit -- draws the orbit trail, and moves along it over time)
  Body   (the physical body -- carries radius and colour; a star also emits light)
    Surface  (the visible sphere -- carries the texture and draws the textured, spinning ball)
```

This split lets the engine keep three concerns separate: *where a thing orbits* (system), *how big it is* (body), and *what it looks like* (surface). It maps directly onto the `Submit_System` / `Submit_Body` / `Submit_Surface` helper trio in the solar-system example module.

The celestial `Type.bType` selects which role a node plays:

| `bType` | Name | Role |
|---|---|---|
| 9 | starsystem | Orbital frame for a star system. |
| 10 | star | A star body. Carries radius + colour to its surface **and emits a point light**. |
| 11 | planetsystem | Orbital frame for a planet. |
| 12 | planet | A planet body. Carries radius + colour to its surface. |
| 125 | moonsystem | Orbital frame for a moon. |
| 13 | moon | A moon body. |
| 135 | debrissystem | Orbital frame for a comet/asteroid. |
| 14 | debris | A comet/asteroid body. |
| 17 | surface | The visible, textured, spinning sphere. |

(Other values -- universe, galaxy, nebula, blackhole, satellite, transport, and so on -- are defined in the type enum but have no rendering behaviour today.)

Here is what each of the three roles actually does in the compositor.

### System nodes (the orbit)

A node whose type is one of the four `*system` values (`starsystem`, `planetsystem`, `moonsystem`, `debrissystem`) is an **orbital frame**. Two things happen:

1. **It moves along its orbit over time.** If the node has orbit parameters, its position each frame is computed from an ellipse rather than taken literally from its transform. The orbit is defined by `Orbit.dA` (semi-major axis, metres), `Orbit.dB` (semi-minor axis, metres), `Orbit.tmPeriod` (orbital period), and `Orbit.tmOrigin` (a phase offset so bodies do not all start at the same point). The node's `Transform.d4Rotation` orients the orbital plane. Because the body and surface are children of the system, they ride along as the system orbits.
2. **It draws an orbit trail.** A tapering, tube-like curve traces the path the body has recently travelled, coloured a dimmed version of the system's `Properties.fColor`.

The trail (and the orbital motion) only appear when the node genuinely has an orbit. The engine's test for that is specific: `Orbit.dA` must be non-zero, `Orbit.tmPeriod` must be non-zero, and the transform's rotation `w` component must be non-zero. A system node missing any of those sits still and draws no trail.

### Body nodes (radius and colour)

A node whose type is `star`, `planet`, `moon`, or `debris` is the **body**. It does not draw a sphere itself. Instead it carries two pieces of appearance down to its surface child:

- **Radius**, from `Bound.d3Max[0]` -- the body's true radius in metres.
- **Colour**, from `Properties.fColor` -- a `0xRRGGBB` value (see [Colour](#colour) below).

A **star** body does one extra thing: it **emits a point light** at its position, so the bodies around it are lit. This is why a solar system is lit without you adding a light node -- the star is the light.

A body normally sits at its system's origin (its own transform is near-identity) and lets the system's orbit do the moving. A body can also have its own orbit parameters, in which case it orbits within its parent frame.

### Surface nodes (the visible sphere)

A node whose type is `surface` is the **only celestial node that draws a visible sphere.** It renders a ball at the body's world position, using:

- the **radius** and **colour** handed down from the parent body,
- an optional **texture** from its own `Resource.sReference` -- an image URL. This is the one place a texture is actually sampled: the image is wrapped onto the sphere. (For the solar example, these are planet maps like `earth.jpg`.)
- a **spin**, from `Orbit.tmPeriod` (the rotation period) and `Orbit.dA` (the initial spin angle in radians) -- the sphere rotates about its vertical axis over time.

If the parent body is a **star**, its surface is drawn **emissive** (it glows) rather than lit, so the star reads as a light source instead of a lit ball.

The consequence to remember: **a celestial body with no surface child is invisible.** The body and system carry data; the surface is what you see. If you author a planet and see nothing, you almost certainly forgot the surface child.

> Celestial orbital data is intricate -- the transform, orbit, and precession fields interact through real orbital mechanics. In practice celestial scenes are generated by an exporter (or copied from the solar-system example) rather than hand-authored. The [dynamic scenes](authoring-dynamic-scenes.md) page points at the example source, which is the clearest working reference.

---

## Light

A light node (`L`) adds illumination. It draws no geometry; it contributes a light to the renderer at the node's world position. Its `Type.bType` selects the kind:

| `bType` | Kind | Behaviour |
|---|---|---|
| 1 | ambient | A uniform fill light with no position and no falloff. |
| 2 | directional | A parallel light; the node's position vector is read as a direction. |
| 3 | point | Emits from the node's world position with distance falloff. |
| 4 | spot | A cone of light from the node's position, aimed along its local +X (rotated by its `Transform`); cone from `fOpeningAngle` / `fFalloffAngle` (degrees). |
| 0 | (none) | Treated as a point light (the default fallback). |

The light reads two appearance fields:

- **Colour**, from `Properties.fColor` -- a `0xRRGGBB` value.
- **Intensity**, from `Properties.fBrightness`.

A point light authored at unit scale is intensity-compensated by the engine so that it lights a scene the same way no matter what scale the scene ends up framed at, or whether the fabric is embedded inside another. The practical upshot for you: **author a point light at a sensible position and brightness at unit scale, and it will look right wherever the fabric is dropped.** Ambient and directional lights have no falloff, so their intensity passes through unchanged.

A light does **not** count toward the scene's automatic framing (below) -- a light placed far out to the side illuminates the scene without shrinking everything else to fit it in view.

---

## Panel

A panel (class 74) is an in-scene **UI surface** -- an RmlUi RML+CSS document (HTML-like markup with CSS styling) rasterized to a texture and drawn as a flat quad in the world. It is the way you put readable text, cards, and simple interfaces into a 3D space.

Panels have two hard constraints:

- **Code only.** There is no JSON node kind for a panel. A panel exists only when a WASM module creates it with the `Node_Panel` host call, handing the engine the RML+CSS source. You cannot author one in a static `data` tree. (See [WASM host API for content](authoring-wasm-host-api.md) for the call and a full example.)
- **`Bound` carries aspect only.** A panel's `Bound.d3Max[0]` and `[1]` are read as the quad's width and height *ratio*, not an absolute size. The panel's on-screen size is derived from the framed scene, and its position comes from the node's transform.

A panel is **billboarded** -- its face turns to follow the camera, so it stays readable from any orbit angle instead of being seen edge-on. Panels are treated as chrome, not scene geometry: like lights, they do not affect the automatic framing.

---

## Cross-cutting details

A few fields and behaviours span all classes.

### Transforms compose down the tree

Every node's `Transform` (position in metres, rotation quaternion `[x,y,z,w]`, scale `[x,y,z]`) is **relative to its parent**, and transforms multiply down the hierarchy. Move a parent and its descendants move with it. This is standard scene-graph behaviour and it is uniform across every class -- root, celestial, terrestrial, physical, light, and panel all inherit their parent's frame identically. Celestial system nodes are the one place where the position each frame may be *computed* (from the orbit) rather than taken literally, but even then it composes onto the parent frame the same way.

### Colour

Colour lives in `Properties.fColor`, and its representation is a genuine gotcha: the field is a 32-bit float whose **bit pattern** is read as a `0xRRGGBB` colour, not a float value. From code this is natural -- you write `f32::from_bits(0x00RRGGBB)` (or use the `Node_Color` host call, which takes the colour as an integer directly). From a JSON `data` tree it is awkward, because you would have to write the decimal value of that reinterpreted float. For static scenes, lean on GLB model colours and the automatic box colouring instead of setting `fColor` by hand; colour authoring is really a job for the code path.

Where colour is read: celestial body/surface (the sphere colour), celestial system (the orbit-trail colour, dimmed), and light nodes (the light colour). A physical box ignores `fColor` entirely -- it is auto-coloured from its index.

### Bound means different things per class

`Bound.d3Max` is overloaded by class:

- **Physical (model-less):** the box size in metres -- `[width, depth, height]` (`[x, y, z]`, with height along +Z).
- **Celestial body:** `[0]` is the body's true radius in metres; the other two are unused.
- **Panel:** `[0]` and `[1]` are the quad's aspect ratio; absolute size is derived.
- **Model of any class:** unused for the model itself (the model has its own bounds), but still the box-fallback size if the model fails to load.

### Automatic framing (render scale)

You do not set a global scale. Every frame, the engine measures how far the scene reaches from its root (the farthest renderable point, plus body radii and model bounds) and computes one uniform **render scale** that maps that reach to a fixed on-screen extent, so the default camera frames the whole thing. Everything -- celestial and physical alike -- rides that single scale.

The practical consequences: author in **real metres** and trust the engine to fit it in view; a scene spanning a plaza and a scene spanning a solar system both frame correctly. Lights and panels are deliberately excluded from the measurement so they never distort the framing. This is an interim mechanism (a single per-scene scale); it is why very large and very small things in the *same* scene can look size-compressed relative to each other, but it guarantees you always see something sensible without tuning a camera.

### Attaching another fabric (subtype 255)

Any node can stand in for a whole other fabric. Set its `Type.bSubtype` to **255** and put a `.msf` URL in `Resource.sReference`; when the engine reaches that node it spawns the referenced fabric at that node's place in the tree, inheriting its transform. The attachment node's own children are not used -- the child fabric supplies its contents. This is how large spaces are composed from independently authored pieces, and it is covered in depth on [Static scenes: the data tree](authoring-static-scenes.md).

---

## Quick "what appears" table

| You author... | You see... |
|---|---|
| Root node | Nothing (it is a frame). |
| Physical node + GLB URL | The model. |
| Physical node, no model, non-zero `Bound` | A grounded, auto-coloured box. |
| Physical node, no model, zero `Bound` | Nothing (invisible). |
| Terrestrial node + GLB URL | The model. |
| Terrestrial node, no model | Nothing (no box fallback). |
| Celestial body alone | Nothing (the body is invisible by itself). |
| Celestial body + surface child | A textured, spinning sphere; a star also lights the scene. |
| Celestial system with orbit params | A moving orbit trail; children orbit with it. |
| Light node | Illumination (point / ambient / directional); no geometry. |
| Panel (via `Node_Panel`) | A camera-facing UI quad. |
| Any node, `Resource` starts with `action:` | Nothing (invisible logic volume). |
| Node with `Type.bSubtype` = 255 + `.msf` URL | The referenced child fabric, spawned in place. |

---

## See also

- [Static scenes: the data tree](authoring-static-scenes.md) -- authoring these objects declaratively in JSON.
- [Dynamic scenes with WASM](authoring-dynamic-scenes.md) -- authoring them from code, with the celestial three-level pattern in the solar-system example.
- [WASM host API for content](authoring-wasm-host-api.md) -- the calls that create and modify these objects, including `Node_Panel`.
- [Scene system](../systems/scene.md) and [Viewport system](../systems/viewport.md) -- the engine internals behind the scene model and the compositor.

---

[Home](../Home.md) · Prev: [Dynamic scenes with WASM](authoring-dynamic-scenes.md) · Next: [WASM host API for content](authoring-wasm-host-api.md)
