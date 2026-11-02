# Panel briefing

How to author an in-scene panel. A panel is an RmlUi document drawn on a quad in the world. The document is the RML+CSS source. Size, raster, and facing are fields on the node.

Leave a field at `0` to keep the default. Omitted `Properties` are all zeros.

## Declare the node

The class letter is `N`. `"N-?"` asks the container for the next free index.

```json
{
   "Head": { "Self": "N-?" },
   "Name": "Panel",
   "Bound": { "Max": [1.6, 0.9, 0] },
   "Transform": {
      "Position": [0, 0, 1.5],
      "Rotation": [0, 0, 0, 1],
      "Scale": [1, 1, 1]
   },
   "Properties": {
      "nWidth": 0,
      "nHeight": 0,
      "bDensity": 1,
      "bScale": 0,
      "bBillboard": 0
   }
}
```

`Bound.Max.x` and `Bound.Max.y` must both be greater than 0 or the panel is not drawn. `z` is unused. Every `Properties` value is a JSON number.

The RML source is separate from these fields. A fabric passes it through the panel module (`Data.sPanel` for `panel.wasm`) or through `Node_Panel`. If no source is set, the engine uses its built-in document.

World frame is right-handed and Z-up. The quad lies in the node's local XY. Local +Z is the face normal. `Rotation` is a quaternion; `[0, 0, 0, 1]` is identity.

Panels do not change how the 3D scene is framed. Their position is the node's world translation.

## Properties

| Field | Values | Default (`0`) |
|---|---|---|
| `nWidth`, `nHeight` | CSS pixels, or `0` for automatic | Aspect-correct canvas, 512 px tall |
| `bDensity` | `0` or `1` = 1x; `2`, `3`, or `4` = that many times the pixels | 1x |
| `bScale` | `0` FOV size, `1` node size | FOV size |
| `bBillboard` | `0` world up, `1` camera up, `2` off | Look at the eye, world up |

`bScale` and `bBillboard` are independent. FOV size can still face the camera, and a node-sized panel can still be turned by its rotation.

## Texture size

`nWidth` and `nHeight` are the layout size in CSS pixels.

- Both `0`. Height is 512. Width is `512 * aspect`, so texels stay square on the quad.
- Only `nHeight` set. Width follows the aspect.
- Only `nWidth` set. Height follows the aspect.
- Both set. Those exact dimensions. If they do not match the quad, the image stretches.

Aspect is `Bound.Max.x / Bound.Max.y` in FOV mode. In node-size mode it is `(Bound.Max.x * scaleX) / (Bound.Max.y * scaleY)`, so a non-uniform scale makes a wider or taller canvas.

Each axis is capped at 4096. `bDensity` is lowered until width and height times density also fit in 4096.

## Sharp text

Two things are already done for every panel. The canvas matches the quad, and glyph atlases are sampled bilinearly. You do not set either of those.

What you do control is how many texels a glyph gets.

- A `px` length is one texture pixel. Raising `bDensity` on a `px` document makes the canvas larger and the type a smaller fraction of it.
- A `dp` length is that many pixels times `bDensity`. The layout stays the same and the glyphs gain texels.

Use `dp` plus `bDensity` when the document should keep its layout and look sharper:

```css
.title { font-size: 26dp; }
.body  { font-size: 16dp; }
```

```json
"Properties": { "nHeight": 720, "bDensity": 2 }
```

That raster is 720 CSS px tall, times 2, so 1440 texture pixels tall. A `26dp` title is 52 texture pixels.

Stay in `px` only if you scale the font with the canvas. A 26px font on a 512-tall canvas is the same fraction of the panel as a 52px font on a 1024-tall canvas, with twice the texels:

```json
"Properties": { "nHeight": 1024, "bDensity": 1 }
```

```css
.title { font-size: 52px; }
```

At the default FOV size the panel is 0.26 of the framed view (1.3 render units tall). A 26px glyph on a 512 canvas is about 5 percent of that, which is only a few screen pixels. More texels, or walking up to a node-sized panel, is what makes the type read cleanly.

Percentage width and height plus padding need `box-sizing: border-box` and `display: block`, or the card runs off the canvas and the corners clip square.

## On-screen size

`bScale: 0` (FOV). The panel keeps a constant fraction of the framed view. Height is 1.3 render units. Width is that height times `Bound.Max.x / Bound.Max.y`. `Transform.Scale` is not used. This is the right default for a label that should stay readable in a room and in a solar system.

`bScale: 1` (node). The panel has a real size in metres:

```
width  = Bound.Max.x * accumulated scale X
height = Bound.Max.y * accumulated scale Y
```

Accumulated scale is this node's `Transform.Scale` composed with its ancestors. Scale defaults to `[1, 1, 1]`, so the panel is `Bound.Max` metres until you change scale. A parent scaled by 2 doubles the panel. The same metres are then fitted into the scene like any other object, so a 1.6 m panel is large in a room and tiny across a planetary scene.

Example: a 3.2 m by 0.9 m sign.

```json
"Bound": { "Max": [1.6, 0.9, 0] },
"Transform": { "Scale": [2, 1, 1] },
"Properties": { "bScale": 1 }
```

## Facing

`bBillboard: 0`. The panel looks at the eye. Up is world up `(0, 0, 1)`. A panel off to the side still tips toward the eye, so its top stays level with the horizon and can look rolled against the camera picture.

`bBillboard: 1`. The panel looks at the eye, and its up matches the camera's screen up, including camera pitch. If the view direction is parallel to that up, it falls back to world up.

`bBillboard: 2`. The node's `Rotation` is used. The panel can be mounted on a wall. It can be seen edge-on. The renderer draws both sides.

Position follows the node in every mode.

## Recipes

Readable overlay, default size, camera-aligned, sharper type. Fonts in `dp`.

```json
"Bound": { "Max": [1.6, 0.9, 0] },
"Properties": {
   "nHeight": 720,
   "bDensity": 2,
   "bScale": 0,
   "bBillboard": 1
}
```

Wall plate, 1.6 m by 0.9 m, facing from the node. Identity rotation faces local +X with +Z up; set `Rotation` to aim the plate.

```json
"Bound": { "Max": [1.6, 0.9, 0] },
"Transform": {
   "Position": [4, 0, 1.2],
   "Rotation": [0, 0, 0, 1],
   "Scale": [1, 1, 1]
},
"Properties": {
   "nWidth": 1600,
   "nHeight": 900,
   "bDensity": 2,
   "bScale": 1,
   "bBillboard": 2
}
```

`1600` by `900` matches the 1.6:0.9 bound, so the texture is not stretched. `bDensity: 2` with `dp` fonts doubles the texels inside that layout.

## WASM

A guest that zero-fills the node and only sets `Bound` gets the defaults: FOV size, world-up billboard, 1x, aspect-correct 512-tall raster. `panel.wasm` does that (`Bound` 1.6 by 1.0) and loads the document from `Data.sPanel`.

To override from a guest, write the same `Properties` bytes the JSON keys fill. The RML string is still passed separately; it does not fit in the node.
