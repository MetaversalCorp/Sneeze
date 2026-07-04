# Example 02 - A Bucket on the Stool

This example builds directly on [Example 01](../01-stool/README.md). We keep the same stool, set a bucket on its seat, and light the whole thing with three soft lights. By the end you will understand how one object becomes the parent of others, how to place a child exactly where you want it, and how to light a scene yourself instead of relying on the engine's fallback.

## ⚠️ The following warning paragraph is different from the one in Example 01
## ⚠️ This is not the preferred way to encode a scene

Loading a fabric with a hard-coded list of nodes stored in a JSON structure as shown below works today and is convenient while you are learning, but it is not how map nodes are meant to be transferred. Many fabrics will be excessively large: RP1's Earth fabric contains roughly half a billion nodes! Trying to store all of that data in a single JSON file is unmanageable, and transferring all of it at once would take hours to days. Normally, a metaverse browser interrogates a map service for relevant nodes within a reasonable proximity to the viewer based on object size and distance. A future example will introduce the concept of a map service and show how to use one instead of hardcoding your nodes directly inside a fabric file.

## What this example teaches

- How a scene becomes a *tree*: one node can have children, and a child moves with its parent.
- How to place a node precisely using a `Transform` (a position, and optionally a rotation and scale) measured relative to its parent.
- How to add your own lights, and why doing so turns off the automatic fallback light from Example 01.

## What is new since Example 01

Example 01 was a single node. This one is a small tree: the stool is the top node, and the bucket and the three lights are its children. Everything else - the `container`, the empty `services`, and the single `map.wasm` module - works exactly as explained in [Example 01](../01-stool/README.md), so this walkthrough focuses only on what is new: `Children`, `Transform`, and light nodes.

## The files

