---
title: Dynamic Scenes with WASM
tier: Guides
audience: [author]
sources:
  - src/deps/wasm/Wasm_Instance.cpp
  - src/deps/wasm/Wasm_Store.cpp
  - src/deps/wasm/HostFunctions.cpp
  - include/Map_Object.h
  - tests/wasm/map/src/lib.rs
  - tests/wasm/solar_system/src/lib.rs
  - tests/wasm/solar_system/src/planets.rs
verified: ca4689d
nav:
  prev: guides/authoring-static-scenes.md
  next: guides/authoring-scene-reference.md
---

# Dynamic Scenes with WASM

This guide is for the second authoring path: building a scene with **code** instead of a static JSON tree. Where a [map-managed fabric](authoring-static-scenes.md) hands the engine a fixed `data` block and lets a generic module inject it, a **WASM-managed** fabric ships its own WebAssembly module that runs inside the engine's sandbox and constructs the scene one node at a time. This is the path you take when a scene needs logic to come into being -- when it is generated, parameterized, computed, or simply too large and too regular to hand-write.

We will go slowly and show everything. By the end you will understand the module lifecycle, the exact way data crosses the boundary between your code and the engine, the one wire structure every node travels as, and you will have a complete, buildable module that puts real geometry on screen. The companion page [WASM host API for content](authoring-wasm-host-api.md) is the exhaustive function-by-function reference; this page is the narrative that teaches you how the pieces fit.

If you have not read [Authoring spatial fabrics](authoring-fabrics.md) and the [static scenes](authoring-static-scenes.md) page, read them first. This page assumes you know what a fabric, a payload, and a node are.

---

## Why a code path exists at all

The map-managed path is declarative: you list objects and where they go, and that is the whole scene forever. That is perfect for a plaza or a museum -- fixed content, placed once. But some spaces cannot be written as a fixed list:

- A **solar system** has hundreds of bodies whose positions are a function of time and orbital mechanics. You do not want to hand-place a thousand asteroids; you want a loop.
- A space assembled from **data** -- a chart, a query result, a procedurally generated layout -- is not known until the code runs.
- A space that **varies** by who is looking, what is stored, or any other input needs a decision, and a JSON tree cannot make decisions.

For all of these, the fabric carries a program. The engine runs that program in a **sandbox** -- a WebAssembly virtual machine that can touch nothing on the user's machine except the specific functions the engine hands it. Your code cannot open files, reach the network, or see other fabrics. It can only call the host functions the engine registered. That is the safety bargain that lets a browser run a stranger's code at all.

In practice the sandbox module is written in **Rust** and compiled to the `wasm32-unknown-unknown` target. Rust is not required by the engine -- any language that compiles to a freestanding `.wasm` with C-ABI exports works -- but every example in this repository is Rust, the toolchain is a one-line install, and the rest of this page uses it.

---

## The shape of a WASM-managed fabric

Structurally, a WASM-managed fabric is almost identical to a map-managed one. It is the same payload with two differences:

- **`modules`** lists *your* module instead of the generic `map.wasm`.
- There is **no `data` block** -- your code builds the scene, so there is nothing to inject.

```json
{
   "container": "my-dynamic-space",
   "services": [],
   "modules":
   [
      { "url": "https://YOUR-HOST/my_module.wasm", "hash": "" }
   ],
   "primary":
   {
      "camera": { "position": [-8, 0, 2], "rotation": [0, 0, 0, 1] },
      "background": "202830"
   }
}
```

Everything you learned about the payload on [The MSF file and signing](authoring-msf-and-signing.md) still applies: `container` is the identity, `hash` is the optional integrity check on the module, `primary` still aims the camera and paints the sky on the top-level fabric. The only thing that changed is what the one module *does*: instead of reading a `data` tree, it makes scene-building calls itself.

You can list more than one module. Each is fetched, compiled, and run. In practice a fabric ships one module that builds its scene.

---

## The module lifecycle

When the engine loads your fabric, it fetches the module, verifies the `hash` if you gave one, compiles it, and creates a live **instance** inside the fabric's sandbox. It then calls a small set of **exported functions** -- functions your module marks for the engine to find by name. Here are the five the engine looks for, exactly as it looks them up:

