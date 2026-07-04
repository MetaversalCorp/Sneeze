---
title: WASM Host API for Content
tier: Guides
audience: [author]
sources:
  - src/deps/wasm/Wasm_Store.cpp
  - src/deps/wasm/HostFunctions.cpp
  - src/deps/wasm/Wasm_Instance.cpp
  - include/Storage.h
  - include/Map_Object.h
verified: b3d15ea
nav:
  prev: guides/authoring-scene-reference.md
---

# WASM Host API for Content

This is the complete reference for every function a content module can call and every function the engine calls on a content module. If [Dynamic scenes with WASM](authoring-dynamic-scenes.md) taught you the shape of a module, this page is the dictionary: every import, its exact signature, its arguments, its return value, and what it actually does -- verified against the code that registers and implements it.

A module talks to the engine across a hard sandbox boundary. It can call **only** the functions listed here, grouped into four import modules -- `Console`, `Storage`, `Scene`, and `Timer` -- and it exposes a small set of **exports** the engine calls in return. Nothing else crosses the line: no files, no network, no other fabrics.

Read the [dynamic scenes](authoring-dynamic-scenes.md) guide first for the narrative and a buildable example. This page is what you keep open beside your editor.

---

## The calling conventions (read once)

Three conventions apply to every function below. Learn them once and the tables read themselves.

**Numbers are `i32`, `i64`, or `f64`.** WebAssembly has only numeric parameters, so everything is one of these. Object indices are `i64`, coordinates and sizes are `f64`, and pointers, lengths, colours, and scopes are `i32`.

**Anything bigger than a number is passed as an offset + length into your linear memory.** A string is `(ptr, len)` -- the byte offset of its UTF-8 bytes in your module's memory and the byte count. A node structure is `(ptr, len)` -- the offset of a 528-byte `RMCOBJECT` and its size. The engine reads (or writes) that region of your memory synchronously during the call and holds no reference to it afterward. Strings are **UTF-8**.

**The read-back (query-size) protocol.** Functions that hand data *back* to you (`Storage.Get`, `Storage.GetJson`) write into a buffer you supply as `(outPtr, outLen)` and **return the full byte size the value needs** -- not the number of bytes written. So:

- If the return value is `<= outLen`, you got the whole thing.
- If the return value is `> outLen`, the value was truncated; reallocate a buffer of the returned size and call again.
- Passing `outLen == 0` writes nothing and just returns the size, so you can allocate exactly once and then fetch.

Two sentinel values show up in `Scene` return codes: an object index of **0** (`OBJECTIX_NULL`) and **`0x0000FFFFFFFFFFFE`** (`OBJECTIX_ERROR`) both mean "failed." A successful create returns a real composed object index.

---

## Exports: what the engine calls on your module

Your module marks functions for the engine to find by name with `#[no_mangle] pub extern "C"`. The engine looks up exactly these five; every one is optional (the engine skips those you do not define):

| Export | Signature | When it runs |
|---|---|---|
| `Init` | `fn Init()` | Once, before the first `Open`, as the instance comes alive. |
| `Open` | `fn Open(twFabricIx: u64, dwOffset: u32, dwLength: u32)` | Each time the fabric opens. **Build your scene here.** |
| `Close` | `fn Close(twFabricIx: u64)` | Each time the fabric closes. |
| `Shutdown` | `fn Shutdown()` | Once, after the last `Close`, as the instance is torn down. |
| `OnTimer` | -- | Intended for timer callbacks. **The engine never calls it today.** |

`Open` receives the fabric's index (which you pass straight to `Node_Root` and use to compose parent ids) plus a `(dwOffset, dwLength)` parameter pair reserved for open-time parameters; the current modules ignore it. The lifecycle is a strict mirror: `Init` and `Shutdown` bracket the instance's life; `Open` and `Close` bracket each use of the fabric. There is **no per-frame export** -- your code runs at `Open` and stops.

---

## `Console` -- developer console output

Fourteen functions that mirror the browser developer console. Each forwards to the fabric's console stream, so anything you log shows up in the engine's developer console -- your primary debugging tool while authoring. None returns a value. Every argument is a `(ptr, len)` UTF-8 string unless noted.

