---
title: "Example 02 - A Bucket on the Stool"
tier: Examples
audience: [author]
sources:
  - examples/02-stool-and-bucket/stool-and-bucket.json
  - examples/02-stool-and-bucket/README.md
verified: b3d15ea
nav:
  prev: examples/01-stool.md
  next: examples/03-signing.md
---

# Example 02 - A Bucket on the Stool

This example builds directly on [Example 01](01-stool.md). We keep the same stool, set a bucket on its seat, and light the whole thing ourselves: a scene-wide ambient fill and a directional "sun" declared in the fabric's `primary` block, plus two spot lights aimed at the objects. By the end you will understand how one object becomes the parent of others, how to place a child exactly where you want it, and the two different ways a fabric provides light -- scene-global lighting and placed light nodes -- instead of relying on the engine's fallback. The complete source lives in the repository at [`examples/02-stool-and-bucket/`](../../examples/02-stool-and-bucket/) -- copy it and follow along.

## ⚠️ The following warning paragraph is different from the one in Example 01
## ⚠️ This is not the preferred way to encode a scene

Loading a fabric with a hard-coded list of nodes stored in a JSON structure as shown below works today and is convenient while you are learning, but it is not how map nodes are meant to be transferred. Many fabrics will be excessively large: RP1's Earth fabric contains roughly half a billion nodes. Trying to store all of that in a single JSON file is unmanageable, and transferring all of it at once would take hours to days. Normally, a metaverse browser interrogates a map service for the nodes within a reasonable proximity of the viewer, based on object size and distance. A future example introduces the map service and shows how to use one instead of hard-coding your nodes inside a fabric file.

## What this example teaches

- How a scene becomes a *tree*: one node can have children, and a child moves with its parent.
- How to place a node precisely with a `Transform` -- a position, and optionally a rotation and scale -- measured relative to its parent.
- The two kinds of lighting a fabric provides: **scene-global** light (ambient and a directional "sun") set once in the `primary` block, and **placed light nodes** (here, spot lights) that live in the scene tree.
- Why authoring `primary` lighting turns off the automatic fallback from Example 01.
- What a spot light is and how you aim it.

## What is new since Example 01

Example 01 was a single node with no lighting. This one is a small tree -- the stool is the top node, and the bucket and two spot lights are its children -- and it adds a new top-level `primary` block that sets the scene-global ambient and directional light. Everything else -- the `container`, the empty `services`, and the single `map.wasm` module -- works exactly as explained in [Example 01](01-stool.md), so this walkthrough focuses only on what is new: `Children`, `Transform`, the `primary` lighting block, and light nodes.

## The files

| File | What it is |
|---|---|
| `stool-and-bucket.json` | The fabric. The whole scene -- stool, bucket, and two spot lights -- as one file, plus the scene-global lighting in its `primary` block. |
| `wasm/map.wasm` | The stock module that reads the scene from `data.scene` and builds it (the same one every map-managed example uses). |
| `assets/Stool.glb` | The stool model, reused from Example 01. |
| `assets/Bucket.glb` | The bucket model. |

## The fabric

```json
{
   "container": "example-stool-bucket",
   "services": [],
   "modules":
   [
      {
         "url": "wasm/map.wasm"
      }
   ],
   "primary":
   {
      "ambient":     { "fBrightness": 0.05, "fColor": "0xFFFFFF" },
      "directional": { "fBrightness": 0.5,  "fColor": "0xFFFFFF", "Rotation": [0.0, 0.5808, 0.6663, 0.468] }
   },
   "data":
   {
      "scene":
      { "Head": { "Self": "P-?" }, "Name": "Stool", "Resource": { "sReference": "assets/Stool.glb" }, "Children":
         [
            { "Head": { "Self": "P-?" }, "Name": "Bucket", "Resource": { "sReference": "assets/Bucket.glb" }, "Transform": { "Position": [0.0, 0.0, 0.428] } },
            { "Head": { "Self": "L-?" }, "Name": "Fill Light", "Type": { "bType": 2 }, "Transform": { "Position": [-0.5, -0.45, 0.514], "Rotation": [0.0, -0.0119, 0.3582, 0.9336] }, "Properties": { "fBrightness": 0.4, "fColor": "0xB8CCFF", "fAngleOpening": 40.0, "fAngleFalloff": 12.0 } },
            { "Head": { "Self": "L-?" }, "Name": "Rim Light",  "Type": { "bType": 2 }, "Transform": { "Position": [-0.15, 0.55, 0.814], "Rotation": [0.0, 0.2846, -0.5489, 0.786] }, "Properties": { "fBrightness": 0.4, "fColor": "0xFFE0B0", "fAngleOpening": 35.0, "fAngleFalloff": 10.0 } }
         ]
      }
   }
}
```