| Export | When the engine calls it | Signature |
|---|---|---|
| `Init` | Once, before the first `Open`, when the instance first comes alive. | `fn Init()` |
| `Open` | When the fabric opens. **This is where you build your scene.** | `fn Open(twFabricIx: u64, dwOffset: u32, dwLength: u32)` |
| `Close` | When the fabric closes. | `fn Close(twFabricIx: u64)` |
| `Shutdown` | Once, after the last `Close`, when the instance is torn down. | `fn Shutdown()` |
| `OnTimer` | Intended for timer callbacks. **Never called today** (see the limitation below). | -- |

Every export is optional -- the engine checks whether each exists and skips the ones that do not. But `Open` is the one that matters: it is where your scene is created.

The order is a strict mirror, the way everything in this engine is a strict mirror: `Init` fires once as the instance wakes, `Open` fires each time the fabric is opened, `Close` fires each time it is closed, and `Shutdown` fires once as the instance is put to sleep. The engine tracks this with a reference count internally; you do not manage it. You just implement the functions.

### The one limitation that shapes everything: no per-frame tick

**Your code runs once, during `Open`, and then it is done.** There is no per-frame callback, no update loop, no `OnTimer` that ever fires -- the timer host functions and the `OnTimer` export exist as stubs but the engine never invokes them. Whatever you want in the scene, you must create during `Open`.

This is less limiting than it sounds, because the engine animates the scene *for* you. A celestial body with orbit parameters moves along its orbit every frame without your code running again -- the motion is computed by the engine from the parameters you set once. What you cannot do today is run your own logic every frame (a game loop, a physics step, a reaction to input). Design your scene as a thing you *describe once* and let the engine animate, not as a thing you *drive continuously*.

---

## How data crosses the boundary

Your module lives in its own memory -- a single flat array of bytes the WebAssembly spec calls **linear memory**. The engine lives outside it. When the two need to exchange anything bigger than a number, they do it by agreement about a region of *your* memory: you tell the engine "there are N bytes starting at offset P in my memory," and the engine reads (or writes) that region directly.

That is why almost every host function takes a pair of integers -- an **offset** (`dwOffset` / a pointer cast to `u32`) and a **length** (`dwLength`). A string is passed as the offset of its bytes and its byte count. A structure is passed the same way: the offset of the struct and its size. The engine reaches into your linear memory at that offset and copies the bytes out.

You never allocate anything on the engine's side. You hand the engine an offset into your own memory, it copies what it needs synchronously during the call, and when the call returns the engine no longer refers to your memory. This keeps the sandbox boundary clean: the engine never holds a pointer into your address space past the moment of a call.

For strings, the encoding is **UTF-8**. For a node, the "structure" is a fixed 528-byte record called `RMCOBJECT`, described next.

---

## RMCOBJECT: the wire form of a node

Every node you create travels to the engine as one fixed-layout, 528-byte structure named `RMCOBJECT`. It is the flattened, C-compatible form of a scene object: identity, name, type, transform, orbit, bounds, and appearance, all in a known byte order. You fill one in, then hand the engine its offset and length.

Here is the exact layout, copied from the working solar-system module. Paste it into your project verbatim -- the field order, the `#[repr(C, packed)]`, and the 528-byte size assertion all matter, because the engine reinterprets the bytes at these exact offsets:

```rust
#[repr(C, packed)]
struct RMCOBJECT
{
   // OBJECT_HEAD (24 bytes) -- identity
   qwObjectIx_Parent:       u64,   // composed id of the parent node
   qwObjectIx_Self:         u64,   // composed id of this node
   qwEvent:                 u64,   // user tag (leave 0)

   // MAP_OBJECT_NAME (96 bytes) -- 48 UTF-16 code units
   wsName:                  [u16; 48],

   // MAP_OBJECT_TYPE (8 bytes)
   bType:                   u8,     // sub-kind within the class (light kind, celestial body kind)
   bSubtype:                u8,     // 255 marks a child-fabric attachment
   bFiction:                u8,     // author tag
   abReserved_Type:         [u8; 5],

   // MAP_OBJECT_OWNER (8 bytes)
   twOwner:                 u64,

   // MAP_OBJECT_RESOURCE (200 bytes) -- external asset binding
   qwResource:              u64,
   sName_Resource:          [u8; 64],
   sReference:              [u8; 128],  // asset URL (GLB model, or celestial texture)

   // MAP_OBJECT_TRANSFORM (80 bytes) -- placement relative to parent
   d3Position:              [f64; 3],   // metres [x, y, z]
   d4Rotation:              [f64; 4],   // quaternion [x, y, z, w]
   d3Scale:                 [f64; 3],   // [x, y, z]

   // MAP_OBJECT_ORBIT (32 bytes) -- orbital / spin parameters (celestial)
   tmPeriod:                i64,
   tmOrigin:                i64,
   dA:                      f64,
   dB:                      f64,

   // MAP_OBJECT_BOUND (48 bytes)
   abReserved_Bound:        [u8; 24],
   d3Max:                   [f64; 3],   // extents (physical box size; celestial radius in [0])

   // MAP_OBJECT_PROPERTIES (32 bytes)
   fMass:                   f32,
   fGravity:                f32,
   fColor:                  f32,        // 0xRRGGBB reinterpreted as f32 bits
   fBrightness:             f32,        // light intensity
   fReflectivity:           f32,
   abReserved_Properties:   [u8; 12],
}

const _: () = assert!(core::mem::size_of::<RMCOBJECT>() == 528);
```

