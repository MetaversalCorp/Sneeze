---
title: MSF (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Msf.h
  - src/context/msf/MsfFile.cpp
verified: b487fd1
nav:
  prev: api/msf/index.md
  next: api/msf/CHAIN.md
---

# `MSF`

The class that owns a Metaversal Spatial Fabric file's entire lifecycle: parsing a signed (JWS) or unsigned (plain JSON) document, verifying its signature and certificate chain as two separate steps, managing the certificate chain used for signing, and giving typed access to the payload. For the conceptual picture — why verification is split in two, how the SPKI fingerprint identifies a source, how results become trust levels — see the [MSF system](../../systems/msf.md) page. This page is the exact behavior of every public member.

```cpp
class MSF
{
public:
   struct SERVICE { /* ... */ };
   struct MODULE  { /* ... */ };
   struct CERT    { /* ... */ };
   class  CHAIN   { /* ... */ };   // see CHAIN.md

   explicit MSF (ENGINE* pEngine = nullptr);
   ~MSF ();
   MSF (const MSF&) = delete;            // non-copyable, non-movable
   // ... see sections below
};
```

---

## Role and ownership

- **Constructed with** an optional `ENGINE*` back-pointer, used only for logging. An MSF with a null engine works but logs nothing.
- **Owned by** whatever drives the load. During fabric loading the [scene](../scene/index.md) constructs and parses the MSF, the [context](../context/index.md) reads its verification results to build a [container](../container/index.md) identity, and the resulting [`FABRIC`](../scene/FABRIC.md) takes effective ownership and deletes it on close.
- **Non-copyable and non-movable.** The class holds a `CHAIN` member (which owns BoringSSL handles) and is deliberately pinned. Pass it by pointer or reference.
- **Self-contained.** It holds the raw JWS, the parsed JSON payload, the decoded `x5c` entries, per-certificate info records, and the verification status. It reaches no other subsystem except the engine for logging.

---

## Lifecycle

The intended call order is **construct → parse → verify → read**, or, for producing a file, **construct → compose → add certs → sign**.

1. **Parse.** `Parse(sJws, sUrl)` resets all state and populates the payload, certificate info, and identity fingerprint. It does *not* verify anything. Parsed data is available regardless of whether verification later succeeds.
2. **Verify (optional, two steps).** `VerifySignature()` checks integrity against the leaf key; `VerifyChain()` checks provenance against the trust store. Either may be skipped; the corresponding `IsX` status simply stays false.
3. **Read.** Typed accessors (`Container`, `Services`, `Modules`, `Successor`), identity accessors (`Fingerprint`, `Organization`, …), and status accessors (`IsSignatureValid`, …).

The composing path (`SetContainer`/`AddService`/`AddModule`/`AddCert` → `Sign`) is the mirror image used by signing tools.

---

## Threading and pitfalls

**`MSF` is not internally synchronized.** It holds no locks. An instance is meant to be owned and driven by one load at a time (typically constructed and parsed on the network fetch thread that delivered its bytes). Do not share a single `MSF` across threads.

**`Parse` is the reset point.** Every call to `Parse` clears *all* prior state — payload, certs, fingerprint, status. Re-parsing throws away earlier verification results. Verify after the parse you care about.

**A valid signature is not trust.** `IsSignatureValid` only means the leaf's key signed these bytes. Whether that leaf is trustworthy is `IsChainTrusted`. Always consult both; the [system page](../../systems/msf.md#trust-levels) explains how they combine.

**Identity fields are derived at parse time, not verify time.** `Fingerprint`, `Organization`, and `OrganizationHash` are computed from the leaf certificate during `Parse`, before any verification. A fingerprint therefore exists even for a document whose signature or chain will fail — do not read a non-empty fingerprint as evidence of trust.

**Unsigned documents parse successfully.** A plain-JSON input (detected by its first meaningful character being `{` or `[`) parses, but has no certificates; `VerifySignature` and `VerifyChain` both fail with "no certificates in JWS header," and the fingerprint is a synthetic hash of URL+content.

---

## Construction and destruction

```cpp
explicit MSF (ENGINE* pEngine = nullptr);
~MSF ();
```

### `MSF(pEngine)`
- **Purpose.** Construct an empty MSF. All status flags start false and the payload is null.
- **Parameters.** `pEngine` — optional engine for logging parse/sign/verify failures; may be null.
- **Notes.** Call `Parse` (to read) or the composing setters (to write) next.

### `~MSF()`
- **Purpose.** Destroy the MSF. The nested `CHAIN` member releases its BoringSSL handles.

---

## Parse and sign

```cpp
bool        Parse (const std::string& sJws, const std::string& sUrl);
std::string Sign  (const std::string& sPrivateKeyPem, const std::string& sAlgorithm = "RS256");
```

### `bool Parse (const std::string& sJws, const std::string& sUrl)`
- **Purpose.** Parse an MSF document. Resets all state, distinguishes JWS from plain JSON (after skipping an optional UTF-8 BOM and leading whitespace, a leading `{` or `[` means plain JSON; otherwise an input containing a `.` is treated as JWS — checking for JSON first stops dots inside a JSON payload from being read as JWS separators), decodes the `x5c` certificate chain and payload for a JWS (or the JSON directly otherwise), and derives the leaf fingerprint, organization, and organization hash.
- **Parameters.** `sJws` — the document, as JWS compact serialization or plain JSON. `sUrl` — the document's source URL; always required (it seeds the synthetic fingerprint for unsigned documents).
- **Returns.** `true` if the document parsed; `false` on malformed input (logged via the engine if present).
- **Notes / pitfalls.** Does **not** verify. Clears all previous state — see [Threading and pitfalls](#threading-and-pitfalls).

### `std::string Sign (const std::string& sPrivateKeyPem, const std::string& sAlgorithm = "RS256")`
- **Purpose.** Produce a signed JWS from the current payload and certificate chain. Converts each added PEM certificate to base64-DER for the `x5c` header, serializes the payload into the `data` claim, and signs the compact form.
- **Parameters.** `sPrivateKeyPem` — the leaf's private key in PEM. `sAlgorithm` — one of `RS256`/`RS384`/`RS512`/`ES256`/`ES384`/`ES512` (default `RS256`).
- **Returns.** The JWS string, or an **empty string** on failure (unknown algorithm, a certificate that fails PEM→DER conversion, or a signing exception).
- **Notes.** Add certificates with `AddCert` (leaf first) and build the payload with the typed setters before calling. Used by signing tooling, not by content loading.

---

## Verification

```cpp
bool VerifySignature ();
bool VerifyChain ();
```

### `bool VerifySignature ()`
- **Purpose.** Verify the JWS signature — the **integrity** check. Extracts the public key from the leaf certificate and verifies the compact serialization against it using the algorithm named in the header.
- **Returns.** `true` if the signature matches; otherwise `false`, with a reason in `SignatureError()`. Sets `IsSignatureValid()` accordingly.
- **Pitfalls.** Fails with a recorded error if the document was not parsed, has no `x5c` certificates, or names an unsupported algorithm. Proves only that the leaf key signed the bytes — *not* that the leaf is trusted.

### `bool VerifyChain ()`
- **Purpose.** Verify the certificate chain — the **provenance** check. Delegates to the nested [`CHAIN`](CHAIN.md), which validates the `x5c` entries against the trust store.
- **Returns.** `true` if the chain reaches a trusted root and is current; otherwise `false`, with a reason in `ChainError()`. Sets `IsChainTrusted()`; also sets `IsChainExpired()` when the failure reason mentions expiry.
- **Pitfalls.** Fails (recorded) if not parsed or no certificates. Independent of `VerifySignature` — a document can pass one and fail the other.

---

## Trust store

```cpp
void AddTrustedCert (const std::string& sPem);
```

### `void AddTrustedCert (const std::string& sPem)`
- **Purpose.** Add a certificate authority the chain validator should trust as a root, on top of the platform's system root store. Forwards to the nested `CHAIN`.
- **Parameters.** `sPem` — a CA certificate in PEM form.
- **Notes.** Call before `VerifyChain`. Used to trust a development or organization-specific CA. Silently ignores PEM that fails to parse.

---

## Certificate chain (for signing)

These manage the PEM certificate chain that `Sign` embeds into `x5c`. They are distinct from the trust store and from the certificate info parsed out of a document.

```cpp
void                     AddCert    (const std::string& sPem);
bool                     RemoveCert (int nIndex);
const std::vector<CERT>& CertInfos  () const;
int                      CertCount  () const;
```

### `void AddCert (const std::string& sPem)`
- **Purpose.** Append a certificate (PEM) to the chain used for signing, and record its decoded `CERT` info. The first certificate added is treated as the leaf; subsequent ones as CAs.
- **Parameters.** `sPem` — the certificate in PEM.

### `bool RemoveCert (int nIndex)`
- **Purpose.** Remove the certificate (and its info record) at `nIndex` from the chain.
- **Returns.** `true` if removed; `false` if `nIndex` is out of range.

### `const std::vector<CERT>& CertInfos () const`
- **Purpose / Returns.** The decoded certificate info records. After `Parse` these describe the parsed `x5c` chain; while composing they describe the certificates added with `AddCert`.
- **Pitfalls.** Returned **by reference** into the MSF — do not retain past the MSF's lifetime.

### `int CertCount () const`
- **Purpose / Returns.** The number of `CERT` info records currently held.

---

## Payload — bulk

```cpp
void           SetPayload (const nlohmann::json& payload);
nlohmann::json Payload    () const;
```

### `void SetPayload (const nlohmann::json& payload)`
- **Purpose.** Replace the entire payload with a JSON value. Used when composing a document from a pre-built JSON object.

### `nlohmann::json Payload () const`
- **Purpose / Returns.** The full payload as JSON (a copy). The typed accessors below read individual fields out of this.

---

## Payload — typed fields

```cpp
void        SetContainer (const std::string& sContainer);
std::string Container    () const;
void        SetSuccessor (const std::string& sSuccessor);
std::string Successor    () const;
```

### `void SetContainer (const std::string& sContainer)` / `std::string Container () const`
- **Purpose.** Set or read the payload's `container` field — the logical name of the runtime [container](../container/index.md) this fabric runs under.
- **Returns (getter).** The container name, or an empty string if absent.

### `void SetSuccessor (const std::string& sSuccessor)` / `std::string Successor () const`
- **Purpose.** Set or read the payload's `successor` field — a reference to a newer version of the fabric.
- **Returns (getter).** The successor reference, or an empty string if absent.

---

## Services

```cpp
void                 AddService    (const SERVICE& service);
bool                 RemoveService (const std::string& sName);
std::vector<SERVICE> Services      () const;
```

### `void AddService (const SERVICE& service)`
- **Purpose.** Append a service entry to the payload's `services` array.
- **Parameters.** `service` — an `MSF::SERVICE` (see [nested structs](#nested-structs)).

### `bool RemoveService (const std::string& sName)`
- **Purpose.** Remove the first service whose `name` equals `sName`.
- **Returns.** `true` if one was removed; `false` otherwise.

### `std::vector<SERVICE> Services () const`
- **Purpose / Returns.** All services in the payload, decoded into `SERVICE` records (a copy).

---

## Modules

```cpp
void                AddModule    (const std::string& sUrl, const std::string& sHash);
bool                RemoveModule (const std::string& sUrl);
std::vector<MODULE> Modules      () const;
```

### `void AddModule (const std::string& sUrl, const std::string& sHash)`
- **Purpose.** Append a module entry `{ url, hash }` to the payload's `modules` array.
- **Parameters.** `sUrl` — the module's download URL; `sHash` — its expected SHA-256 hex digest.

### `bool RemoveModule (const std::string& sUrl)`
- **Purpose.** Remove the first module whose `url` equals `sUrl`.
- **Returns.** `true` if one was removed; `false` otherwise.

### `std::vector<MODULE> Modules () const`
- **Purpose / Returns.** All modules in the payload, decoded into `MODULE` records (a copy). This is the list a [`FABRIC`](../scene/FABRIC.md) fetches and instantiates.

---

## Status and identity accessors

```cpp
bool        IsSignatureValid    () const;
bool        IsChainTrusted      () const;
bool        IsChainExpired      () const;
std::string Algorithm           () const;
std::string Fingerprint         () const;
std::string Organization        () const;
std::string OrganizationHash    () const;
std::string DisplayOrganization () const;
std::string SignatureError      () const;
std::string ChainError          () const;
```

| Accessor | Returns |
|---|---|
| `IsSignatureValid()` | Whether the last `VerifySignature` succeeded. |
| `IsChainTrusted()` | Whether the last `VerifyChain` reached a trusted root. |
| `IsChainExpired()` | Whether the chain failure was due to an expired certificate. |
| `Algorithm()` | The JWS signing algorithm read from the header (e.g. `"RS256"`); empty for unsigned. |
| `Fingerprint()` | The leaf certificate's SPKI SHA-256 (full hex). For unsigned documents, a synthetic hash of URL+content. Stable across certificate renewal that keeps the key. |
| `Organization()` | The `O` field of the leaf certificate's subject. |
| `OrganizationHash()` | The SHA-256 (full 64-hex) of the leaf subject string, used as a non-impersonable display stand-in. |
| `DisplayOrganization()` | The real organization name **only if** the chain is trusted or expired; otherwise the organization hash. The safe-to-show identity label. |
| `SignatureError()` | Human-readable reason the last signature verification failed (empty on success). |
| `ChainError()` | Human-readable reason the last chain verification failed (empty on success). |

---

## Nested structs

### `MSF::SERVICE`
An external service the fabric connects to.

| Field | Type | Meaning |
|---|---|---|
| `sName` | `std::string` | Service name. |
| `sType` | `std::string` | Protocol/type (e.g. `"websocket"`). |
| `sEndpoint` | `std::string` | Endpoint URL. |
| `aModules` | `std::vector<std::string>` | Names of modules this service uses. |

### `MSF::MODULE`
A WebAssembly module the fabric loads.

| Field | Type | Meaning |
|---|---|---|
| `sUrl` | `std::string` | Download URL. |
| `sHash` | `std::string` | Expected SHA-256 hex digest. |

### `MSF::CERT`
Decoded metadata for one certificate (produced by `Parse`, `AddCert`, and the [`CHAIN`](CHAIN.md) decode utilities).

| Field | Type | Meaning |
|---|---|---|
| `sSubject` | `std::string` | Subject distinguished name (one line). |
| `sIssuer` | `std::string` | Issuer distinguished name (one line). |
| `sOrganization` | `std::string` | The subject's `O` field. |
| `sSerial` | `std::string` | Serial number in hex. |
| `sNotBefore` | `std::string` | Start of the validity window. |
| `sNotAfter` | `std::string` | End of the validity window. |
| `sKeyType` | `std::string` | `"RSA"`, `"EC"`, or `"unknown"`. |
| `nKeyBits` | `int` | Key size in bits. |
| `bIsCA` | `bool` | `true` for an intermediate/root, `false` for the leaf. |

---

## See also

- [MSF system](../../systems/msf.md) — design, parse-then-verify model, trust levels.
- [CHAIN](CHAIN.md) — the X.509 validator `VerifyChain` delegates to.
- [Container API](../container/index.md) — the identity an MSF's verification produces.
- [Scene API](../scene/FABRIC.md) — `FABRIC` owns the MSF and loads its modules.

---

[MSF API](index.md) · Prev: [index](index.md) · Next: [CHAIN](CHAIN.md)
