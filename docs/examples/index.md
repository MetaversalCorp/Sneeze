---
title: Examples
tier: Examples
audience: [author]
sources: []
verified: 10a5afd
---

# Examples

Hands-on, copy-and-edit walkthroughs for building spatial fabrics. Where the [Guides](../guides/index.md) explain the authoring path in concept, these examples give you a complete, working fabric you can read line by line, copy, and change. They are ordered from the simplest possible scene to progressively richer ones, so read them in order the first time through.

Each example is a self-contained folder in the repository under `examples/`, containing the fabric source you can copy and a walkthrough that explains every piece. The fabrics and their shared assets are also hosted for loading at `https://cdn.rp1.com/sneeze/examples/`.

## The examples

1. [A Single Stool](01-stool.md) -- the smallest fabric that puts something real on screen: one 3D model, described as data, with no code and no lights of your own. It introduces the parts every other example builds on: the container, modules, the `data` node tree, attaching a model to a node, and how relative addresses resolve.
2. [A Bucket on the Stool](02-stool-and-bucket.md) -- turns the single node into a small tree: a bucket and two spot lights become children of the stool. It introduces `Children`, placing a node precisely with a `Transform`, and the two ways to light a scene: scene-global ambient and directional light in the `primary` block, and placed spot lights you aim at the scene.
3. [Publishing a Signed Fabric](03-signing.md) -- takes that same scene and prepares it for the world. It introduces pinning a module to an exact version with a hash, the signing credentials (the test certificates and how to get your own), and signing the fabric into a `.msf` using the browser's built-in signing.
   - [Building SignMsf from source](building-signmsf.md) -- an optional side page for building the standalone signing tool from the Sneeze source, instead of using the browser.

More examples follow these, each adding a single new idea.

---

[Home](../Home.md)
