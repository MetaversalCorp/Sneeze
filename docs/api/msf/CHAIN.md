---
title: MSF::CHAIN (class reference)
tier: API
audience: [integrator, contributor]
sources:
  - include/Msf.h
  - src/context/msf/Chain.cpp
verified: b487fd1
nav:
  prev: api/msf/MSF.md
  next: api/persona/index.md
---

# `MSF::CHAIN`

The X.509 certificate-chain validator nested inside [`MSF`](MSF.md). It wraps BoringSSL's `X509_STORE` verification machinery and answers one question: *does this chain of certificates lead back to a certificate authority the engine trusts, and is it currently valid?* It also exposes a handful of static certificate utilities used across the MSF system (and by signing tooling). For why chain validation is a separate step from signature verification, see the [MSF system](../../systems/msf.md) page; this page is the exact behavior of every public member.

```cpp
class MSF::CHAIN
{
public:
   CHAIN ();
   ~CHAIN ();
   CHAIN (const CHAIN&) = delete;            // non-copyable, non-movable
   // ... see sections below
private:
   struct IMPL;
   IMPL*             m_pImpl;                 // owns X509_STORE + validated leaf
   std::vector<CERT> m_aCertInfos;
};
```

---

## Role and ownership

- **Owned by** an [`MSF`](MSF.md) as a member (`m_certChain`); `MSF::VerifyChain` and `MSF::AddTrustedCert` delegate to it. It can also be used standalone.
- **Owns** a BoringSSL `X509_STORE` (the trust store) and, after a successful validation, a duplicated copy of the validated leaf certificate (so its fingerprint can be read back). Both are freed in the destructor.
- **Non-copyable and non-movable.** It holds raw BoringSSL handles; pass by reference.
- **The static utilities are stateless** — they construct and free their own BoringSSL objects per call and need no `CHAIN` instance.

---

## Lifecycle

A `CHAIN` is constructed empty. Its trust store is **lazily loaded** on first use — the first call to `Validate` or `AddTrustedCert` populates it from the platform's system root certificate store (on Windows, the `ROOT` system store; elsewhere, OpenSSL's default certificate paths). Extra roots added via `AddTrustedCert` join the same store. After a successful `Validate`, the validated leaf is remembered until the next successful validation or destruction.

---

## Threading and pitfalls

**Not internally synchronized.** A `CHAIN` holds no locks. It belongs to one `MSF`, which is itself single-load-at-a-time; do not validate concurrently on one instance.

