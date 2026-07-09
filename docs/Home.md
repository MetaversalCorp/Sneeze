---
title: Sneeze Documentation
tier: Meta
sources: []
verified: b487fd1
---

# Sneeze Documentation

**Sneeze is the Open Metaverse Browser Engine** — a reusable software library that renders, runs, and secures interactive 3D spaces delivered over the network. It is to a metaverse browser what a web rendering engine is to a web browser: the component that turns a remote address into a live, navigable experience. Sneeze is not itself an application; it is embedded inside a host application that supplies a window and input, and in return Sneeze produces the rendered world.

This wiki is the reference manual for Sneeze. It is written for readers coming in cold — whether you are evaluating the technology, integrating it into your own product, or contributing to the engine itself.

---

## Choose your path

**I want to understand what this is and why it matters.** Start with [What is the Open Metaverse Browser?](overview/what-is-omb.md), then [Core Concepts](overview/core-concepts.md) and [The Standards Sneeze Builds On](overview/standards.md).

**I want to build a 3D space (author a fabric).** Start with the [Examples](examples/index.md) — complete, working fabrics you can copy and edit, walking from the simplest possible scene through placing objects, adding lights, and publishing a signed fabric.

**I want to embed Sneeze in my own application.** Read the [Overview](overview/what-is-omb.md) for grounding, then [Architecture Overview](architecture/overview.md), then the [Embedding Sneeze](guides/embedding-sneeze.md) guide and the [API Reference](api/index.md).

**I want to contribute to the engine.** Read the [Architecture](architecture/overview.md) tier in full, then the [Systems](systems/index.md) pages for the areas you will touch, and the [Contributing](guides/contributing.md) guide.

---

## The five tiers

The documentation is organized as a descent from ideas to implementation. Read top to bottom the first time through.

### 1. Overview — what and why
- [What is the Open Metaverse Browser?](overview/what-is-omb.md)
- [Core Concepts](overview/core-concepts.md)
- [The Standards Sneeze Builds On](overview/standards.md)

### 2. Architecture — how it fits together
- [Architecture Overview](architecture/overview.md)
- [Lifecycle](architecture/lifecycle.md)
- [Fabric Loading](architecture/fabric-loading.md)
- [Threading Model](architecture/threading.md)
- [Trust & Isolation](architecture/trust-and-isolation.md)
- [Coding Conventions](architecture/conventions.md)

### 3. Systems — one page per subsystem
See the [Systems index](systems/index.md) for the full map. Highlights: [Engine](systems/engine.md) · [Control](systems/control.md) · [Context](systems/context.md) · [Container](systems/container.md) · [Scene](systems/scene.md) · [Network](systems/network.md) · [Storage](systems/storage.md) · [Console](systems/console.md) · [Viewport](systems/viewport.md) · [MSF](systems/msf.md) · [WASM](systems/wasm.md)

### 4. API — public class reference
-  [API index](api/index.md)

### 5. Guides — task-oriented how-tos
- [Guides index](guides/index.md)
- [Embedding Sneeze](guides/embedding-sneeze.md)
- [Building Sneeze](guides/building.md)
- [Contributing](guides/contributing.md).

### 6. Examples — copy-and-edit walkthroughs
- [Examples index](examples/index.md)
- [01 - A Single Stool](examples/01-stool.md)
- [02 - A Bucket on the Stool](examples/02-stool-and-bucket.md)
- [03 - Publishing a Signed Fabric](examples/03-signing.md)
  - [Building SignMsf from source](examples/building-signmsf.md)

---

## About this wiki

These pages are authored from the source code, which is the single source of truth, and are written and kept current by AI coding agents working directly from the source tree. Each page declares the code files it documents and the commit it was last verified against, so the wiki can be checked for drift as the engine evolves. If you maintain or extend this documentation, read [STYLE.md](STYLE.md) first.
