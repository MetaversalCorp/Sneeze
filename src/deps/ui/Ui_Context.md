# UI — RmlUi (context, panels, software rasterizer)

The `ui` module wraps **RmlUi** 6.2 (a retained-mode HTML/CSS toolkit) with
**FreeType** for fonts. It owns the engine's one-time RmlUi global lifecycle and
turns RML+CSS documents into CPU pixel buffers the viewport can draw as in-scene
quads. All classes live in namespace `SNEEZE::DEP`.

Three classes:

- **UI_CONTEXT** — the per-engine manager (global lifecycle + shared services).
- **UI_PANEL** — one in-scene UI surface (one per `MAP_OBJECT_PANEL`).
- **UI_RENDER** — the software render interface (rasterizes RmlUi geometry to RGBA).

## UI_CONTEXT

One per `ENGINE`, created last among the dependency wrappers and reachable via
`ENGINE::Ui_Context()`. It brackets RmlUi's process-global state:

- `Initialize(ENGINE*)` installs the global **system interface** (`UI_SYSTEM`:
  elapsed time from a steady clock + logging routed into `ENGINE::Log`), sets the
  single shared **render interface** (`UI_RENDER`), and calls `Rml::Initialise()`.
  The destructor mirrors it: `Rml::Shutdown()`, then deletes the render interface
  last (it must outlive shutdown, which releases geometry/textures back through it).
- `Render()` — the one shared `UI_RENDER`. Every panel binds its context to this
  single, engine-lifetime interface (see "One render interface" below).

`Rml::Initialise` brings up RmlUi's FreeType-backed font engine automatically.

**Fonts are host-owned.** The engine bundles and loads no fonts. The host
application loads its faces (e.g. Inter) into RmlUi's process-global font
registry, which this engine instance shares, so panels render with the host's
fonts without the engine touching the filesystem.

## UI_PANEL

One self-contained UI surface: its own `Rml::Context` and `ElementDocument`, plus
a straight-alpha output buffer it owns. A scene may hold many; the owning
`MAP_OBJECT_PANEL` holds one.

- `Source(rml)` sets the RML+CSS document (a built-in default panel is used until
  then) and marks the canvas dirty.
- `Render(ENGINE*, w, h)` rasterizes the document into the canvas. It resolves
  the engine's `UI_CONTEXT` and shared `UI_RENDER`, creates the context lazily
  **bound to the shared `UI_RENDER`** (`Rml::CreateContext(name, dims, pUi_Render)`), then on the dirty
  path resizes the shared canvas to this panel, `Update()`s, clears, `Render()`s,
  and `Straighten()`s the result into its own buffer. Cheap when unchanged. Must
  run on the render (compositor) thread.
- `Pixels()/Width()/Height()/Serial()` expose the cached **straight-alpha** RGBA8 (row-major,
  top-down) and a generation counter that `Straighten()` bumps each rewrite. The destructor removes only its context — never the shared interface.

`Straighten()` converts `UI_RENDER`'s premultiplied-alpha canvas to straight alpha,
because the renderer's unlit **blend** material expects straight alpha. The panel
owns this UI-format knowledge so the renderer stays UI-agnostic.

Live camera textures: `LoadTexture("camera://N")` opens `ENGINE::Capture()`
device N. Each `Render` calls `UI_RENDER::LiveTexture_Update()` so a new camera
frame dirties the panel and is sampled on the next raster. Camera frames are
scaled into the texture size reported at load (RmlUi keeps that size).
`LiveTexture_Waiting()` is true until the first sample; `UI_PANEL::Render`
withholds that raster from the compositor so the first GPU copy is a real
frame, not the opaque placeholder. See `Capture.md`.

## UI_RENDER

A CPU implementation of `Rml::RenderInterface`. It rasterizes RmlUi's 2D triangle
geometry into an RGBA8 canvas (**premultiplied alpha**, row-major, top-down),
with a top-left fill rule (no double-drawn shared edges), texture sampling
(text/atlas), and scissor clipping. Optional layer/filter/shader hooks keep their
no-op defaults, so soft-glow/blur effects are deferred to a future GPU path.
`DrawCount()/DrawTextured()` are per-pass diagnostics reset on `Clear()`.

## One render interface (and the host split)

RmlUi keeps a **process-global render-manager registry keyed by render
interface**. The host application shares that registry: it creates its own
`Rml::Context` + its own render interface for chrome, and its teardown calls
the global `Rml::ReleaseRenderManagers()`. If the engine handed each panel its own
render interface, tearing a panel down would leave RmlUi holding a render manager
for a freed interface, and the host's `ReleaseRenderManagers()` would call into
freed memory and crash. So **every engine panel shares the one `UI_RENDER` owned
by `UI_CONTEXT`**, alive for the whole engine; panels render one at a time on the
compositor thread and reuse its canvas as scratch, copying each result out.

Ownership contract with the host:

- **Engine (Sneeze):** owns `Rml::Initialise`/`Shutdown`, the global system
  interface, and the one engine-side render interface. It loads no fonts.
- **Host application:** loads its font faces into RmlUi's process-global registry
  and creates its own context(s) and render interface for chrome; must tear those
  down before the engine shuts RmlUi down.

## How a panel reaches the screen

`MAP_OBJECT_PANEL` (scene) owns a `UI_PANEL`. The compositor rasterizes it during
traversal and flattens it to a `PANEL_DATA` (transform + pixel buffer); the ANARI
renderer draws it as an unlit, alpha-blended textured quad. See `Scene.md`
(`MAP_OBJECT_PANEL`), `Control.md` (compositor panel build), and `Viewport.md`
(renderer panel path).

## Files

| File | Contents |
|------|----------|
| `Ui_Context.h/.cpp` | UI_CONTEXT — RmlUi global lifecycle, system interface, fonts, shared render interface |
| `Ui_Panel.h/.cpp`   | UI_PANEL — per-panel context/document, rasterize to straight-alpha buffer |
| `Ui_Render.h/.cpp`  | UI_RENDER — software `Rml::RenderInterface` (premultiplied-alpha RGBA canvas, `camera://N` live textures) |
