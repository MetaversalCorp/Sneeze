---
title: Your First Fabric
tier: Guides
audience: [author]
sources:
  - src/context/scene/Scene.cpp
  - src/deps/wasm/HostFunctions.cpp
  - tests/wasm/map/src/lib.rs
  - tools/ConvertDfw/convert_dfw.py
  - tools/SignMsf/main.cpp
verified: b3d15ea
nav:
  prev: guides/authoring-fabrics.md
  next: guides/authoring-msf-and-signing.md
---

# Your First Fabric

This is the fastest path from an empty text editor to a shape floating in a 3D browser. We will build a **map-managed** fabric — a scene described entirely in JSON, with no code to write — get it on screen as plain JSON, add a real 3D model, set a camera, and finally sign it into a proper `.msf` file. Every step is shown in full and annotated. Nothing here is a fragment; you can copy each block verbatim.

If you have not read [Authoring spatial fabrics](authoring-fabrics.md), read it first — it defines the words used below (payload, map-managed, MSF, and so on).

---

## What you need

- **A running host application** — a metaverse browser built on Sneeze that you can point at a URL. Building and embedding the engine is covered in [Building Sneeze](building.md) and [Embedding Sneeze](embedding-sneeze.md); this guide assumes you have a working browser.
- **A place to serve static files over HTTPS** — any static web host or CDN. The example fabrics in this repository are served from `https://cdn.rp1.com/`. For local work, any static file server your browser can reach will do.
- **The `SignMsf` tool** — built alongside the engine. You will find it in the install tree, e.g. `builds\windows-x64\install\release\bin\SignMsf.exe`. Only needed for the final signing step.
- **A Rust toolchain** — *only once*, to build the generic `map.wasm` module you will reuse for every map-managed fabric. If someone has already hosted a `map.wasm` you can use, you can skip that build entirely.

---

## Step 1 — Write the payload

A fabric's payload is a JSON object. Create a file named `first.json` with the minimum that produces something visible: an identity, a code module that will inject the scene, and a one-node scene.

```json
{
   "container": "first-space",
   "services": [],
   "modules":
   [
      {
         "url": "https://YOUR-HOST/map.wasm",
         "hash": ""
      }
   ],
   "data":
   {
      "scene":
      {
         "Head": { "Self": "P-1" },
         "Name": "Hello Box",
         "Bound": { "Max": [2.0, 2.0, 2.0] }
      }
   }
}
```

Line by line, this is what each part does:

- **`container`** is the fabric's identity name. It scopes the fabric's storage and sandbox. Pick anything descriptive.
- **`services`** is required to be present but does nothing yet; leave it as an empty array.
- **`modules`** lists the code the fabric runs. Here it is a single entry pointing at `map.wasm` — the generic module whose only job is to read your scene from `data.scene` and build it. **That scene is inert without this module**; the map module is what tells the engine to inject it.
  - **`url`** is where the engine fetches the module. You will host `map.wasm` yourself in Step 2 and replace `YOUR-HOST`.
  - **`hash`** is an optional integrity check in the form `sha256-<hex>`. An empty string means "do not check." We start with no check and add one at the end. (Note: the field is `hash`. Some example files use `comment-hash`, which the engine ignores.)
- **`data`** is a general-purpose block your fabric ships for its modules to read. `map.wasm` looks in one place inside it — `data.scene` — for the scene tree. Here `data.scene` is a single node: a **physical** object (`"P-1"` — the `P` prefix means physical), named `Hello Box`, with a bounding box two metres on each side. A physical node with **no** model URL renders as a plain, automatically coloured box sized by its bounds. That is our first visible result, and it needs no external asset at all.

The `Head.Self` value `"P-1"` is a human-readable node id: a class letter followed by a number. The letters are `R` root, `C` celestial, `T` terrestrial, `P` physical, `L` light. Every node in a `data.scene` tree needs a unique id.

---

## Step 2 — Build and host the map module

