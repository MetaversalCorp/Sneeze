---
title: Coding Conventions
tier: Architecture
audience: [contributor]
sources:
  - src/sneeze/Engine.h
  - src/sneeze/Types.h
verified: b487fd1
nav:
  prev: architecture/trust-and-isolation.md
  next: systems/engine.md
---

# Coding Conventions

This page is for contributors. Sneeze holds itself to a specific set of design principles and a distinctive code style, and both are deliberate — they are the reason the codebase reads consistently and tears down predictably. If you are going to read the source or add to it, these are the rules in force. They are not arbitrary preferences; several of them encode hard lessons (the threading rules in particular came from real crashes). Read this before the Systems and API tiers if you intend to contribute; read it after, as background, if you only intend to evaluate or embed.

A warning up front: the style will look unusual if you are used to mainstream modern C++. It is internally consistent and applied uniformly. When in doubt, match the surrounding code.

---

## Design principles

These are the architectural commitments. They shape *structure*, not just formatting.

**Symmetry above all else.** Initialization and shutdown are exact mirrors. Whatever order you bring things up in, you tear them down in precise reverse — at every level: engine, context, subsystem, object. There are no exceptions. This is what makes failure handling and teardown a single, predictable path rather than a thicket of special cases. (See
[Lifecycle](lifecycle.md).)

**Add before init, remove after shutdown.** When a parent manages a list of owned children, it adds the child to the list *before* `Initialize` and removes it *after* teardown. A child must be visible to other threads during both its startup and its destruction. (See
[Threading](threading.md) for the compositor/viewport case that makes this load-bearing.)

**No leaky abstractions.** Module boundaries are hard. The renderer never includes windowing headers; the engine has no dependency on any windowing or input library; the host passes raw input deltas. Each module knows only its own concern.

**Namespace everything.** All code lives in `namespace SNEEZE`, with sub-namespaces per module where appropriate (`SNEEZE::persona`, `SNEEZE::DEP`, …). No global symbols.

**Prepare for concurrency.** Data structures are designed read/write-separable even when currently single-threaded, because GPU and multi-thread parallelism is coming.

**Own the math.** `sneeze/Types.h` defines the canonical vector and quaternion types. Modules use these, never renderer- or library-specific types.

**One pointer of ownership, one path to everything else.** An object stores a pointer to its *owner* and reaches everything else by walking up to the owner and back down. It does not cache convenience copies of things it can already reach. An agent reaches the engine via `Pool()->...->Engine()`, not a cached `m_pEngine`. This keeps ownership unambiguous.

---

## The threading contract (do not "simplify")

Two threading rules are written down precisely because well-meaning edits keep trying to undo them. Both are covered in depth in [Threading](threading.md); restated here as conventions:

- **`THREAD::Wait` is not shutdown-aware.** Neither `Wait` overload reads the shutdown flag. Shutdown lives in the *predicate* a subclass passes to `Wait` (e.g. `Job()` / `Tick()`) and in the metronome loop — never inside `Wait`. Do not add `IsShutdown()` checks inside `Wait` or duplicate them in `Main()` lambdas.
- **Every `THREAD` subclass calls `Join()` first in its own destructor.** Because C++ destroys derived members before the base destructor runs, the base `~THREAD()` join would be too late for any `Main()` that touches derived members. Join in the derived destructor, before its members are gone.

If you find yourself "cleaning up" either of these, stop — they are correct as written.

---

## C++ style

The style is consistent and uniformly applied. The headline rules:

**Layout**

- **3-space indentation.** Not 2, not 4, never tabs.
- **Allman braces.** Opening `{` on its own line; `else` on its own line.
- **Space before `(`** everywhere: `if (`, `for (`, `FunctionName (`.
- **Double-space around `&&` and `||`:** `if (a  &&  b)`.
- **Vertical alignment** of related declarations and initializer lists.
- **No split lines.** Keep a call or declaration on one line regardless of length.
- **No executable code in headers.** Headers declare; `.cpp` files implement.