**`Validate` resets the info list and the remembered leaf only on success.** Each call clears `m_aCertInfos` and rebuilds it from the supplied chain. The remembered leaf (`GetLeafFingerprint`'s source) is replaced **only when validation succeeds** — after a failed validation it still reflects the last *successful* one, or is empty if there has never been one.

**Trust-store load is one-time.** The lazy load happens once; certificates added with `AddTrustedCert` after the first load are added to the existing store. There is no reload.

**Static utilities fail soft.** The decode/convert helpers return empty strings or a default-constructed `CERT` on malformed input rather than throwing. Check for empty results.

---

## Construction and destruction

```cpp
CHAIN ();
~CHAIN ();
```

### `CHAIN()`
- **Purpose.** Construct an empty validator. The trust store is not built until first use.

### `~CHAIN()`
- **Purpose.** Free the trust store and any remembered validated leaf.

---

## Validation

```cpp
bool        Validate           (const std::vector<std::string>& aX5cEntries, std::string& sError);
std::string GetLeafFingerprint () const;
const std::vector<CERT>& CertInfos () const;
void        AddTrustedCert     (const std::string& sPem);
```

### `bool Validate (const std::vector<std::string>& aX5cEntries, std::string& sError)`
- **Purpose.** Validate a certificate chain against the trust store. Decodes each base64-DER entry, records a `CERT` info for each (entry 0 = leaf, the rest = CAs), treats entries 1..n as untrusted intermediates, and runs `X509_verify_cert` with the leaf against the store.
- **Parameters.**
- `aX5cEntries` — the chain, leaf first, each a base64-encoded DER certificate (the JWS `x5c` format).
- `sError` — **out**; set to a human-readable reason on failure (empty chain, undecodable entry, or the BoringSSL verification error string, which includes expiry).
- **Returns.** `true` if the chain validates; `false` otherwise.
- **Notes.** On success, remembers a copy of the leaf for `GetLeafFingerprint`. Loads the trust store on first call. Rebuilds `CertInfos()` every call.

### `std::string GetLeafFingerprint () const`
- **Purpose / Returns.** The SHA-256 of the **validated** leaf certificate's SPKI (public key), as hex — or an empty string if no validation has succeeded. Because it hashes the public key, not the whole certificate, it is stable across certificate renewal that keeps the same key pair.

### `const std::vector<CERT>& CertInfos () const`
- **Purpose / Returns.** The decoded info records for the chain from the most recent `Validate` call. Returned **by reference** — do not retain past the `CHAIN`'s lifetime or across another `Validate`.

### `void AddTrustedCert (const std::string& sPem)`
- **Purpose.** Add a CA certificate (PEM) as a trusted root, on top of the system store.
- **Parameters.** `sPem` — the CA certificate in PEM.
- **Notes.** Loads the trust store first if needed. Silently ignores PEM that fails to parse.

---

## Static certificate utilities

These need no `CHAIN` instance. They are the shared primitives the MSF system (and signing tooling) use to decode, convert, and fingerprint certificates.

```cpp
static CERT        DecodeInfoDerBase64 (const std::string& sB64, bool bIsCA);
static CERT        DecodeInfoPem       (const std::string& sPem, bool bIsCA);
static std::string ComputeFingerprint  (const std::string& sB64Der);
static std::string ExtractPublicKeyPem (const std::string& sB64Der);
static std::string PemToDerBase64       (const std::string& sPem);
static std::string HashString           (const std::string& sInput);
```

### `static CERT DecodeInfoDerBase64 (const std::string& sB64, bool bIsCA)`
- **Purpose.** Decode a base64-DER certificate into a `CERT` info record.
- **Parameters.** `sB64` — base64-encoded DER; `bIsCA` — the flag to record in `bIsCA`.
- **Returns.** The populated `CERT`, or a default-constructed one if decoding fails.

### `static CERT DecodeInfoPem (const std::string& sPem, bool bIsCA)`
- **Purpose.** As above, but from a PEM certificate.
- **Returns.** The populated `CERT`, or a default-constructed one on failure.

### `static std::string ComputeFingerprint (const std::string& sB64Der)`
- **Purpose.** Compute a certificate's identity fingerprint: the SHA-256 of its SPKI (public key), as hex.
- **Parameters.** `sB64Der` — base64-encoded DER certificate.
- **Returns.** The hex fingerprint, or empty on failure. This is the value `MSF::Fingerprint` reports for the leaf.

### `static std::string ExtractPublicKeyPem (const std::string& sB64Der)`
- **Purpose.** Extract a certificate's public key as a PEM string (used by `MSF::VerifySignature` to get the leaf's verification key).
- **Parameters.** `sB64Der` — base64-encoded DER certificate.
- **Returns.** The public key in PEM, or empty on failure.

### `static std::string PemToDerBase64 (const std::string& sPem)`
- **Purpose.** Convert a PEM certificate to base64-DER (used by `MSF::Sign` to build the `x5c` header array).
- **Parameters.** `sPem` — the certificate in PEM.
- **Returns.** The base64-DER string, or empty on failure.

### `static std::string HashString (const std::string& sInput)`
- **Purpose.** A general-purpose hash: the full SHA-256 of `sInput` as lowercase hex. Used for the organization hash and the synthetic fingerprint of unsigned documents.
- **Parameters.** `sInput` — the string to hash.
- **Returns.** The 64-character (32-byte) hex digest, or empty if `sInput` is empty.

---

## See also

- [MSF](MSF.md) — the owning class; `VerifyChain` delegates here.
- [MSF system](../../systems/msf.md) — design, trust levels, current limitations.
- [Container API](../container/index.md) — where the validated identity is consumed.

---

[MSF API](index.md) · Prev: [MSF](MSF.md) · Next: [Persona API](../persona/index.md)