The map-managed path relies on one small, generic, reusable module: `map.wasm`. You build it **once** and reuse it for every map-managed fabric you ever make — it contains no scene data of its own. Its entire source is a few dozen lines (`tests/wasm/map/src/lib.rs`): on `Open`, it makes a single call, `Node_Map`, which asks the engine to read the current fabric's `data.scene` tree and build the whole thing. (The `"scene"` location is baked into `map.wasm`; the rest of `data` is left for other uses.)

Build it from the repository:

```bash
rustup target add wasm32-unknown-unknown
cd tests/wasm/map
cargo build --release --target wasm32-unknown-unknown
```

The output is `target/wasm32-unknown-unknown/release/map.wasm`. Upload that file to your static host, then set the `url` in `first.json` to wherever you put it (for example `https://YOUR-HOST/map.wasm`).

You can leave `hash` empty for now. When you want the integrity check, compute the hash of the exact file you uploaded and paste it in:

```powershell
# Windows PowerShell — prints an uppercase hex digest
Get-FileHash -Algorithm SHA256 map.wasm
```

```bash
# macOS / Linux
shasum -a 256 map.wasm
```

Take the hex digest, lowercase it, and prefix it with `sha256-`, giving a value like `"hash": "sha256-c3fadcd3914bd2ecf386ef0661e0b4385e4d4d80e7c7f96ca49eef56b1fb36d0"`. If the hosted file's bytes ever change, the hash must change too, or the load will fail the integrity check.

---

## Step 3 — Host it and load it as plain JSON

You do not have to sign a fabric to see it. The engine loads a plain-JSON payload directly — it just carries an untrusted, synthetic identity, which is fine for development.

Upload `first.json` next to `map.wasm` on your host. Then point your browser at the JSON file's address, for example `https://YOUR-HOST/first.json`.

You should see a single grey box floating in space. If you do:

1. The engine fetched your JSON.
2. It fetched and ran `map.wasm`.
3. The map module called `Node_Map`, and the engine built your one-node `data.scene` tree.
4. The compositor drew the model-less physical node as a box.

If nothing appears, the usual causes are: the module `url` is wrong or unreachable; the `data` node has no `Bound` (a zero-sized box is invisible); or the JSON has a syntax error (a trailing comma, a missing brace). Check the browser's developer console — the map module logs `Injected N nodes from MSF data block` on success.

---

## Step 4 — Replace the box with a real 3D model

A grey box proves the pipeline; now let us render actual geometry. Physical nodes render a **GLB** model when you give them one. Host a self-contained `.glb` file (embedded geometry and textures — a plain `.gltf` with separate `.bin`/image files will not resolve) and point a node at it with `Resource.sReference`.

```json
{
   "container": "first-space",
   "services": [],
   "modules":
   [
      { "url": "https://YOUR-HOST/map.wasm", "hash": "" }
   ],
   "data":
   {
      "scene":
      {
         "Head": { "Self": "R-0" },
         "Name": "Root",
         "Children":
         [
            {
               "Head": { "Self": "P-1" },
               "Name": "My Model",
               "Resource": { "sReference": "https://YOUR-HOST/model.glb" },
               "Transform": { "Position": [0.0, 0.0, 0.0] }
            }
         ]
      }
   }
}
```

What changed and why:

- The scene now has a **root** node (`"R-0"`) with the model as a child. Once you have more than one object it is cleanest to give the tree a single root and hang everything under `Children`. Parentage comes purely from nesting — the engine derives it from the tree, so you never set a `Parent` field.
- The model node points at a GLB with `Resource.sReference`. When the model loads, the node renders the mesh. If the model fails to load, the node falls back to a box sized by its `Bound` (which is why it is still worth giving models a sensible `Bound`).
- `Transform.Position` places the node relative to its parent, in metres. Omitting `Transform` entirely means "at the parent's origin, unrotated, unit scale" — the decoder defaults rotation to identity and scale to `[1,1,1]`.

Reload the JSON in your browser and your model appears in place of the box.

---

## Step 5 — Aim the camera and set the sky

By default you may be looking at your object from an arbitrary angle. The top-level fabric can specify where the camera starts and what colour the background is, using the `primary` block:

