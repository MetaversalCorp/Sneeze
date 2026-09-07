# glTF — Model Loading (fastgltf wrapper)

The `gltf` module is the engine's dependency wrapper around
[fastgltf](https://github.com/spnda/fastgltf). It parses a glTF or GLB blob held
in memory into a renderer-agnostic CPU model (`GLTF_MODEL`). It is a pure loader:
it does not touch ANARI, Filament, or any renderer type — per Jonathan Hale's
guidance, glTF loading belongs in the engine framework (Sneeze), not in the
rendering-engine abstraction (ANARI). The renderer-ready flattening of a
`GLTF_MODEL` into draw calls lives one layer up, in `viewport/GltfMesh.cpp` (see
`Viewport.md`).

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
takes a byte buffer (binary GLB, VRM 1.0 `.vrm`, or glTF JSON) and fills
`model`. On failure it leaves `model` empty, sets `sError`, and returns false.
fastgltf auto-detects the container from the bytes. A VRM 1.0 file is a GLB
with extra `VRMC_*` extensions (humanoid, MToon, spring bones). Those names
are often listed in `extensionsRequired`; fastgltf does not implement them and
would otherwise reject the file. `Load` drops only the VRM/VRMC names from
`extensionsRequired` so the ordinary glTF mesh, skin, and **embedded** images
still parse. The instance methods (`GLTF(ENGINE*)`, `Initialize()`)
exist for symmetry with the other dependency wrappers; parsing itself needs no
engine state.

`Load` reads only the **default scene**, flattens the mesh/material/texture
tables it references, records skins (`JOINTS_0` / `WEIGHTS_0` plus inverse-bind
matrices, including when those attributes are Draco-compressed), and records
the scene's root node indices. Geometry is converted to flat, renderer-ready
streams; **only triangle primitives** are mapped (points, lines, and triangle
strips/fans are skipped). Image bytes are kept **encoded** (decoding to RGBA8
is deferred to the renderer layer via `SNEEZE::IMAGE::Decode`, so the loader
pulls in no image codec). Skinned primitives keep 4 joint indices and 4
weights per vertex. The loader does not pose: `Gltf_Render_Model_Build` packs
bind-pose bone palettes and leaves rest-pose vertices in place; Halogen GPU
skinning (`HALOGEN_GEOMETRY_SKINNING`) applies palettes per instance.

Extensions enabled on the parser so REQUIRED files are not rejected:
`KHR_mesh_quantization`, `KHR_materials_emissive_strength`,
`KHR_materials_clearcoat`, `KHR_texture_transform`, `KHR_materials_unlit`,
`KHR_texture_basisu`, `EXT_texture_webp`, `EXT_meshopt_compression`, and
`KHR_draco_mesh_compression`. VRM 1.0 (`VRMC_vrm`, `VRMC_materials_mtoon`,
`VRMC_springBone`, `VRMC_node_constraint`, ...) is accepted by stripping those
names from `extensionsRequired`. After the ordinary glTF tables are mapped,
`Load` re-reads the JSON for VRM extras: `VRMC_materials_mtoon` marks a
dielectric (`dMetallic` 0, `dRoughness` 1, `bUnlit` false) and copies
`shadeColorFactor` into `shadeColor`; `KHR_materials_unlit` without MToon
sets `bUnlit`. UniVRM stamps both extensions on MToon materials -- MToon
wins, so the renderer lights them instead of emitting raw albedo. `eAlpha`
comes from glTF `alphaMode` / `alphaCutoff`. When those stay OPAQUE, MToon
`transparentWithZWrite` and VRM 0 `materialProperties.floatProperties._BlendMode`
(1 = cutout / MASK, 2+ = BLEND) fill in. `VRMC_node_constraint`
on nodes fills `GLTF_MODEL::aConstraint`. Constraint
evaluation (aim at bind, rotation/roll as rest-delta) happens in
`Gltf_Render_Model_Build`, not here. Meshopt-compressed buffer views are decoded with
the decode units vendored at `src/deps/meshoptimizer` (`allocator` /
`vertexcodec` / `indexcodec` / `vertexfilter`) before accessors are read.
Draco-compressed primitives (`KHR_draco_mesh_compression`) are decoded with
the decode units vendored at `src/deps/draco` into the same float
position/normal/texcoord and uint32 index streams. Basisu/webp textures parse but stay encoded; stbi cannot decode
KTX2/WebP, so those albedos remain empty until a later decode path exists.

Each `GLTF_PRIMITIVE` carries a model-space position AABB (`aBoundMin` /
`aBoundMax`, `bBound`) computed while filling positions, so the renderer
bridge can bound the model from 8 corners instead of walking every vertex.

## Data Model

A `GLTF_MODEL` is a faithful CPU image of the loaded asset's default scene:

| Type | Contents |
|------|----------|
| `GLTF_MODEL` | The whole asset: `aMesh`, `aMaterial`, `aTexture`, `aNode`, `aSkin`, `aRoot` (root node indices of the default scene), and `aConstraint` (`VRMC_node_constraint` records). |
| `GLTF_NODE` | One hierarchy node: local column-major `transform` (translation in `d[12..14]`), `nMesh` index (-1 = none), `nSkin` index (-1 = none), and `aChild` indices. Children compose under the parent transform. |
| `GLTF_SKIN` | Joint node indices (`aJoint`), matching inverse-bind matrices (`aInverseBind`, identity when the accessor is omitted), and optional `nSkeleton` root (-1 = none). |
| `GLTF_MESH` | A list of `GLTF_PRIMITIVE` surfaces. |
| `GLTF_PRIMITIVE` | One triangle surface: flat `aPosition` (xyz), optional `aNormal` (xyz) / `aTexCoord` (uv), `aIndex` (uint32), optional `aJoint` / `aWeight` (4 influences per vertex from `JOINTS_0` / `WEIGHTS_0`), `nMaterial` index (-1 = none), and a model-space AABB (`aBoundMin`/`aBoundMax`, `bBound`). Normals/texcoords/joints may be empty when the source omits them. Non-triangle primitives are not loaded. |
| `GLTF_MATERIAL` | Metallic-roughness PBR: `baseColor[4]`, `dMetallic`, `dRoughness`, `emissive[3]`, `shadeColor[3]` (MToon), `nBaseColorTexture` index (-1 = none), `bUnlit` (`KHR_materials_unlit` without MToon), `eAlpha` (`kOPAQUE` / `kMASK` / `kBLEND` from glTF `alphaMode`), and `dAlphaCutoff` (MASK only, glTF default 0.5). |
| `GLTF_CONSTRAINT` | One `VRMC_node_constraint`: destination `nNode`, `nSource`, `eKind` (`kROTATION` / `kAIM` / `kROLL`), `nAxis` (aim: 0=+X .. 5=-Z; roll: 0=X, 1=Y, 2=Z), and `dWeight`. |
| `GLTF_TEXTURE` | Raw encoded image bytes (`aEncoded`, e.g. PNG/JPEG) exactly as embedded in the asset -- not decoded. VRM 1.0 ships textures this way (GLB buffer views), not as external files. |

## Files

| File | Contents |
|------|----------|
| `Gltf.h` | `DEP::GLTF` loader + the `GLTF_MODEL` / `GLTF_NODE` / `GLTF_SKIN` / `GLTF_MESH` / `GLTF_PRIMITIVE` / `GLTF_MATERIAL` / `GLTF_CONSTRAINT` / `GLTF_TEXTURE` CPU model structs |
| `Gltf.cpp` | `GLTF::Load` -- fastgltf parse (meshopt + Draco decode), default-scene traversal, stream/material/texture/skin extraction, VRM extras (`bUnlit`, `eAlpha`, `aConstraint`) |
| `../meshoptimizer/` | Decode-only meshoptimizer units compiled into Sneeze (CI does not have Filament's clone) |
| `../draco/` | Decode-only Draco units compiled into Sneeze; features header stays in `draco_config/` |