Declared in Rust as:

```rust
#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log            (dwOffset: u32, dwLength: u32);
   fn Debug          (dwOffset: u32, dwLength: u32);
   fn Info           (dwOffset: u32, dwLength: u32);
   fn Warn           (dwOffset: u32, dwLength: u32);
   fn Error          (dwOffset: u32, dwLength: u32);
   fn Assert         (nCondition: i32, dwOffset: u32, dwLength: u32);
   fn Group          (dwOffset: u32, dwLength: u32);
   fn GroupCollapsed (dwOffset: u32, dwLength: u32);
   fn GroupEnd       ();
   fn Count          (dwOffset: u32, dwLength: u32);
   fn CountReset     (dwOffset: u32, dwLength: u32);
   fn Time           (dwOffset: u32, dwLength: u32);
   fn TimeEnd        (dwOffset: u32, dwLength: u32);
   fn TimeLog        (dwOffset: u32, dwLength: u32);
}
```

| Function | Arguments | Effect |
|---|---|---|
| `Log` | (ptr, len) | Log a message at normal level. |
| `Debug` | (ptr, len) | Log at debug level. |
| `Info` | (ptr, len) | Log at info level. |
| `Warn` | (ptr, len) | Log a warning. |
| `Error` | (ptr, len) | Log an error. |
| `Assert` | (cond, ptr, len) | If `cond` is 0, log the message as an assertion failure; otherwise do nothing. |
| `Group` | (ptr, len) | Open a named, indented log group. |
| `GroupCollapsed` | (ptr, len) | Open a group that starts collapsed. |
| `GroupEnd` | () | Close the most recent group. |
| `Count` | (ptr, len) | Increment and log a named counter. |
| `CountReset` | (ptr, len) | Reset a named counter. |
| `Time` | (ptr, len) | Start a named timer. |
| `TimeEnd` | (ptr, len) | Stop a named timer and log its elapsed time. |
| `TimeLog` | (ptr, len) | Log a running timer's elapsed time without stopping it. |

The one-line logging helper every example module uses:

```rust
fn LogMsg(sMsg: &str)
{
   unsafe { Log(sMsg.as_ptr() as u32, sMsg.len() as u32); }
}
```

---

## `Storage` -- persistent per-fabric JSON

Six functions backing a persistent, per-fabric JSON document store -- the spatial equivalent of a web page's `localStorage`/`sessionStorage`. Every call takes a **scope** selector as its first argument, choosing one of four independent document stores:

| Scope value | Constant | Meaning |
|---|---|---|
| 0 | `PERMANENT_ORG` | Persists; shared across the organization's fabrics. |
| 1 | `PERMANENT_COMPANY` | Persists; scoped to the publisher (company). |
| 2 | `TEMPORARY_ORG` | Session-lived; organization-shared. |
| 3 | `TEMPORARY_COMPANY` | Session-lived; company-scoped. |

Paths are dot-notation with array brackets, e.g. `game.table[5].color`. Values are JSON text in both directions -- a value can be a scalar, object, or array.

```rust
#[link(wasm_import_module = "Storage")]
extern "C"
{
   fn Get     (nScope: i32, dwPathPtr: u32, dwPathLen: u32, dwOutPtr: u32, dwOutLen: u32) -> i32;
   fn Set     (nScope: i32, dwPathPtr: u32, dwPathLen: u32, dwValPtr: u32, dwValLen: u32) -> i32;
   fn Remove  (nScope: i32, dwPathPtr: u32, dwPathLen: u32) -> i32;
   fn Has     (nScope: i32, dwPathPtr: u32, dwPathLen: u32) -> i32;
   fn GetJson (nScope: i32, dwOutPtr: u32,  dwOutLen: u32)  -> i32;
   fn SetJson (nScope: i32, dwJsonPtr: u32, dwJsonLen: u32) -> i32;
}
```

