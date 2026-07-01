---
title: PERSONA (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Persona.h
  - src/persona/Persona.cpp
verified: b487fd1
nav:
  prev: api/persona/index.md
  next: api/image/index.md
---

# `PERSONA`

A temporary local identity proxy. A user "logs in" with a first and optional second name; the combined name is hashed (SHA-256) into a short key that scopes WebAssembly stores and persistent [storage](../storage/index.md). It is a deliberate stub — there is no password, credential, or verification of any kind — that occupies the identity slot so the rest of the engine can be built and tested against a stable per-user key. For the conceptual picture see the [Persona system](../../systems/persona.md) page; this page is the exact behavior of every public member.

`PERSONA` lives in the **`SNEEZE::persona`** namespace (note the lowercase sub-namespace), not directly in `SNEEZE`.

```cpp
namespace SNEEZE { namespace persona {

class PERSONA
{
public:
   explicit PERSONA (SNEEZE::ENGINE* pEngine);

   bool IsLoggedIn () const;
   void Login  (const std::string& sFirst, const std::string& sSecond);
   void Logout ();
   const std::string& Name () const;
   const std::string& Hash () const;

private:
   static std::string ComputeHash (const std::string& sInput);
   // ...
};

} } // namespace SNEEZE::persona
```

---

## Role and ownership

- **Owned by** the [`ENGINE`](../../systems/engine.md), one per engine, reachable as `ENGINE::Persona()`. Constructed with a back-pointer to the engine, used for logging.
- **Holds** a logged-in flag, the display name, and the truncated SHA-256 hash. Nothing else.
- **Feeds** the container identity ([`CID`](../container/index.md)) and the storage path: its hash is the first of the three keys that isolate per-user state.

---

## Lifecycle

A persona is constructed logged-out: empty name, but the hash defaults to `000000000000` (12 zeros) so the persona path segment is never empty. `Login` populates the name and hash and sets the flag; `Logout` clears the name, drops the flag, and resets the hash back to the `000000000000` default. The same instance is reused across logins — there is one persona per engine, not one per session.

---

## Threading and pitfalls

**Not synchronized.** `PERSONA` holds no locks and mutates its fields directly. It is expected to be set up early and read (via `Hash()`) on the loading paths that build container identities. Do not call `Login`/`Logout` concurrently with fabric loads that are reading the hash.

**`Name()` and `Hash()` return references into the object.** They are valid only while the persona is unchanged; a subsequent `Login`/`Logout` mutates the underlying strings. Copy if you need to retain the value across such a call.

**The hash is truncated.** It is the first 12 hex characters of the SHA-256, chosen as a short filesystem-path component. It is an identity *key*, not a cryptographic commitment; do not treat it as collision-proof.

**Not authentication.** Logging in establishes an identity by name alone. Never rely on a persona as a security boundary.

---

## Construction

```cpp
explicit PERSONA (SNEEZE::ENGINE* pEngine);
```

### `PERSONA(pEngine)`
- **Purpose.** Construct a logged-out persona owned by `pEngine`.
- **Parameters.** `pEngine` — the owning engine, used for logging login/logout. Must outlive the persona.

---

## Methods

```cpp
bool IsLoggedIn () const;
void Login  (const std::string& sFirst, const std::string& sSecond);
void Logout ();
const std::string& Name () const;
const std::string& Hash () const;
```

### `bool IsLoggedIn () const`
- **Purpose / Returns.** `true` between a successful `Login` and the next `Logout`; otherwise `false`.

### `void Login (const std::string& sFirst, const std::string& sSecond)`
- **Purpose.** Establish the active identity. Composes the name as `"First.Second"`, or just `"First"` if `sSecond` is empty; computes the persona hash; sets the logged-in flag; logs the event.
- **Parameters.** `sFirst` — the first name (required in practice); `sSecond` — the optional second name.
- **Notes.** Overwrites any previous login. There is no validation of the names.

### `void Logout ()`
- **Purpose.** Clear the active identity: resets the logged-in flag, name, and hash; logs the event.

### `const std::string& Name () const`
- **Purpose / Returns.** The composed display name (`"First.Second"` or `"First"`), or empty when logged out. Returned **by reference** — see [pitfalls](#threading-and-pitfalls).

### `const std::string& Hash () const`
- **Purpose / Returns.** The persona hash: the first 12 hex characters of the SHA-256 of the name, or the `000000000000` default when logged out (never empty). This is the value the storage and container layers use to scope per-user state. Returned **by reference**.

---

## See also

- [Persona system](../../systems/persona.md) — design, the storage-scoping role, limitations.
- [Storage API](../storage/index.md) — what the persona hash scopes.
- [Container API](../container/index.md) — the identity triple the persona hash feeds.

---

[Persona API](index.md) · Prev: [index](index.md) · Next: [Image API](../image/index.md)
