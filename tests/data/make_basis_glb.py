#!/usr/bin/env python3
# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# Generates tests/data/basis-quad.glb: a minimal self-contained GLB -- a single
# textured quad whose base-color texture is the KTX2/Basis Universal image
# kodim23.ktx2, referenced through the KHR_texture_basisu extension. This is the
# fixture GltfTest uses to verify the loader recognizes basis textures and
# routes their encoded bytes to the renderer (no external tooling needed; the
# KTX2 is embedded byte-for-byte into the GLB binary chunk).
#
# Usage:  python tests/data/make_basis_glb.py

import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
KTX2 = os.path.join(HERE, "kodim23.ktx2")
OUT  = os.path.join(HERE, "basis-quad.glb")


def pad4(data, fill):
    while len(data) % 4 != 0:
        data += fill
    return data


def main():
    with open(KTX2, "rb") as f:
        ktx2 = f.read()

    # Unit quad in the XY plane, +Z normal, with UVs.
    positions = struct.pack("<12f",
        -0.5, -0.5, 0.0,   0.5, -0.5, 0.0,
         0.5,  0.5, 0.0,  -0.5,  0.5, 0.0)
    texcoords = struct.pack("<8f",
        0.0, 1.0,  1.0, 1.0,  1.0, 0.0,  0.0, 0.0)
    indices = struct.pack("<6H", 0, 1, 2, 0, 2, 3)

    # BIN layout: geometry, then (4-aligned) the KTX2 image.
    off_pos = 0
    off_uv  = off_pos + len(positions)          # 48
    off_idx = off_uv + len(texcoords)           # 80
    geom_end = off_idx + len(indices)           # 92 (already 4-aligned)
    assert geom_end % 4 == 0
    off_img = geom_end

    bin_data = positions + texcoords + indices + ktx2
    buffer_len = len(bin_data)

    gltf = {
        "asset": {"version": "2.0", "generator": "make_basis_glb.py"},
        "extensionsUsed": ["KHR_texture_basisu"],
        "extensionsRequired": ["KHR_texture_basisu"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                "indices": 2,
                "material": 0,
            }]
        }],
        "materials": [{
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}
        }],
        "textures": [{
            "extensions": {"KHR_texture_basisu": {"source": 0}}
        }],
        "images": [{"bufferView": 3, "mimeType": "image/ktx2"}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-0.5, -0.5, 0.0], "max": [0.5, 0.5, 0.0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": off_pos, "byteLength": len(positions), "target": 34962},
            {"buffer": 0, "byteOffset": off_uv,  "byteLength": len(texcoords), "target": 34962},
            {"buffer": 0, "byteOffset": off_idx, "byteLength": len(indices),   "target": 34963},
            {"buffer": 0, "byteOffset": off_img, "byteLength": len(ktx2)},
        ],
        "buffers": [{"byteLength": buffer_len}],
    }

    json_chunk = pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    bin_chunk = pad4(bin_data, b"\x00")

    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    with open(OUT, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))       # glTF, version 2
        f.write(struct.pack("<II", len(json_chunk), 0x4E4F534A)) # JSON
        f.write(json_chunk)
        f.write(struct.pack("<II", len(bin_chunk), 0x004E4942))  # BIN\0
        f.write(bin_chunk)

    print("wrote {} ({} bytes; KTX2 {} bytes at BIN+{})".format(
        OUT, total, len(ktx2), off_img))


if __name__ == "__main__":
    main()