| Function | Returns | Effect |
|---|---|---|
| `Get` | full byte size of the value | Read the JSON value at a path into your out-buffer. Follows the query-size protocol above. |
| `Set` | 1 on success, 0 on failure | Write a JSON value (the `val` bytes, parsed as JSON) at a path. Fails if the value is not valid JSON. |
| `Remove` | 1 | Delete the value at a path. |
| `Has` | 1 if present, 0 if not | Test whether a path exists. |
| `GetJson` | full byte size of the document | Read the entire scope document as one JSON string. Query-size protocol applies. |
| `SetJson` | 1 | Replace the entire scope document from a JSON string. |

Storage is fully wired and durable (every mutation is journaled to disk), but note that nothing else in the running application uses storage yet -- it works, but it is not yet exercised by the rest of the browser. Treat it as functional and stable, lightly travelled.

---

## `Scene` -- building and modifying the scene

The heart of a content module: twelve functions that create and modify nodes. Creation calls take a 528-byte `RMCOBJECT` by `(ptr, len)`; mutators address an existing node by its `i64` composed index. See [Scene object reference](authoring-scene-reference.md) for what each resulting node actually draws.

```rust
#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Map      (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Root     (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Open     (twParentIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Close    (twObjectIx: u64) -> i32;
   fn Node_Position (twObjectIx: u64, dX: f64, dY: f64, dZ: f64);
   fn Node_Scale    (twObjectIx: u64, dScale: f64);
   fn Node_Bound    (twObjectIx: u64, dBound: f64);
   fn Node_Color    (twObjectIx: u64, nColor: i32);
   fn Node_Name     (twObjectIx: u64, dwOffset: u32, dwLength: u32);
   fn Node_Radius   (twObjectIx: u64, dRadius: f64);
   fn Node_Texture  (twObjectIx: u64, dwOffset: u32, dwLength: u32);
   fn Node_Panel    (twParentIx: u64, dwObjPtr: u32, dwObjLen: u32, dwSrcPtr: u32, dwSrcLen: u32) -> u64;
}
```

### Creation

| Function | Returns | Effect |
|---|---|---|
| `Node_Map` | root object index, or error | **The map-managed shortcut.** Reads a node tree out of the fabric's MSF `data` block and builds it host-side. `(ptr, len)` is a UTF-8, dot-separated path locating the tree inside `data` (e.g. `"scene"`, or `"a.b.c"`); an empty path uses the whole `data` block. This is the single call the generic `map.wasm` makes -- it passes its hardcoded `"scene"`. Mutually exclusive with building by hand -- use this *or* `Node_Root`+`Node_Open`, not both. |
| `Node_Root` | new object index, or error | Create the fabric's root node from the `RMCOBJECT` at `(ptr, len)`. The first argument is the **fabric** index passed to `Open`. |
| `Node_Open` | new object index, or error | Create a child node under `twParentIx` from the `RMCOBJECT` at `(ptr, len)`. The parent must already exist. The `RMCOBJECT` needs at least 528 bytes or the call fails. |
| `Node_Close` | 1 on success, 0 on failure | Remove and delete the node with the given index (and its subtree). |

`Node_Root` and `Node_Open` are the two workhorses; the example modules wrap them in `Submit_*` helpers. A returned index of `0` or `0x0000FFFFFFFFFFFE` means the create failed (bad parent, undersized buffer, or unreadable memory).

### Mutators

Each mutator finds the node by index and changes one thing on its map object. They take effect on the next frame. They return nothing.

| Function | Effect | Note |
|---|---|---|
| `Node_Position` | Set the node's local position to `(dX, dY, dZ)` metres. | |
| `Node_Scale` | Set the node's **X-axis** scale to `dScale`. | Sets only `Transform.d3Scale[0]`; Y and Z are left unchanged. |
| `Node_Bound` | Set all three bound extents (`d3Max[0..2]`) to `dBound`. | A uniform bound. |
| `Node_Color` | Set `Properties.fColor` from the 32-bit colour `nColor` (`0xRRGGBB`). | Pass the colour as an integer, e.g. `0x00FF8800u32 as i32`. |
| `Node_Name` | Set the node's display name from the UTF-8 string at `(ptr, len)`. | Up to 47 characters. |
| `Node_Radius` | Set all three bound extents to `dRadius`. | **Identical to `Node_Bound`** -- both write the same three fields. Named for celestial radius; use whichever reads clearer. |
| `Node_Texture` | Copy the UTF-8 string at `(ptr, len)` into the node's `Resource.sReference`. | Sets the reference string **only**; it does not start a fetch (fetches happen at node creation from the `RMCOBJECT` you submitted). Of limited use post-creation today. |

