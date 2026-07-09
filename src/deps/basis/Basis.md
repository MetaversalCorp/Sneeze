# deps/basis

Thin wrapper (`SNEEZE::BASIS`) around the Basis Universal **transcoder** for
decoding KTX2 / `.basis` GPU-supercompressed textures — the format glTF exposes
through the `KHR_texture_basisu` extension, which `stb_image` cannot read.

- `BASIS::Available ()` — inits the transcoder lookup tables (idempotent) and
  reports readiness. Called from `GLTF::Initialize` (also the dependency link
  self-test for now).
- `BASIS::FORMAT` — transcode targets: the GPU block formats (`kFORMAT_BC7`,
  `kFORMAT_ASTC_4x4`, `kFORMAT_ETC2_RGBA8`) plus `kFORMAT_RGBA8` (uncompressed
  fallback) and `kFORMAT_None`.
- `BASIS::Format_Anari (fmt)` — the ANARI `compressedImage2D` `format` string
  for a compressed target (e.g. `"BC7"`), or null for None/RGBA8.
- `BASIS::Target_Choose (szDeviceFormat)` — picks the best target (BC7 > ASTC >
  ETC2) from a device's advertised compressed-format list.
- `BASIS::Transcode (aEncoded, fmt, w, h, aOut)` — parses the KTX2 container
  (`basist::ktx2_transcoder`) and transcodes image level 0 to `fmt`'s blocks
  (or RGBA8 for `kFORMAT_RGBA8`). Returns false on a non-KTX2 blob or an
  unsupported target.

## Flow (GPU-compressed path)

1. `deps/gltf/Gltf.cpp` enables `fastgltf::Extensions::KHR_texture_basisu`, and
   per texture prefers `basisuImageIndex`, marking `GLTF_TEXTURE::bBasis`.
2. `context/viewport/GltfMesh.cpp` does **not** decode basis textures (the GPU's
   supported formats aren't known there); it carries the raw blob through to the
   draw list as `MESH_DATA::pTextureEncoded`. Non-basis textures still decode to
   RGBA8 via `IMAGE::Decode`.
3. `context/viewport/AnariRenderer.cpp` picks a preferred target once at
   `Initialize` (`Target_Choose` over the device's `halogen.textureFormats`
   property, gated on the `EXT_SAMPLER_COMPRESSED_IMAGE2D` device extension),
   then per basis mesh transcodes the blob and binds it:
   - a GPU block format via a `compressedImage2D` sampler (`image` =
     `ARRAY1D<UINT8>`, `format` string, `size`) when one is available, or
   - **RGBA8** via a plain `image2D` sampler (`ARRAY2D<UFIXED8_VEC4>`) as the
     fallback when the device advertised no compressed format.

   A transcode failure leaves the surface on its base-color factor.

The device side lives in Halogen: `Library.cpp` advertises
`EXT_SAMPLER_COMPRESSED_IMAGE2D`, `Device.cpp` reports GPU-supported formats via
the `halogen.textureFormats` property (probing `filament::Texture::
isTextureFormatSupported`), and `Sampler.cpp::commitCompressedImage2D` uploads
the blocks as a compressed Filament texture.

## Dependency

Backed by `basisu_transcoder` (deps/basisu.cmake, transcoder-only build of
BinomialLLC/basis_universal, Apache-2.0). Header self-contained; located by
`src/cmake/FindBasisu.cmake` (`BASISU_INCLUDE` / `BASISU_LIB`).

## Notes

`Transcode` handles a single 2D image (level 0, layer 0, face 0) — the shape
`KHR_texture_basisu` binds. Mipmaps, cubemaps, and array layers are not yet
surfaced. The KTX2 blob may be UASTC or ETC1S, Zstandard-supercompressed or not
(the transcoder lib is built with `BASISD_SUPPORT_KTX2` and `_KTX2_ZSTD`).
