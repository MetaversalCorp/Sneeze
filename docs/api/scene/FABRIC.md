---
title: FABRIC (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Scene.h
  - src/context/scene/Fabric.cpp
verified: b487fd1
nav:
  prev: api/scene/SCENE.md
  next: api/scene/NODE.md
---

# `FABRIC`

A spatial fabric's branch in the scene graph. One `FABRIC` represents one source's content — bound to a [`CONTAINER`](../container/index.md) (its identity and sandbox) and an [`MSF`](../msf/index.md) (its signed manifest) — and owns the tree of [`NODE`](NODE.md)s rooted at `Node_Root()`. Fabrics also form their own parent/child hierarchy that mirrors the attachment relationships in the scene tree. For the conceptual picture, see the [Scene system](../../systems/scene.md); this page is the exact behavior of every public member.

```cpp
class FABRIC
{
public:
   FABRIC (SCENE* pScene, CONTAINER* pContainer, uint64_t twFabricIx,
           NODE* pNode_Attach, MSF* pMsf);
   ~FABRIC ();
   // ... see sections below
protected:
   class Impl;
   Impl* m_pImpl;
};
```

---

## Role and ownership

- **Created and owned by** the [`SCENE`](SCENE.md), via `SCENE::Fabric_Open`. Never construct a fabric directly — the scene assigns its index and registers it.
- **Bound to** a `CONTAINER` (passed in at construction; the fabric does not own it — the [`CONTEXT`](../context/index.md) does) and an `MSF` (which the fabric *does* effectively own — the scene deletes it when the fabric closes).
- **Owns** its node tree (rooted at `Node_Root()`), its set of WASM module instances inside the container, and its list of child fabrics.
- **Attaches** to a `NODE` in its parent fabric's tree — the *attachment node*. The root fabric has no attachment node (`nullptr`).

The owner chain a fabric sits on is `NODE → FABRIC → SCENE → CONTEXT → ENGINE`. A fabric reaches engine services through `Scene()`, never through a cached pointer.

> `m_pImpl` is `protected` rather than `private` for historical reasons (a former > `FABRIC_ROOT` subclass). Nothing derives from `FABRIC` today; treat it as private.

---

## Lifecycle and the loading cascade

A fabric is brought to life in three steps, and torn down in the reverse order:

1. **Construct.** The constructor records its scene, container, index, attachment node, and MSF, derives its parent fabric from the attachment node, and — as a side effect — **adds itself to the attachment node's child-fabric list** (`pNode_Attach->Fabric_Add(this)`). Construction therefore mutates the parent node; it is not a pure value-init.
2. **Initialize.** `Initialize(sUrl)` records the URL and, if the fabric has an MSF, starts an asynchronous fetch for every WASM module the MSF declares. Each completed module is compiled into a WASM instance in the container. If the MSF declares no modules, the fabric completes immediately.
3. **Destruct.** The destructor cancels any still-pending module fetches, closes the root node through the container (`Container()->Node_Close`, which cascades to the entire subtree), closes every WASM instance it opened in the container (`Container()->Instance_Close`), logs any leaked child fabrics, and removes itself from its attachment node.

**WASM fetches are asynchronous and run on network threads.** `OnWasmReady` / `OnWasmFailed` are invoked from the network completion path, not the thread that called `Initialize`. They mutate fabric state (the module list) and the container (instances).

---

## Threading and pitfalls

**`m_mxFabric` (a `std::recursive_mutex`) guards only the child-fabric list.** It is held by `Fabric_Add` and `Fabric_Remove`. The module list and node root are *not* guarded by it; they are touched from the fetch-completion path and the loading path, which are not designed to run concurrently for the same fabric.

**Construction and destruction reach into neighbors.** The constructor calls `Fabric_Add` on the attachment node; the destructor calls `Fabric_Remove`. `Fabric_Remove` additionally nulls the *child's* parent pointer. Because closing a fabric cascades into node teardown that calls back into [`SCENE`](SCENE.md)'s recursive-locked methods, the whole teardown path relies on the scene's mutex being recursive — see [SCENE → Threading](SCENE.md#threading-locking-and-pitfalls).

**Child-fabric leak is logged, not prevented.** If a fabric is destroyed while it still has child fabrics registered, the destructor logs an error ("Leaked N child fabric(s)") but does not force-close them. Close child fabrics before their parent.

**The pointer from `Fabric_Find` is not lifetime-guarded.** If you obtained this fabric via `SCENE::Fabric_Find`, nothing prevents it being closed and freed by another thread. Do not retain it across anything that could trigger a fabric close.

**In-flight module fetches are cancelled on destruction**, by deleting the pending fetch helpers (which close their network files). A fetch that completes *after* destruction has begun is handled by that cancellation; do not call into a fabric you are destroying.

---

## Construction and destruction

```cpp
FABRIC (SCENE* pScene, CONTAINER* pContainer, uint64_t twFabricIx,
        NODE* pNode_Attach, MSF* pMsf);
~FABRIC ();
```

### `FABRIC(pScene, pContainer, twFabricIx, pNode_Attach, pMsf)`
- **Purpose.** Construct a fabric bound to a scene, container, and MSF, attached to a node.
- **Parameters.**
- `pScene` — the owning scene (required).
- `pContainer` — the identity/sandbox this fabric's content runs under (required).
- `twFabricIx` — the scene-global fabric index assigned by the scene.
- `pNode_Attach` — the node in the parent fabric's tree this fabric mounts on; `nullptr` for the root fabric.
- `pMsf` — the parsed, verified manifest; `nullptr` for the root fabric.
- **Side effect.** If `pNode_Attach` is non-null, the fabric adds itself to that node's child-fabric list during construction.
- **Note.** Call `Initialize` next. Do not construct directly — go through `SCENE::Fabric_Open`.

### `~FABRIC()`
- **Purpose.** Tear the fabric down: cancel pending fetches, close the root node (cascading the subtree), close all WASM instances, detach from the attachment node.
- **Pitfalls.** Logs an error if child fabrics remain. Runs the node-teardown cascade, which re-enters scene locking — must not be invoked concurrently with traversal of the same subtree.

---

## Lifecycle method

```cpp
bool Initialize (const std::string& sUrl);
```

### `bool Initialize (const std::string& sUrl)`
- **Purpose.** Record the fabric's URL and begin loading. If the fabric has an MSF, start an asynchronous fetch for each declared WASM module; if none are declared, complete immediately.
- **Parameters.** `sUrl` — the fabric's address (stored, returned later by `Url()`).
- **Returns.** `true`. (The method currently always succeeds at *starting* the load; module fetch outcomes are reported asynchronously via `OnWasmReady` / `OnWasmFailed`.)
- **Notes.** Logs module counts to the engine log and to the container's console stream.

---

## Accessors

```cpp
SCENE*             Scene         () const;
FABRIC*            Fabric_Parent () const;
NODE*              Node_Root     () const;
NODE*              Node_Attach   () const;
CONTAINER*         Container     () const;
uint64_t           FabricIx      () const;
MSF*               Msf           () const;
const std::string& Url           () const;
```

| Accessor | Returns | Notes |
|---|---|---|
| `Scene()` | The owning scene. | Never null for a live fabric. |
| `Fabric_Parent()` | The parent fabric, or null. | Null for the root fabric, or after the parent detaches this fabric (`Fabric_Remove` nulls it). |
| `Node_Root()` | The root node of this fabric's tree, or null. | Set indirectly when the root node is constructed; cleared when it is closed. |
| `Node_Attach()` | The node this fabric mounts on, or null. | Null for the root fabric. |
| `Container()` | The bound container. | Identity + sandbox; owned by the context, not the fabric. |
| `FabricIx()` | The scene-global fabric index. | The key under which `SCENE::Fabric_Find` registers it. |
| `Msf()` | The fabric's manifest, or null. | Null for the root fabric. |
| `Url()` | The fabric's URL by const reference. | Empty until `Initialize` runs. Returned **by reference** into the fabric — do not retain it past the fabric's lifetime (a dangling-reference foot-gun if the fabric is then closed). |

---

## Mutator

```cpp
void Node_Root (NODE* pNode_Root);
```

### `void Node_Root (NODE* pNode_Root)`
- **Purpose.** Set (or clear) this fabric's root node.
- **Parameters.** `pNode_Root` — the new root node, or `nullptr` to clear it.
- **Notes.** Called by the `NODE` constructor (when a node is created with no parent) and by the `NODE` destructor (passing `nullptr`). Not intended for general use.

---

## Child-fabric methods (internal)

```cpp
void Fabric_Add    (FABRIC* pFabric_Child);
void Fabric_Remove (FABRIC* pFabric_Child);
```

### `void Fabric_Add (FABRIC* pFabric_Child)`
- **Purpose.** Register a child fabric. Called from the child's constructor via its attachment node, not by application code.
- **Parameters.** `pFabric_Child` — the child to register.
- **Thread-safety.** Takes `m_mxFabric`.

### `void Fabric_Remove (FABRIC* pFabric_Child)`
- **Purpose.** Unregister a child fabric and null the child's parent pointer.
- **Parameters.** `pFabric_Child` — the child to remove.
- **Thread-safety.** Takes `m_mxFabric`. No-op if the child is not registered.

---

## Fetch callbacks (internal)

```cpp
void OnWasmReady  (FILE* pFile, const std::string& sUrl, const std::string& sHash);
void OnWasmFailed (FILE* pFile, const std::string& sUrl);
```

Delegated from the file-local WASM fetch helper; invoked on a network fetch thread.

### `void OnWasmReady (FILE* pFile, const std::string& sUrl, const std::string& sHash)`
- **Purpose.** A module's bytes have arrived. Read them, open a WASM instance for them in the container, record the module, and log the result. When the last pending fetch resolves, the fabric reports how many instances are active.
- **Parameters.** `pFile` — the completed network file; `sUrl` / `sHash` — the module's address and integrity hash.
- **Returns.** Nothing.

### `void OnWasmFailed (FILE* pFile, const std::string& sUrl)`
- **Purpose.** A module fetch failed. Log it to the engine log and the container's console stream, and remove the pending fetch.
- **Parameters.** `pFile` — the failed request's file; `sUrl` — the module's address.
- **Returns.** Nothing.

---

## See also

- [Scene system](../../systems/scene.md) — design, loading flow, limitations.
- [SCENE](SCENE.md) — creates and owns fabrics (`Fabric_Open` / `Fabric_Close`).
- [NODE](NODE.md) — the tree a fabric owns.
- [Container API](../container/index.md) — the identity and WASM sandbox a fabric is bound to.
- [MSF API](../msf/index.md) — the signed manifest that declares a fabric's modules.

---

[Scene API](index.md) · Prev: [SCENE](SCENE.md) · Next: [NODE](NODE.md)