Two honest quirks to keep in mind: `Node_Scale` changes only the X axis (despite the name suggesting uniform scale), and `Node_Bound`/`Node_Radius` are the same operation under two names. Set scale and bounds in the `RMCOBJECT` before you submit the node when you want all three axes controlled.

### `Node_Panel` -- create an in-scene UI surface

`Node_Panel` is the only way to put a UI panel in a scene (there is no JSON equivalent). It creates a child node under `twParentIx` from an `RMCOBJECT` -- forcing the node's class to panel regardless of how you composed its id -- and then sets the panel's document from an RML+CSS source string at `(srcPtr, srcLen)`. It returns the new object index (or error).

The `RMCOBJECT` you pass sets the panel's placement (`Transform`) and its aspect ratio (`Bound.d3Max[0]`, `[1]` = width, height ratio); the source string is an RmlUi RML+CSS document the engine rasterizes to the panel's texture. Here is the working pattern, condensed:

```rust
#[link(wasm_import_module = "Scene")]
extern "C" { fn Node_Panel(p: u64, o: u32, ol: u32, s: u32, sl: u32) -> u64; }

const PANEL_RML: &str =
   "<rml><head><style>\
    body { width: 100%; height: 100%; font-family: Inter; color: #e9eef6; }\
    #card { position: absolute; left: 6%; top: 6%; width: 88%; height: 88%;\
            padding: 28px; border-radius: 18px;\
            background-color: rgba(18, 22, 32, 224); }\
    .title { display: block; font-size: 26px; color: #ffd089; }\
    </style></head>\
    <body><div id='card'><span class='title'>Hello Panel</span></div></body></rml>";

// obj is an RMCOBJECT with class PANEL, a position, and Bound.d3Max = [aspectW, aspectH, 0]
let dwObjPtr = &obj as *const RMCOBJECT as u32;
let dwObjLen = core::mem::size_of::<RMCOBJECT>() as u32;
let twPanel  = unsafe {
   Node_Panel(obj.qwObjectIx_Parent, dwObjPtr, dwObjLen, PANEL_RML.as_ptr() as u32, PANEL_RML.len() as u32)
};
```

Notes that keep panels working:

- Use `font-family: Inter` (the font the engine's UI context loads) unless you have arranged for another.
- The panel is rasterized at 512x512 and drawn as a camera-facing (billboarded) quad; its on-screen size derives from the framed scene, and `Bound` supplies only the aspect ratio.
- If you never set a source, the panel shows a built-in default document.

---

## `Timer` -- stubs, no effect today

Two functions exist and are registered, but both are **stubs**: `Timer.Set` returns 0 and does nothing, `Timer.Clear` does nothing, and the `OnTimer` export is never called. There is no working scheduling mechanism -- do not build against timers. They are declared here only so you recognize them if you see them in older example code.

```rust
#[link(wasm_import_module = "Timer")]
extern "C"
{
   fn Set   (nDelayMs: i32, nId: i32) -> i32;   // stub: returns 0
   fn Clear  (nId: i32);                         // stub: no-op
}
```

Because there is no tick and no timer, everything your module does must happen during `Open`. The engine animates the scene (orbits, spins, panel billboarding) on its own; your code does not run again.

---

## See also

- [Dynamic scenes with WASM](authoring-dynamic-scenes.md) -- the narrative guide and a full buildable module using these calls.
- [Scene object reference](authoring-scene-reference.md) -- what the nodes these calls create actually draw.
- [WASM system](../systems/wasm.md) -- the engine internals of the sandbox, the linker, and host-function registration.
- [Console system](../systems/console.md) and [Storage system](../systems/storage.md) -- where `Console` and `Storage` calls end up.

---

[Home](../Home.md) · Prev: [Scene object reference](authoring-scene-reference.md) · Up: [Guides](index.md)