## The scene is now a tree

In Example 01 `data.scene` was a single node. Here that same node -- the stool -- gains an array of `Children`, and everything inside it becomes a child of the stool. A child belongs to its parent: it inherits the parent's place in the world, and if the parent ever moves, rotates, or scales, every child moves with it. You never write down who a node's parent is; the parent is simply whatever node the child is nested inside. That is why the bucket and the lights, written inside the stool's `Children`, are all children of the stool.

This is the mechanism you use to build things out of parts. Group the pieces under one node, position each piece relative to that node, and from then on you can move the whole assembly as a unit.

## Placing the bucket with a Transform

A **`Transform`** places a node relative to its parent. It has three optional parts: `Position` (metres, `[x, y, z]`), `Rotation` (a quaternion, `[x, y, z, w]`), and `Scale` (`[x, y, z]`). Anything you leave out defaults to "no change": position `[0, 0, 0]`, rotation `[0, 0, 0, 1]`, scale `[1, 1, 1]`. A node with no `Transform` at all sits exactly at its parent's origin -- which is what the stool itself does, so the stool sits at the scene origin.

The bucket needs to rest on the seat, so it gets a `Position`. Both of these models have their origin at the base -- the point the object stands on -- which makes the math simple. The stool is about 0.43 m tall, so the top of its seat is `0.428` m above the stool's base. Because the bucket's own origin is also at its base, setting the bucket `0.428` m up rests its bottom exactly on the seat. Up is the `z` axis, so this is `"Position": [0.0, 0.0, 0.428]` -- centered over the seat (`x` and `y` are 0) and raised to sit on it (`z` is 0.428). If you swap in a different model, use the height of whatever it stands on to find the surface, and set that as the bucket's `z`.

## Lighting the scene

A fabric provides light in two different ways, and this example uses both.

The first is **scene-global light**: a single ambient level and a single directional "sun" that apply to the whole scene at once. These are not objects in the scene tree; they are properties of the scene, declared in the top-level `primary` block. Only the *primary* fabric -- the one the browser loaded directly -- gets to set them, so a fabric that another one embeds cannot hijack the global lighting.

The second is **placed light nodes**: ordinary nodes in the scene tree, positioned and aimed like any other object. This example uses two of them, both spot lights.

**The fallback, and how to turn it off.** In Example 01 the fabric declared no lighting at all, so the engine supplied a fallback ambient light just so the model was not pure black. That fallback is tied specifically to the `primary` block: the moment a fabric sets `primary.ambient` or `primary.directional`, the fallback is gone and the scene's global light is exactly what you asked for. Placed light nodes on their own do not switch it off -- the fallback is about *global* light, so only global light replaces it. This example sets both ambient and directional, so from here on the look is yours.

### The `primary` block

```json
"primary":
{
   "ambient":     { "fBrightness": 0.05, "fColor": "0xFFFFFF" },
   "directional": { "fBrightness": 0.5,  "fColor": "0xFFFFFF", "Rotation": [0.0, 0.5808, 0.6663, 0.468] }
}
```

**`ambient`** is flat, directionless fill -- the same light reaching every surface from every angle, with no highlights and no shadows. It is the base level that keeps shadowed areas from going fully black. It takes two values:

- **`fBrightness`** -- the intensity. Here it is a very low `0.05`, just a faint floor; the real shaping comes from the directional and the spots. Turn it up toward `1.0` for a bright, shadowless overcast look; leave it near zero to let the directed lights do all the modeling.
- **`fColor`** -- an `0xRRGGBB` colour, white here. If you leave it out it defaults to white. A warm ambient reads like a room full of incandescent bounce; a cool one like open shade under a blue sky.

