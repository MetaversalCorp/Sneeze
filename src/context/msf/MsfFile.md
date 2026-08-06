# MSF — Metaverse Spatial Fabric File

The `msf` module provides signing and verification of `.msf` files using
RFC 7515 JWS compact serialization, plus plain JSON (unsigned) MSF support.

`MSF` is the single class for the full lifecycle: parsing, signing, verification,
certificate management, and typed payload access. `MSF::CHAIN` handles X.509
chain validation. Both are in the `SNEEZE` namespace.

Public header: `include/Msf.h`.

## Reading and Verifying

```cpp
#include <Msf.h>

SNEEZE::MSF msf;
msf.TrustedCert_Add (sCaPem);

// Parse — always populates all fields, even if invalid
msf.Parse (sJwsOrJson, sUrl);

// Verify (optional — parsed data is available either way)
msf.Signature_Verify ();
msf.Chain_Verify ();

// Read results
msf.Algorithm ();            // "RS256"
msf.Fingerprint ();          // SHA-256 of leaf cert's SPKI
msf.Organization ();         // from cert subject
msf.OrganizationHash ();     // truncated SHA-256 of organization
msf.IsSignatureValid ();
msf.IsChainTrusted ();
msf.IsChainExpired ();

// Inspect payload
msf.Container ();            // container name
msf.Service_Names ();         // vector<string> (the declared service names)
msf.Service_Has ("Map");      // bool
msf.Service ("Map");         // nlohmann::json (the named service's whole object)
msf.Modules ();              // vector<MODULE>
msf.Payload ();              // const nlohmann::json& — the raw payload, by reference (no copy)
msf.Successor ();            // successor fingerprint
```

### Parse accepts both JWS and plain JSON

`Parse(sInput, sUrl)` detects the format:
- **JWS compact serialization** (signed) — splits header.payload.signature,
  decodes certificates, extracts fingerprint
- **Plain JSON** (unsigned) — parses directly, generates a synthetic fingerprint
  from SHA-256 of the URL + content (100% untrustworthy, unique per file)

The `sUrl` parameter is always required.

## Composing and Signing

```cpp
SNEEZE::MSF msf;
msf.Container ("poker-table");
msf.Service_Add ("Map", nlohmann::json {{"sNamespace", "com.example.game"}, {"sService", "websocket"}, {"sConnect", "wss://rt.example.com/game"}});
msf.Module_Add ("https://cdn.example.com/game.wasm", "sha256-a1b2c3...");
msf.Cert_Add (sLeafPem);
msf.Cert_Add (sCaPem);

std::string sJws = msf.Sign (sPrivateKeyPem, "RS256");
```

## Services

`Services` is a **name-keyed JSON object** in the payload (not an array): each key is a service name and its value is a free-form object carrying whatever fields the fabric author chose (a map service, for example, uses `sNamespace`, `sService`, `sConnect`, `bAuth`, `sRootUrl`, `wClass`, `twObjectIx`). The engine does not impose a fixed shape, so the API hands back whole service objects rather than a fixed struct:

| Method | Returns | Meaning |
|--------|---------|---------|
| `Service_Add (sName, service)` | `void` | Add/replace the named service (value is any `nlohmann::json` object). |
| `Service_Remove (sName)` | `bool` | Remove the named service; `true` if it existed. |
| `Service_Has (sName)` | `bool` | Whether a service is declared under that name. |
| `Service (sName)` | `nlohmann::json` | The named service's whole object (null if absent). |
| `Service_Names ()` | `vector<string>` | The declared service names. |

Guest WASM modules read the same objects on demand, by name, through the SDK's `SERVICES` subsystem (`pFabric.Services ().Get ("Map")`), which returns the service's whole JSON as text - the host serves it from this payload block.

## Nested Types

### MSF::MODULE

| Field | Type | Description |
|-------|------|-------------|
| `sUrl` | `string` | Download URL |
| `sHash` | `string` | SHA-256 hex digest |

### MSF::CERT

| Field | Type | Description |
|-------|------|-------------|
| `sSubject` | `string` | Certificate subject |
| `sIssuer` | `string` | Certificate issuer |
| `sOrganization` | `string` | Organization from subject |
| `sSerial` | `string` | Serial number (hex) |
| `sNotBefore/After` | `string` | Validity dates |
| `sKeyType` | `string` | "RSA", "EC", or "unknown" |
| `nKeyBits` | `int` | Key size in bits |
| `bIsCA` | `bool` | CA or leaf |

### MSF::CHAIN

X.509 chain validation via BoringSSL's `X509_STORE`. Loads the OS root
certificate store automatically. Computes the leaf certificate's SPKI
fingerprint (SHA-256).

Static utilities (no MSF instance needed):
- `Cert_DecodeDerBase64()` / `Cert_DecodePem()` — parse cert metadata
- `Fingerprint_Compute()` — SHA-256 of SPKI from base64 DER
- `PublicKey_Extract()` — extract public key as PEM
- `Pem_ToDerBase64()` — convert PEM to base64 DER
- `String_Hash()` — SHA-256 of arbitrary string

## Supported Algorithms

RS256, RS384, RS512, ES256, ES384, ES512.

## Files

| File | Contents |
|------|----------|
| `include/Msf.h` | MSF class, MODULE, CERT, CHAIN |
| `MsfFile.cpp` | MSF implementation (parse, sign, verify, payload access) |
| `Chain.cpp` | CHAIN implementation (X509_STORE, fingerprint, cert utilities) |