Two fields carry the whole identity system, so they deserve a close look.

### Composed object indices

A node's identity -- both its own (`qwObjectIx_Self`) and its parent's (`qwObjectIx_Parent`) -- is a single 64-bit number that packs two things: a **class** in the top 16 bits and a **48-bit index** in the low bits. You compose it with one helper (also copied from the working modules):

```rust
const fn OBJECTIX_COMPOSE(wClass: u16, twObjectIx: u64) -> u64
{
   ((wClass as u64) << 48)  |  (twObjectIx & 0x0000_FFFF_FFFF_FFFF)
}
```

The class values are fixed integers:

| Class | Value | Letter (JSON id) |
|---|---|---|
| Root | 70 | `R` |
| Celestial | 71 | `C` |
| Terrestrial | 72 | `T` |
| Physical | 73 | `P` |
| Panel | 74 | (code only) |
| Light | 75 | `L` |

So `OBJECTIX_COMPOSE(73, 1)` is "physical object 1" -- the same node the JSON id `"P-1"` names. The two forms are interchangeable; code composes the integer, JSON writes the letter.

**Parentage is by index, not by pointer.** When you create a child, you set its `qwObjectIx_Parent` to the *composed id of a node you already created*, and you pass that same parent id as the first argument to the create call. The engine looks up the parent by that index within the fabric. This has one hard consequence: **create parents before children.** A child that names a parent that does not exist yet is dropped.

### The `wsName` and `sReference` limits

`wsName` is 48 UTF-16 code units -- keep names short and ASCII-safe. `sReference` is a 128-byte buffer and holds at most **127 usable characters**; a longer URL is truncated and the fetch breaks. Keep asset URLs short (this is the same limit called out for the static path).

---

## Your first dynamic module, end to end

Let us build the smallest module that puts real geometry on screen: a root, one automatically coloured box, and one GLB model. Every line is annotated.

### Step 1 -- create the Rust project

```bash
rustup target add wasm32-unknown-unknown
cargo new --lib my_module
cd my_module
```

### Step 2 -- configure it to build a `.wasm`

Edit `Cargo.toml` so the crate compiles to a standalone dynamic library (a `.wasm` with no runtime). The `cdylib` crate type and a small release profile are all you need:

```toml
[package]
name = "my_module"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[profile.release]
opt-level = "s"
lto = true
```

### Step 3 -- declare the imports and the wire struct

In `src/lib.rs`, first declare the host functions you intend to call. Each `extern "C"` block names the **import module** (`"Console"`, `"Scene"`, and so on) that the engine registered its functions under. You only declare the ones you use:

```rust
#![allow(non_snake_case, non_camel_case_types, dead_code)]

#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log(dwOffset: u32, dwLength: u32);
}

#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Root(twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Open(twParentIx: u64, dwOffset: u32, dwLength: u32) -> u64;
}
```

Then paste the `RMCOBJECT` struct and the `OBJECTIX_COMPOSE` helper from above, plus the class constants:

```rust
const MAP_OBJECT_CLASS_ROOT:     u16 = 70;
const MAP_OBJECT_CLASS_PHYSICAL: u16 = 73;
```

A tiny logging helper makes the browser's developer console your friend while you iterate:

```rust
fn LogMsg(sMsg: &str)
{
   unsafe { Log(sMsg.as_ptr() as u32, sMsg.len() as u32); }
}
```

`sMsg.as_ptr() as u32` is the offset of the string's bytes in your linear memory; `sMsg.len()` is its byte count. That offset-plus-length pair is the whole boundary convention in one line.

### Step 4 -- build the scene in `Open`

`Open` receives the fabric's index as `twFabricIx`. You create the root by passing that index to `Node_Root`, then hang children off the root by passing the root's composed id to `Node_Open`.

