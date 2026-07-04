---
title: The MSF File and Signing
tier: Guides
audience: [author]
sources:
  - include/Msf.h
  - src/context/msf/MsfFile.cpp
  - src/context/msf/Chain.cpp
  - src/context/Context.cpp
  - tools/SignMsf/main.cpp
verified: b3d15ea
nav:
  prev: guides/authoring-first-fabric.md
  next: guides/authoring-static-scenes.md
---

# The MSF File and Signing

This page is the full reference for the fabric file itself: the JSON payload that describes your space, the optional cryptographic wrapper that proves who authored it, the `SignMsf` tool that produces and inspects fabrics, and the trust model the engine applies when it loads one. If the [quickstart](authoring-first-fabric.md) showed you the happy path, this page explains every field and every option behind it.

An MSF — Metaverse Spatial Fabric — comes in two forms that carry the *same author-facing content*:

- **Plain JSON.** The file is literally the payload object. The engine loads it, but there is no signature, so it carries a synthetic, low-trust identity. Ideal for development.
- **Signed JWS.** The payload is wrapped in a JSON Web Signature that also embeds the author's certificate chain. The engine can verify it and read the publisher's identity. This is the intended form for anything published.

We cover the payload first (which you always write), then the signature (which wraps it).

---

## The payload schema

The payload is a single JSON object. These are all the fields the engine looks at:

```json
{
   "container": "example",
   "services": [],
   "modules":
   [
      { "url": "https://cdn.example/module.wasm", "hash": "sha256-<hex>" }
   ],
   "primary":
   {
      "camera": { "position": [-8, 0, 2], "rotation": [0, 0, 0, 1] },
      "background": "202830"
   },
   "data": { "Head": { "Self": "R-0" }, "Name": "Root" },
   "successor": "https://cdn.example/example-v2.msf"
}
```

### `container` (string)

The fabric's identity name. It is not decoration: combined with the signer's certificate and the signed-in user, it scopes the fabric's persistent storage and its sandbox. Two fabrics that share a `container` name share that scope, so pick a stable, unique name per space.

### `modules` (array)

The WASM code the fabric runs. Each entry is an object:

| Key | Type | Meaning |
|---|---|---|
| `url` | string | HTTPS address the engine fetches the module from. |
| `hash` | string | Subresource-integrity check, form `sha256-<hex>` (also `sha384-`/`sha512-`). Empty string = no check. |

Two things trip people up here. First, the integrity key is **`hash`** — some example files in the repo write `comment-hash`, which the engine **ignores** (so the module loads unchecked). Second, the digest is written as **hex**, not base64: prefix the algorithm name, a hyphen, then the lowercase hex digest, e.g. `sha256-c3fadcd3914bd2ecf386ef0661e0b4385e4d4d80e7c7f96ca49eef56b1fb36d0`.

For a **map-managed** fabric this list contains the generic `map.wasm`. For a **WASM-managed** fabric it contains your own module(s). A fabric with an empty `modules` list and a `data.scene` tree shows nothing, because it is the map module that injects that tree.

### `data` (object)

A general-purpose block the fabric carries for its modules to read. The **map-managed** path uses it for the scene: the generic `map.wasm` reads a node tree from `data.scene` (the rest of `data` is free for other use). This path has its own page: [Static scenes: the data tree](authoring-static-scenes.md). A WASM-managed fabric typically omits `data` and builds its scene from code.

### `primary` (object)

The starting camera and background, applied **only to the top-level fabric**:

- `camera.position` — eye position in metres, `[x, y, z]`.
- `camera.rotation` — orientation quaternion, `[x, y, z, w]`.
- `background` — six-hex-digit `RRGGBB` backdrop colour.

A fabric embedded as a child of another fabric does not get a camera or a sky; only the fabric the browser is pointed at does.

### `services` (array) and `successor` (string) — reserved

`services` is meant to declare external services the fabric talks to, and `successor` is meant to point at a newer version. Both are **parsed but not acted on** today: `services` has no runtime effect, and while `successor` is read (the `SignMsf --verify` dump prints it) nothing upgrades the fabric. Include them if you like for forward-compatibility, but expect no behaviour from them yet.

---

## The signature: how a signed MSF is built

A signed `.msf` is a **JWS in compact form** — three base64url segments joined by dots:

```
<header> . <payload> . <signature>
```

- **Header** declares the signature algorithm (`alg`, e.g. `RS256`) and carries an **`x5c`** claim: the author's X.509 certificate chain as an array of base64 DER certificates, leaf certificate first. This is how the file says "here is who I am" — the identity travels inside the file.
- **Payload** is a small JSON object with a single claim, **`data`**, whose value is your payload object serialized as a string. When the engine (or `SignMsf --verify`) decodes the file, it reads that `data` claim back and parses it into the payload you wrote.
- **Signature** is the cryptographic signature over the header and payload, produced with your private key.

At verification time the engine extracts the public key from the **leaf** certificate (`x5c[0]`), checks the signature with it, and then walks the embedded chain against its trust anchors. Supported algorithms are `RS256`, `RS384`, `RS512`, `ES256`, `ES384`, and `ES512`; the default is `RS256`.

You never assemble any of this by hand — `SignMsf` does it.

---

## What you need to sign

To sign, you need a **private key**, a **leaf certificate** for that key, and (usually) one or more **chain certificates** up to a trusted authority:

