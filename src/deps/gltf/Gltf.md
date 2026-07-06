# glTF — Model Loading (fastgltf wrapper)

The `gltf` module is the engine's dependency wrapper around
[fastgltf](https://github.com/spnda/fastgltf). It parses a glTF or GLB blob held
in memory into a renderer-agnostic CPU model (`GLTF_MODEL`). It is a pure loader:
it does not touch ANARI, Filament, or any renderer type — per Jonathan Hale's
guidance, glTF loading belongs in the engine framework (Sneeze), not in the
rendering-engine abstraction (ANARI). The renderer-ready flattening of a
`GLTF_MODEL` into draw calls — and the per-context `GLTF_MODEL_CACHE` that
shares one built model among all nodes referencing the same URL — lives one
layer up, in `viewport/GltfMesh.cpp` (see `Viewport.md`).

All source lives in `src/deps/gltf/`. Public to the rest of Sneeze via
`gltf/Gltf.h`; everything is in namespace `SNEEZE::DEP`.

## GLTF

```cpp
DEP::GLTF_MODEL model;
std::string     sError;

if (DEP::GLTF::Load (pData, nLen, model, sError))
{
   // model is a faithful CPU image of the asset's default scene
}
```

`GLTF::Load(pData, nLen, model, sError)` is a **static** parse entry point — it
takes a byte buffer (binary GLB or glTF JSON) and fills `model`. On failure it
leaves `model` empty, sets `sError`, and returns false. fastgltf auto-detects the
container from the bytes. The instance methods (`GLTF(ENGINE*)`, `Initialize()`)
exist for symmetry with the other dependency wrappers; parsing itself needs no
engine state.

`Load` reads only the **default scene**, flattens the mesh/material/texture
tables it references, and records the scene's root node indices. Geometry is
converted to flat, renderer-ready streams; image bytes are kept **encoded**
(decoding to RGBA8 is deferred to the renderer layer via `SNEEZE::IMAGE::Decode`,
so the loader pulls in no image codec).

## Data Model

A `GLTF_MODEL` is a faithful CPU image of the loaded asset's default scene:

| Type | Contents |
|------|----------|
| `GLTF_MODEL` | The whole asset: `aMesh`, `aMaterial`, `aTexture`, `aNode`, and `aRoot` (root node indices of the default scene). |
| `GLTF_NODE` | One hierarchy node: local column-major `transform` (translation in `d[12..14]`), `nMesh` index (−1 = none), and `aChild` indices. Children compose under the parent transform. |
| `GLTF_MESH` | A list of `GLTF_PRIMITIVE` surfaces. |
| `GLTF_PRIMITIVE` | One drawable surface: flat `aPosition` (xyz), optional `aNormal` (xyz) / `aTexCoord` (uv), `aIndex` (uint32), and `nMaterial` index (−1 = none). Normals/texcoords may be empty when the source omits them. |
| `GLTF_MATERIAL` | Metallic-roughness PBR: `baseColor[4]`, `dMetallic`, `dRoughness`, `emissive[3]`, and `nBaseColorTexture` index (−1 = none). |
| `GLTF_TEXTURE` | Raw encoded image bytes (`aEncoded`, e.g. PNG/JPEG) exactly as embedded in the asset — not decoded. |

## Files

| File | Contents |
|------|----------|
| `Gltf.h` | `DEP::GLTF` loader + the `GLTF_MODEL` / `GLTF_NODE` / `GLTF_MESH` / `GLTF_PRIMITIVE` / `GLTF_MATERIAL` / `GLTF_TEXTURE` CPU model structs |
| `Gltf.cpp` | `GLTF::Load` — fastgltf parse, default-scene traversal, stream/material/texture extraction |