**Naming**

- **Types, classes, constants: `ALL_CAPS`** — `ENGINE`, `FABRIC`, `MAP_OBJECT`, `RMCOBJECT`.
- **Functions: capitalized** — `Initialize()`, `Node_Add()`.
- **`Object_Action` method names** — the object first, then the action, then a qualifier: `Node_Add()`, `Fabric_Spawn()`, `Queue_Post_Fetch()`, `FrameBuffer_Capture()`. Getters drop the action: `Node_Root()`, `Fabric_Parent()`. Boolean getters use `IsX()`. Simple owner-pointer accessors are just the name: `Engine()`, `Scene()`.
- **No Get/Set prefixes** — overload the same name for getter and setter: `AgentIndex()` / `AgentIndex(n)`.
- **Parameter names match member names** — if the member is `m_nAgentIz`, the setter parameter is `nAgentIz`, so the relationship is visually obvious.
- **No pluralized collections** — `m_apNode`, not `m_apNodes`; the prefix already says it is a collection. Qualify with purpose: `m_pFabric_Parent`, `m_pNode_Root`.

**Hungarian prefixes.** Every variable carries a type prefix; members add `m_`. The prefix carries the type so the rest of the name can describe *purpose*.

| Prefix | Type | Prefix | Type |
|---|---|---|---|
| `p` | pointer / object | `s` | string |
| `n` | integer | `d` | double / float |
| `b` | bool | `a` | array |
| `tm` | time | `fn` | function |
| `ump` | unordered_map | `map` | ordered map |
| `mx` | mutex | `cv` | condition variable |
| `th` | thread | `pth` | pointer-to-thread |
| `tw` | (type) word / 48-bit object index | | |

**Control flow**

- **Single return at the end** of a function — one `return`, last line. No early returns.
- **No narrating comments.** Comments explain non-obvious *intent*, trade-offs, or constraints the code cannot convey — never restate what the code plainly does.

**Other**

- **Cross-language invariance.** Class names and algorithms stay identical across the project's JavaScript and C++ implementations, so the two read the same.
- **Apache 2.0 header** on every `.h` and `.cpp`.

> A footgun the conventions explicitly call out: `=` binds looser than `&&`, so an assignment-in-condition must be parenthesized — `if ((m_pX = new X ())  &&  m_pX->Initialize ()) { … }`. Without the inner parentheses the expression parses wrong.

---

## A note on C++ standard

The engine core targets a conservative subset (broadly C++14-compatible: `std::thread`, `std::mutex`, `std::chrono`, `std::function`). Some C++17 usage exists (`std::optional`, `std::filesystem`, structured bindings), and a few third-party headers assume C++17 — those are isolated behind module boundaries. There is a standing intent to audit toward C++14 for broader compatibility; treat new core code as conservatively portable.

---

## Where these show up

You will see every rule above the moment you open a source file. The pimpl pattern (a public class holding a single `Impl*`) is used pervasively for the larger subsystems — `ENGINE`, `CONTEXT`, `SCENE`, `FABRIC`, `NODE`, `NETWORK`, `STORAGE`, `CONSOLE`, `VIEWPORT` — to keep public headers free of implementation detail. The [API tier](../api/index.md) documents each class's public surface; the implementation conventions here explain what you will find behind the `Impl`.

---

## See also

- [Lifecycle](lifecycle.md) — symmetry and add-before-init in action.
- [Threading Model](threading.md) — the full `Wait`/`Join` contract.
- [Contributing](../guides/contributing.md) — repository layout and how to add a subsystem.
- [Architecture Overview](overview.md) — the ownership discipline these rules serve.

---

[Home](../Home.md) · Prev: [Trust & Isolation](trust-and-isolation.md) · Next: [Engine system](../systems/engine.md)