```rust
#[no_mangle]
pub extern "C" fn Open(twFabricIx: u64, _dwOffset: u32, _dwLength: u32)
{
   LogMsg("my_module: Open");

   // --- The root node ---------------------------------------------------
   let mut root = RMCOBJECT::New();
   root.qwObjectIx_Self = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_ROOT, 0);
   root.d3Scale         = [1.0, 1.0, 1.0];       // never leave scale at zero
   root.d4Rotation      = [0.0, 0.0, 0.0, 1.0];  // identity orientation
   root.Name_Set("Root");

   let dwOffset = &root as *const RMCOBJECT as u32;
   let dwLength = core::mem::size_of::<RMCOBJECT>() as u32;
   let twRoot = unsafe { Node_Root(twFabricIx, dwOffset, dwLength) };

   // --- A model-less box (auto-coloured, sized by its bound) ------------
   let mut box_node = RMCOBJECT::New();
   box_node.qwObjectIx_Parent = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_ROOT, 0);      // child of the root
   box_node.qwObjectIx_Self   = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_PHYSICAL, 1);
   box_node.d3Position        = [0.0, 3.0, 0.0];   // three metres to the left (+Y, facing +X)
   box_node.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   box_node.d3Scale           = [1.0, 1.0, 1.0];
   box_node.d3Max             = [2.0, 2.0, 2.0];   // 2 m box; a zero bound is invisible
   box_node.Name_Set("Box");
   Submit(&box_node);

   // --- A GLB model -----------------------------------------------------
   let mut model = RMCOBJECT::New();
   model.qwObjectIx_Parent = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_ROOT, 0);
   model.qwObjectIx_Self   = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_PHYSICAL, 2);
   model.d3Position        = [0.0, -3.0, 0.0];      // three metres to the right (-Y, facing +X)
   model.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   model.d3Scale           = [1.0, 1.0, 1.0];
   model.d3Max             = [1.0, 1.0, 1.0];       // fallback box if the model fails to load
   model.Reference_Set("https://YOUR-HOST/model.glb");
   model.Name_Set("Model");
   Submit(&model);

   LogMsg("my_module: scene built");
}

fn Submit(obj: &RMCOBJECT)
{
   let dwOffset = obj as *const RMCOBJECT as u32;
   let dwLength = core::mem::size_of::<RMCOBJECT>() as u32;
   unsafe { Node_Open(obj.qwObjectIx_Parent, dwOffset, dwLength); }
}
```

The `New`, `Name_Set`, and `Reference_Set` helpers are the same small `impl RMCOBJECT` block used in the example modules -- `New` zeroes the struct, `Name_Set` copies up to 48 characters into `wsName`, and `Reference_Set` copies up to 127 bytes into `sReference`. Copy that block from `tests/wasm/solar_system/src/lib.rs`.

Two details worth internalizing from this example:

- **Set scale and rotation explicitly.** A freshly zeroed `RMCOBJECT` has scale `[0,0,0]` and rotation `[0,0,0,0]`, both degenerate -- a zero-scale node collapses and a zero quaternion is not a valid orientation. Always set scale to `[1,1,1]` and rotation to `[0,0,0,1]` unless you mean otherwise. (The map-managed JSON path fills these defaults for you; the code path does not.)
- **The box and the model are siblings under the root**, each with a unique `Self` index and both naming the root as parent. That is the entire parenting mechanism.

### Step 5 -- add the lifecycle stubs

Round out the module with the other exports. They can be empty; they exist so the engine has something to call:

```rust
#[no_mangle] pub extern "C" fn Init()               { LogMsg("my_module: Init"); }
#[no_mangle] pub extern "C" fn Close(_fabric: u64)  { LogMsg("my_module: Close"); }
#[no_mangle] pub extern "C" fn Shutdown()           { LogMsg("my_module: Shutdown"); }
```

### Step 6 -- build, host, and load

```bash
cargo build --release --target wasm32-unknown-unknown
```

The output is `target/wasm32-unknown-unknown/release/my_module.wasm`. Host it, point your payload's `modules[0].url` at it, host the payload, and open it in the browser -- exactly the host-and-load flow from [Your first fabric](authoring-first-fabric.md). You should see an auto-coloured box on the left and your model on the right. The developer console will show your `Open: scene built` log line, which is the fastest confirmation your module ran.

---

## Adjusting nodes after you create them