```json
{
   "container": "first-space",
   "services": [],
   "modules": [ { "url": "https://YOUR-HOST/map.wasm", "hash": "" } ],
   "primary":
   {
      "camera":
      {
         "Position": [-8.0, 0.0, 2.0],
         "Rotation": [0.0, 0.0, 0.0, 1.0]
      },
      "background": "202830"
   },
   "data":
   {
      "scene":
      {
         "Head": { "Self": "R-0" },
         "Name": "Root",
         "Children":
         [
            {
               "Head": { "Self": "P-1" },
               "Name": "My Model",
               "Resource": { "sReference": "https://YOUR-HOST/model.glb" }
            }
         ]
      }
   }
}
```

- **`camera.position`** is the eye position in metres, `[x, y, z]`. Here it sits eight metres back along -X and two metres up (+Z).
- **`camera.rotation`** is the orientation as a quaternion `[x, y, z, w]`. `[0, 0, 0, 1]` is the identity orientation: looking level along +X, up toward +Z. The camera therefore faces the model at the origin.
- **`background`** is a six-hex-digit `RRGGBB` colour for the sky/backdrop — `202830` is a dark blue-grey.

The `primary` block is only read on the **top-level** fabric — the one the browser is pointed at. A fabric embedded as a child inside another fabric does not get to move the camera or repaint the sky.

---

## Step 6 — Sign it into an `.msf`

Plain JSON is perfect for iterating. When you are ready to publish, wrap the payload in a signature so the engine can tell who authored it. The `SignMsf` tool does this. For local testing there are ready-made test certificates in `tests/certs/`.

```powershell
SignMsf --payload first.json ^
        --key  tests\certs\provider-key.pem ^
        --cert tests\certs\provider-cert.pem ^
        --chain tests\certs\ca-cert.pem ^
        --out first.msf
```

- **`--payload`** is your JSON file — its contents become the signed payload verbatim.
- **`--key`** is the private key that signs it.
- **`--cert`** is the signer's (leaf) certificate; it is embedded in the file so verifiers know who signed it.
- **`--chain`** adds an intermediate or root certificate to the embedded chain. You can pass `--chain` more than once. For the bundled test certs, `ca-cert.pem` is the authority that issued the provider cert.
- **`--out`** is the resulting `.msf`. By default the signature algorithm is `RS256`; pass `--alg` to change it.

Confirm the file before you ship it. The same tool verifies and dumps a fabric:

```powershell
SignMsf --verify first.msf --trust tests\certs\ca-cert.pem
```

This prints the algorithm, the certificate chain, whether the signature verifies against the trust anchor you passed, and the decoded payload. A healthy result ends with `Signature:   VERIFIED`.

Now upload `first.msf` alongside `map.wasm` and your model, and point your browser at the `.msf` instead of the `.json`. You should see exactly the same scene — now delivered as a signed fabric.

> One honest caveat: today the engine verifies the signature but still marks every fabric's trust level as *expired*, and it loads untrusted and plain-JSON fabrics anyway. Signing stamps a verifiable identity onto your fabric, but it does not yet unlock or gate any behaviour. Sign your published fabrics regardless — the identity is real and the enforcement is coming.

---

## What you built, and what to learn next

You now have a complete, signed, hostable fabric and you have exercised the entire pipeline: author, host the module, load as plain JSON, add a model, aim the camera, sign, verify, and load the signed file. Everything else in this section deepens one of those steps.

- To understand the file format, the signature, and the trust model in full — [The MSF file and signing](authoring-msf-and-signing.md).
- To learn every field of the `data.scene` node tree and how to compose large static scenes — [Static scenes: the data tree](authoring-static-scenes.md).
- To generate scenes with code instead of static JSON — [Dynamic scenes with WASM](authoring-dynamic-scenes.md).
- To know exactly what each object kind draws — [Scene object reference](authoring-scene-reference.md).

---

[Home](../Home.md) · Prev: [Authoring spatial fabrics](authoring-fabrics.md) · Next: [The MSF file and signing](authoring-msf-and-signing.md)
