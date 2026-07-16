# API Overview

This page ties the SDK classes together. Read it once before the per-class pages; it explains the object model, the module lifecycle, and how a module is structured.

## The object model is rooted at FABRIC

A running module is a **WASM instance**. One instance can serve **many fabrics** - the engine calls your `Open` once per fabric that loads your module. Everything you can do for a given fabric hangs off the [`FABRIC`](FABRIC.md) handle you are given at `Open`:

```
FABRIC (the root handle for one open fabric)
- Console ()   -> CONSOLE    developer-console logging
- Storage ()   -> STORAGE    persistent JSON, per scope
- Data ()      -> DATA       the fabric's read-only config "Data" tree
- Scene ()     -> SCENE      build the node tree
```

The subsystem objects (`CONSOLE`, `STORAGE`, `DATA`, `SCENE`) are **zero-cost views**: each is just the fabric index in a typed wrapper. Calling `Console ()` allocates nothing; it hands back a handle that knows which fabric it belongs to. This is why there is one `Console_Log` and not a `Fabric_Console` round-trip - the fabric handle *is* the routing key.

There is exactly one of each subsystem per fabric (they are singletons), which is why they need no identifier beyond the fabric. Nodes are different: there are many per fabric, so a [`NODE`](NODE.md) carries its own object index and is mutated by that index.

## The Open snapshot

At `Open` the engine also pushes an immutable [Open snapshot](SNAPSHOT.md): a JSON document it synthesized for this fabric, copied into your memory through the ABI's `Alloc`/`Open`/`Free` handshake. The SDK parses it **once, privately** - before your `Open` runs - so a module never touches the raw JSON or its transient buffer. You read the parsed sections through the same [`FABRIC`](FABRIC.md) handle:

```
FABRIC (also exposes the immutable snapshot)
- Location ()   -> LOCATION    the fabric URL, split into parts
- Resource ()   -> RESOURCE    the launching resource's identity
- Container ()  -> CONTAINER   container identity and trust
- Signature ()  -> SIGNATURE   MSF verification detail
- Agent ()      -> AGENT       host/engine identity (a navigator analog)
- Services ()   -> [SERVICE]   declared services
- Modules ()    -> [MODULE]    declared WASM modules
```

`LOCATION` is derived guest-side by splitting the launching resource's reference; the rest are read straight from the parsed snapshot.

The fabric's open-ended configuration `Data` block is **not** in the snapshot. It is served on demand, read-only, through [`DATA`](DATA.md) - the same path-addressed model as storage, so a large data block never inflates the `Open` blob.

## The module lifecycle

You implement the [`INSTANCE`](INSTANCE.md) trait and register it with the `instance!` macro. The engine drives it:

```
Init ()                        once, when the module is first loaded
Open (pFabric)                 once per fabric that loads the module
   ... the fabric is live: build nodes, read data, store state ...
Close (pFabric)                that fabric unloaded
Shutdown ()                    the module is unloading
```

`Init` and `Shutdown` bracket the module's whole life. Each `Open`/`Close` pair brackets one fabric. Because one instance can hold several fabrics open at once, never assume `Open` and `Close` are one-to-one in time - key any per-fabric state by `FABRIC::Index ()`.

## Building a scene

Scene construction is a two-step pattern:

1. Build a [`SNEEZE_ABI_MAPOBJECT`](MAPOBJECT.md) in guest memory with a fluent builder - pick a class factory (`Physical ()`, `Celestial ()`, `Light ()`, ...), then chain setters (`Name`, `Reference`, `Position`, ...).
2. Hand it to [`SCENE`](SCENE.md): `Node_Root (&obj)` for the fabric root, or `Node_Open (&obj)` for a child (set the child's parent index on the object first). Each returns a [`NODE`](NODE.md).

After creation you mutate a live node through its `NODE` handle (`Position`, `Scale`, `Name`, `Resource`, `Panel`, ...). The map object is the *creation* template; the node is the *live* object.

`Scene::Node_Map (sPath)` is a shortcut: it asks the engine to build an entire node subtree directly from a path in the fabric's `Data` block, without you constructing each map object.

## What is not here yet

The ABI reserves numbers for subsystems and methods that are declared but not yet implemented in the engine: `NETWORK` (fetch), `VIEWPORT` (camera get/set), the `SCENE` global-lighting and background methods, and `NODE::Rotation`. These appear in `sneeze_abi.h` marked "not implemented yet" and have no SDK wrapper - do not rely on them until they land. Event delivery through `Notify` is likewise reserved for a later item and is currently inert.

## See also

- [Incorporating the ABI](../incorporating-the-abi.md) - the wire format beneath these objects and how to set up a project.
- [FABRIC](FABRIC.md) - the entry point every example starts from.
- [INSTANCE](INSTANCE.md) - the lifecycle you implement.