- **Private key** (`--key`, PEM) — signs the file. Keep it secret.
- **Leaf certificate** (`--cert`, PEM) — your public certificate; embedded in the file so verifiers get your identity and public key.
- **Chain certificate(s)** (`--chain`, PEM) — intermediate and/or root certificates that connect your leaf to a recognized authority. Pass `--chain` multiple times to add several, in order from nearest-to-leaf upward.

For local development the repository ships a ready-made set under `tests/certs/`:

| File | Role |
|---|---|
| `provider-key.pem` | The private key you sign with. |
| `provider-cert.pem` | The leaf (provider) certificate. |
| `ca-cert.pem` | The test certificate authority — use as both a `--chain` when signing and a `--trust` anchor when verifying. |

These are test credentials only. A real publisher uses their own key and a certificate that chains to an authority the engine's trust store recognizes.

---

## `SignMsf` — the complete tool

`SignMsf` has two modes: **sign** and **verify**. It is built with the engine; look for it in the install tree, for example `builds\windows-x64\install\release\bin\SignMsf.exe`.

### Sign

```
SignMsf --payload <json> --key <key.pem> --cert <cert.pem>
        [--chain <intermediate.pem>] [--alg RS256] --out <file.msf>
```

| Flag | Required | Meaning |
|---|---|---|
| `--payload <json>` | yes | The JSON payload file; its contents become the signed `data`. |
| `--key <key.pem>` | yes | Private key used to sign. |
| `--cert <cert.pem>` | yes | Leaf certificate, embedded in the file. May be repeated. |
| `--chain <pem>` | no | Additional chain certificate, embedded after the leaf. May be repeated. |
| `--alg <name>` | no | Signature algorithm; default `RS256`. One of `RS256/384/512`, `ES256/384/512`. |
| `--out <file.msf>` | yes | Output path for the signed fabric. |

A worked example using the bundled test credentials:

```powershell
SignMsf --payload space.json ^
        --key   tests\certs\provider-key.pem ^
        --cert  tests\certs\provider-cert.pem ^
        --chain tests\certs\ca-cert.pem ^
        --out   space.msf
```

On success it prints `Signed space.msf (<n> bytes)`.

### Verify and inspect

```
SignMsf --verify <file.msf> [--trust <ca.pem>]
```

`--verify` decodes a fabric and reports on it; `--trust` supplies a trust anchor to check the chain against (repeatable). Verifying the file we just signed:

```powershell
SignMsf --verify space.msf --trust tests\certs\ca-cert.pem
```

The output includes:

- **File / Algorithm / Fingerprint** — the file, its `alg`, and a fingerprint of the signing certificate.
- **Successor** — the `successor` URL, if the payload declared one.
- **Signature: VERIFIED** or **FAILED** — whether the signature checks out *and* the chain is trusted against the anchors you passed. On failure it prints a signature error and/or a chain error.
- **Certificate Chain** — each embedded certificate: subject, issuer, serial, validity dates, key type and size, and whether it is a CA.
- **Payload** — the decoded JSON payload, pretty-printed, so you can confirm exactly what got signed.

Use `--verify` as your pre-publish sanity check: it proves the signature is good, the chain is what you expect, and the payload is byte-for-byte what you intended.

---

## Inspecting a fabric without the CLI: MsfViewer

`tools/MsfViewer/` is a standalone HTML page for inspecting a `.msf` in an ordinary web browser — no build, no install. Open it and load a fabric to decode the JWS locally and read its header (algorithm and certificate chain) and its payload. It is handy for a quick look when you do not have a terminal in front of you, or for sharing a fabric's contents with someone who does not have the toolchain. For authoritative signature-and-chain verification, `SignMsf --verify` is the source of truth; MsfViewer is a convenience inspector.

---

## The trust model (and its current caveat)

When the engine loads a signed fabric it computes a **trust level** from the signature and chain:

| Trust level | Meaning |
|---|---|
| `UNTRUSTED` | The signature did not verify. |
| `UNVERIFIED` | The signature verified, but the chain does not reach a trusted anchor. |
| `EXPIRED` | The chain reaches a trusted anchor, but a certificate has expired. |
| `VERIFIED` | Signature good and chain trusted and current. |
| `ROOT` | The synthetic identity used for the browser's own home context, not a loaded fabric. |

The trust anchors are the operating system's certificate store, consulted through the engine's chain verifier.

**The caveat you must know:** in the current code the container's trust level is unconditionally overwritten to `EXPIRED` after being computed, and the engine loads untrusted and plain-JSON fabrics regardless of trust. In other words, **trust is computed and reported but not yet enforced.** Sign your published fabrics — the embedded identity is real, verifiable, and future-proof — but do not design anything today that depends on the engine refusing a low-trust fabric, because it will not refuse it yet.

Plain-JSON (unsigned) fabrics load with a synthetic identity and no verified authorship. That is exactly what you want during development; it is not what you want for a fabric you publish.

---

## See also

- [Your first fabric](authoring-first-fabric.md) — signing in the context of a full end-to-end build.
- [Static scenes: the data tree](authoring-static-scenes.md) — the schema of the `data` field this page referenced.
- [MSF system](../systems/msf.md) and [MSF API](../api/msf/index.md) — the engine internals of parsing, verification, and the certificate chain.
- [Container system](../systems/container.md) — how `container` and trust scope a fabric's storage and sandbox.

---

[Home](../Home.md) · Prev: [Your first fabric](authoring-first-fabric.md) · Next: [Static scenes: the data tree](authoring-static-scenes.md)
