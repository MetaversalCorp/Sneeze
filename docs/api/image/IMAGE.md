---
title: IMAGE::Decode (function reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Image.h
  - src/deps/stb/Image.cpp
verified: b487fd1
nav:
  prev: api/image/index.md
  next: ../../guides/index.md
---

# `IMAGE::Decode`

A single free function that decodes encoded image bytes held in memory into raw 8-bit RGBA pixels. Unlike the rest of the API reference, `IMAGE` is a **namespace**, not a class — there is no object to construct, no state, and no lifecycle. The function is a thin, self-contained wrapper over the public-domain **stb_image** decoder. This page is its exact contract.

```cpp
namespace SNEEZE { namespace IMAGE {

bool Decode (const std::vector<uint8_t>& aEncoded,
             int&                        nWidth,
             int&                        nHeight,
             std::vector<uint8_t>&       aPixels);

} } // namespace SNEEZE::IMAGE
```

---

## Role

`Decode` is the engine's one entry point for turning a compressed image file — **PNG, JPEG, BMP, GIF**, and the other formats stb_image understands — into pixels the renderer can upload as a texture. It exists so that texture handling has a single, format-agnostic decode step: the [network](../network/index.md) layer fetches the encoded bytes, the [scene](../scene/NODE.md)'s `NODE` passes them here, and the resulting RGBA buffer goes to the [viewport](../viewport/index.md).

The output is always **8-bit RGBA**: four bytes per pixel (red, green, blue, alpha), row-major, top-to-bottom, regardless of the source format's native channel count. The underlying decoder forces four output channels, so a grayscale or RGB source is expanded to RGBA on decode.

---

## Threading and pitfalls

**Stateless and reentrant.** The function holds no shared state; separate calls on separate threads with separate arguments are independent. In the engine it is typically called on a network fetch thread when a texture's bytes arrive.

**Output buffer size.** On success, `aPixels` holds exactly `nWidth * nHeight * 4` bytes. Compute the size from the returned dimensions, not from the input size.

**Failure is total and clean.** On any failure the function returns `false`, leaves `aPixels` empty, and sets `nWidth` and `nHeight` to zero — never a partial result. An empty input vector is treated as failure. Always check the return value before reading the outputs.

---

## Function

### `bool Decode (const std::vector<uint8_t>& aEncoded, int& nWidth, int& nHeight, std::vector<uint8_t>& aPixels)`
- **Purpose.** Decode an in-memory encoded image into 8-bit RGBA pixels.
- **Parameters.**
- `aEncoded` — the encoded image bytes (a complete PNG/JPEG/BMP/GIF/… file in memory).
- `nWidth` — **out**; set to the decoded width in pixels (0 on failure).
- `nHeight` — **out**; set to the decoded height in pixels (0 on failure).
- `aPixels` — **out**; filled with `nWidth * nHeight * 4` bytes of RGBA on success, cleared on failure.
- **Returns.** `true` on success; `false` if `aEncoded` is empty or the decoder rejects the data.
- **Notes.** Backed by `stbi_load_from_memory` with a forced 4-channel (RGBA) output. The decoder's temporary buffer is freed internally; the caller owns only `aPixels`.

---

## Example

```cpp
std::vector<uint8_t> aEncoded = /* fetched PNG/JPEG bytes */;
int nWidth = 0, nHeight = 0;
std::vector<uint8_t> aPixels;

if (SNEEZE::IMAGE::Decode (aEncoded, nWidth, nHeight, aPixels))
{
   // aPixels.size() == nWidth * nHeight * 4, RGBA, top-to-bottom
   // upload to a texture, etc.
}
```

---

## See also

- [Scene API — NODE](../scene/NODE.md) — decodes a fetched texture with this function.
- [Network API](../network/index.md) — fetches the encoded bytes this consumes.
- [Viewport API](../viewport/index.md) — consumes the decoded RGBA pixels.

---

[Image API](index.md) · Prev: [index](index.md) · Next: [Guides](../../guides/index.md)