**`directional`** is a "sun": parallel light crossing the entire scene from one direction. Because the rays are parallel it has no position and does not fall off with distance -- only its *direction* matters, so it lights near and far objects identically. It takes:

- **`fBrightness`** -- the intensity, `0.5` here. This is the scene's main light, doing most of the shaping of the stool and bucket.
- **`fColor`** -- the colour, white here. Warm it toward amber for late-afternoon sun, or cool it toward pale blue for cold daylight.
- **`Rotation`** -- a quaternion that aims it. A directional light points along the identity forward direction (+X) after this quaternion rotates it, and the vector it ends up along is the direction the light *travels*. The value here sends it down and across from the front-right-above -- the same place a "key" light would sit -- so the sun is doing the job of the key. Absent a `Rotation` the sun points straight along +X. Aiming by quaternion is the same idea as aiming a spot (below); you normally compute it to face a direction rather than type the four numbers by hand.

### The fill and rim spot lights

The directional is the key -- the dominant light. Two placed **spot** lights finish the classic three-point setup, kept soft rather than harsh:

- **Fill Light** -- a dimmer light on the side opposite the key, lower down. Its only job is to soften the shadows the key casts so nothing goes fully black. It is tinted a cool `0xB8CCFF`, standing in for the blue of sky bounce.
- **Rim Light** -- a dim light behind and above the objects. It catches their top edges and separates them from the background. It is tinted a warm `0xFFE0B0`.

Each is a node with the light class id `"L-?"`. A spot is the kind of light you aim, like a stage light or a desk lamp. Its parts:

- **`Type.bType`** = `2` selects a **spot** light -- a light that sits at a position and casts a cone in a direction you choose, instead of shining every way at once. A spot is the natural choice for lighting a specific object: it stays where you put it and does not spill into other fabrics that might embed this one. (A `bType` of `1` is the other placed kind, a **point** light, which shines every way at once from its position. Ambient and directional are not node types at all -- they are the scene-global `primary` values above.)
- **`Transform.Position`** places the light relative to the stool, just like the bucket. The two positions put the fill to the front-left and the rim behind and above.
- **`Transform.Rotation`** aims the cone -- the same quaternion-aims-the-forward-direction idea as the directional's `Rotation`. Each spot is turned to point at the seat where the bucket sits. You rarely write these four numbers by hand; you either compute them to aim at a point or copy a working set and adjust.
- **`Properties.fAngleOpening`** and **`fAngleFalloff`** shape the cone, in degrees. `fAngleOpening` is how wide the beam is (smaller is a tighter spot); `fAngleFalloff` is how soft its edge is (smaller is a crisper circle). The values here -- opening 35 to 40, falloff 10 to 12 -- give a broad, gentle pool rather than a hard theatrical spot.
- **`Properties.fBrightness`** sets the intensity. Unlike the directional, a spot sits at a point and falls off with distance, so brightness and position work together -- moving a spot closer or raising its brightness both make it stronger. Both spots are `0.4` here, deliberately dimmer than the key.
- **`Properties.fColor`** sets the colour, as an `0xRRGGBB` value. A light with no colour set is white. All three of these spellings give the same warm amber:

```json
"fColor": 16755280
"fColor": "0xFFAA50"
"fColor": "#FFAA50"
```

## Deploying it

Deployment works exactly as in [Example 01](01-stool.md): host `stool-and-bucket.json` at a public address, make sure the relative references (`wasm/map.wasm`, `assets/Stool.glb`, `assets/Bucket.glb`) resolve to reachable files beside it, and give that address to the browser. This fabric and its assets are hosted under `https://cdn.rp1.com/sneeze/examples/`.

## What is next

Example 03 takes this exact scene and prepares it for the world: it pins the module to an exact version with a hash and signs the whole fabric into a `.msf`, which is the real, publishable form of a fabric.

---

[Example 01](01-stool.md) | [Example 03](03-signing.md) | [Home](../Home.md)