Besides `Node_Open`, the `Scene` module exposes a handful of **mutators** -- calls that change a property on a node you already created, addressed by its composed id. They are convenient when you build in a loop and want to tweak values without re-filling a whole `RMCOBJECT`:

```rust
#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Position(twObjectIx: u64, dX: f64, dY: f64, dZ: f64);
   fn Node_Color(twObjectIx: u64, nColor: i32);   // 0xRRGGBB
   fn Node_Name(twObjectIx: u64, dwOffset: u32, dwLength: u32);
   fn Node_Close(twObjectIx: u64) -> i32;
}
```

For instance, once you have the box's id you can recolour it directly:

```rust
let twBox = OBJECTIX_COMPOSE(MAP_OBJECT_CLASS_PHYSICAL, 1);
unsafe { Node_Color(twBox, 0x00FF8800u32 as i32); }   // orange
```

The complete list of mutators, their exact effects, and their argument types is on [WASM host API for content](authoring-wasm-host-api.md). A couple have surprising behaviour (for example, `Node_Bound` and `Node_Radius` are the same call under the hood, and `Node_Scale` sets only one axis), so check the reference before relying on one.

---

## A larger example: the solar system

The repository's `tests/wasm/solar_system` module is the canonical dynamic fabric and the best thing to read once the basics click. It builds a whole solar system -- a star, planets, moons, comets -- entirely in code, and it shows two patterns you will reuse:

**Building in a loop from data.** Its `planets.rs` is one call per body, each a line of orbital constants, submitted through a shared `Submit_System` / `Submit_Body` / `Submit_Surface` helper trio. That is the "scene from data" pattern: the data is a table, the code is a loop, the result is hundreds of nodes.

**The three-level celestial pattern.** A celestial body is not one node but three, nested:

1. A **system** node carries the orbit (its `Orbit.dA`/`dB` are the ellipse semi-axes, `tmPeriod` the period) and draws the orbit trail.
2. A **body** node under it carries the radius and colour.
3. A **surface** node under *that* carries the texture and draws the visible, spinning sphere.

This split is not arbitrary -- it is how the engine separates "where a thing orbits" from "how big it is" from "what it looks like," and it is explained in full on [Scene object reference](authoring-scene-reference.md). If you are building a space scene, read that page's celestial section alongside the solar-system source; celestial orbital data is intricate enough that it is almost always generated by an exporter rather than hand-written.

One more capability has no JSON equivalent: an in-scene **UI panel** built with `Node_Panel`. It hands the engine an RML+CSS document (RmlUi's HTML-like markup) that the engine rasterizes to a textured, camera-facing quad. That is the only way to put a UI surface in a scene, and it is covered on the host-API page.

---

## What you can and cannot do today (honest limitations)

These are current, code-level facts. Design around them:

- **No per-frame code.** Your module runs during `Open` and stops. There is no tick, no update, no input callback. `OnTimer` and the `Timer` host functions are stubs that never fire. Build the scene once; let the engine animate orbits for you.
- **Sandbox only.** Your module can call the registered host functions (console, storage, scene, timer) and nothing else. No file system, no network, no access to other fabrics.
- **Storage works but is unused elsewhere.** The `Storage` host functions are fully wired to persistent per-fabric JSON storage, but nothing else in the running application uses storage yet, so treat it as functional-but-unexercised.
- **GLB, not external glTF.** Model URLs must resolve to self-contained `.glb` files (or glTF JSON with embedded buffers). A `.gltf` that points at separate `.bin` or image files will not resolve them.
- **Textures apply only to celestial surfaces.** Setting a texture URL on a physical node fetches the image but nothing samples it; physical models get their look from the GLB and model-less boxes are auto-coloured. Only a celestial `surface` node textures its sphere.
- **One build of the module, reused.** Like `map.wasm`, a compiled module is content-addressed by URL and hash; the same instance is shared across fabrics that reference the same URL and hash.

---

## See also

- [WASM host API for content](authoring-wasm-host-api.md) -- the complete, function-by-function reference for every call this page used.
- [Scene object reference](authoring-scene-reference.md) -- what each class of node actually draws, including the celestial three-level pattern.
- [Static scenes: the data tree](authoring-static-scenes.md) -- the declarative path, for scenes that do not need code.
- [WASM system](../systems/wasm.md) -- the engine internals of the sandbox, stores, and instances.

---

[Home](../Home.md) · Prev: [Static scenes: the data tree](authoring-static-scenes.md) · Next: [Scene object reference](authoring-scene-reference.md)