| File | What it is |
|---|---|
| `stool-and-bucket.json` | The fabric. The whole scene - stool, bucket, and three lights - as one file. |
| `wasm/map.wasm` | The stock module that reads the `data` block and builds the scene (same one every map-managed example uses). |
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
   "data":
      { "Head": { "Self": "P-?" }, "Name": "Stool", "Resource": { "sReference": "assets/Stool.glb" }, "Children": [
            { "Head": { "Self": "P-?" }, "Name": "Bucket", "Resource": { "sReference": "assets/Bucket.glb" }, "Transform": { "Position": [0.0, 0.428, 0.0] } },
            { "Head": { "Self": "L-?" }, "Name": "Key Light",  "Type": { "bType": 4 }, "Transform": { "Position": [0.45, 0.864, 0.5], "Rotation": [-0.2443, 0.3281, 0.0, 0.9125] }, "Properties": { "fBrightness": 1.0, "fOpeningAngle": 35.0, "fFalloffAngle": 10.0 } },
            { "Head": { "Self": "L-?" }, "Name": "Fill Light", "Type": { "bType": 4 }, "Transform": { "Position": [-0.5, 0.514, 0.45], "Rotation": [0.0122, -0.4068, 0.0, 0.9134] }, "Properties": { "fBrightness": 0.5, "fOpeningAngle": 40.0, "fFalloffAngle": 12.0 } },
            { "Head": { "Self": "L-?" }, "Name": "Rim Light",  "Type": { "bType": 4 }, "Transform": { "Position": [-0.15, 0.814, -0.55], "Rotation": [-0.8540, -0.4495, 0.0, 0.2619] }, "Properties": { "fBrightness": 0.5, "fOpeningAngle": 35.0, "fFalloffAngle": 10.0 } }
         ]
      }
}
```

## The scene is now a tree

In Example 01 the `data` block was a single node. Here that same node - the stool - gains an array of `Children`, and everything inside it becomes a child of the stool. A child belongs to its parent: it inherits the parent's place in the world, and if the parent ever moves, rotates, or scales, every child moves with it. You never write down who a node's parent is; the parent is simply whatever node the child is nested inside. That is why the bucket and the lights, written inside the stool's `Children`, are all children of the stool.

This is the mechanism you use to build things out of parts. Group the pieces under one node, position each piece relative to that node, and from then on you can move the whole assembly as a unit.

## Placing the bucket with a Transform

**`Transform`** places a node relative to its parent. It has three optional parts: `Position` (metres, `[x, y, z]`), `Rotation` (a quaternion, `[x, y, z, w]`), and `Scale` (`[x, y, z]`). Anything you leave out defaults to "no change": position `[0, 0, 0]`, rotation `[0, 0, 0, 1]`, scale `[1, 1, 1]`. A node with no `Transform` at all sits exactly at its parent's origin - which is what the stool itself does, so the stool sits at the scene origin.

The bucket needs to rest on the seat, so it gets a `Position`. Both of these models have their origin at the base - the point the object stands on - which makes the math simple. The top of the stool's seat is `0.428` m above the ground. Because the bucket's own origin is also at its base, setting the bucket `0.428` m up rests its bottom exactly on the seat.

## Lighting the scene

Example 01 had no lights, so the engine supplied a fallback lighting just so the model was not black. The moment a fabric provides *any* light of its own, that fallback switches off and the scene is lit entirely by what you author - so from here on, the look is yours to control.

This scene uses the classic three-light setup, chosen to be soft rather than harsh:

- **Key Light** - the main light, front and above and to one side. It does most of the lighting and establishes the primary shadows. It is the brightest of the three (`fBrightness` 1.0).
- **Fill Light** - a dimmer light on the opposite side, lower down. Its only job is to soften the shadows the key light casts, so nothing goes fully black (`fBrightness` 0.5).
- **Rim Light** - a dim light behind and above the objects. It catches the top edges of the stool and bucket and separates them from the background (`fBrightness` 0.5).

Each light is a node with the light class id `"L-?"`, and all three are **spot** lights - the kind of light you aim, like a stage light or a desk lamp. Its parts:

- **`Type.bType`** = `4` selects a **spot** light - a light that sits at a position and casts a cone of light in a direction you choose, instead of shining every way at once. A spot is the natural choice for lighting a specific object: it stays where you put it and does not spill into other fabrics that might embed this one. (Point, directional, and ambient lights also exist and are covered later.)
- **`Transform.Position`** places the light, relative to the stool, just like the bucket. The three positions put the key to the front-right-above, the fill to the front-left, and the rim behind and above.
- **`Transform.Rotation`** aims the light. A spot shines along the direction it faces, and the `Rotation` quaternion turns that direction toward the target - here, each one is turned to point at the seat where the bucket sits. You rarely write these four numbers by hand; you either compute them to aim at a point or copy a working set and adjust.
- **`Properties.fOpeningAngle`** and **`fFalloffAngle`** shape the cone, in degrees. `fOpeningAngle` is how wide the beam is (smaller is a tighter spot); `fFalloffAngle` is how soft its edge is (smaller is a crisper circle). The values here - opening 35 to 40, falloff 10 to 12 - give a broad, gentle pool of light rather than a hard theatrical spot.
- **`Properties.fBrightness`** sets the intensity. Like any positioned light, a spot falls off with distance, so brightness and position work together - moving a light closer or raising its brightness both make it stronger. These values are a gentle starting point; nudge them to taste.

Notice there is no color on these lights. A light with no color set is **white** by default, which is what you usually want. When you *do* want a colored light, set `Properties.fColor` to an ordinary `0xRRGGBB` value - either the decimal integer or a hex string. All three of these give the same warm amber:

```json
"Properties": { "fColor": 16755024, "fBrightness": 1.0 }
"Properties": { "fColor": "0xFFAA50", "fBrightness": 1.0 }
"Properties": { "fColor": "#FFAA50", "fBrightness": 1.0 }
```

## Deploying it

Deployment works exactly as in [Example 01](../01-stool/README.md): 
1. Upload `stool-and-bucket.json` to your own web server so it has a public address. A version of this fabric file is also located at `https://cdn.rp1.com/sneeze/examples/stool-and-bucket.json`.
2. Make sure everything the fabric references resolves to a reachable address. Because `wasm/map.wasm`, `assets/Stool.glb`, and `assets/Bucket.glb` are relative, they must sit beside `stool.json` - here, under `https://cdn.rp1.com/sneeze/examples/`.
3. Give that fabric address to the metaverse browser. The browser will fetch the fabric, then the wasm module and model, and build the scene.

The module and models in this example are already hosted on RP1's CDN, so you can point at those URLs as-is and it will load. For your own fabrics, though, do not depend on RP1's server - host your own copies of the modules and assets on a server you control and change the URLs in the fabric to match. That keeps your fabric working no matter what happens to anyone else's server.

## What is next

Example 03 introduces attaching a whole separate fabric as a child, so you can compose large spaces out of independently authored pieces.
