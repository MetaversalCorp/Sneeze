---
title: Guides
tier: Guides
sources: []
verified: 92fdc1c
---

# Guides

Task-oriented walkthroughs for the things people most want to do with Sneeze -- author the spatial fabrics it renders, embed it in a host application, and build and contribute to the engine itself.

## Authoring spatial fabrics

How to build the 3D spaces the engine loads. A fabric is to a spatial browser what an HTML page is to a web browser: a file you write, host, and open. This is a guided path -- read it in order the first time.

1. [Authoring spatial fabrics](authoring-fabrics.md) -- what a fabric is, the two authoring paths, the load pipeline, and an honest list of what renders today. Start here.
2. [Your first fabric](authoring-first-fabric.md) -- the fastest end-to-end walkthrough: a tiny scene, on screen, then signed.
3. [The MSF file and signing](authoring-msf-and-signing.md) -- the full payload schema, plain JSON vs signed JWS, the trust model, and the `SignMsf` tool.
4. [Static scenes: the data tree](authoring-static-scenes.md) -- the complete JSON node schema for the map-managed path, with worked examples.
5. [Dynamic scenes with WASM](authoring-dynamic-scenes.md) -- building a scene from code: the toolchain, the module lifecycle, and the node-building calls.
6. [Scene object reference](authoring-scene-reference.md) -- every object kind the engine draws, the fields it reads, and exactly what appears.
7. [WASM host API for content](authoring-wasm-host-api.md) -- the complete list of functions a module can call: console, storage, scene, and timer.

## Embedding, building, and contributing

- [Embedding Sneeze](embedding-sneeze.md) -- write a minimal host: implement the host interfaces, create the engine, open a context, drive a frame, present pixels.
- [Building Sneeze](building.md) -- the two-tree build model, in brief, with pointers to the full build guide.
- [Contributing](contributing.md) -- repository layout, adding a subsystem, and the documentation-maintenance workflow.

---

[Home](../Home.md)
