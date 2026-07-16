# CONTAINER

The container identity (the engine's `CID`) and its trust standing. Reached through [`FABRIC::Container`](FABRIC.md#snapshot-views), a read-only view over the [Open snapshot](SNAPSHOT.md).

A container is identified by persona + organization + container name. Friendly display names are composed guest-side from these raw fields, not transported.

## Methods

| Method | Returns | Meaning |
|--------|---------|---------|
| `Name ()` | `&str` | The container name. |
| `Organization ()` | `&str` | The organization's friendly name (e.g. "Metaversal Corporation"), not a domain. |
| `OrganizationHash ()` | `&str` | The organization hash. |
| `Persona ()` | `&str` | The persona name. |
| `PersonaHash ()` | `&str` | The persona hash. |
| `Fingerprint ()` | `&str` | The signer/leaf certificate fingerprint. |
| `Trust ()` | `i32` | The trust level (see below). |
| `DisplayName ()` | `String` | Friendly container name, composed guest-side (like `CID::DisplayName`): the organization - its friendly name at/above the expired-trust threshold, else its hash - joined to the container name. |
| `DisplayOrganization ()` | `String` | Friendly organization name, composed guest-side (like `MSF::DisplayOrganization`): the friendly name once the chain is trusted-or-expired, else the hash. |

## Trust level

`Trust ()` returns the `eSNEEZE_ABI_TRUST` integer. Compare it against the `kSNEEZE_ABI_TRUST_*` constants (in the `abi` module), ordered least-to-most trusted:

| Constant | Value | Meaning |
|----------|-------|---------|
| `kSNEEZE_ABI_TRUST_NONE` | 0 | no trust information |
| `kSNEEZE_ABI_TRUST_UNTRUSTED` | 1 | explicitly untrusted |
| `kSNEEZE_ABI_TRUST_UNVERIFIED` | 2 | not verified |
| `kSNEEZE_ABI_TRUST_EXPIRED` | 3 | signature/chain expired |
| `kSNEEZE_ABI_TRUST_VERIFIED` | 4 | verified |
| `kSNEEZE_ABI_TRUST_ROOT` | 5 | root of trust |

It is transported as an integer precisely so a module can branch on it without parsing a string.

## Usage

```rust
use sneeze::abi::*;

fn Open (pFabric: FABRIC)
{
   let pContainer = pFabric.Container ();
   let pConsole   = pFabric.Console ();

   pConsole.Log (pContainer.Name ());
   pConsole.Log (&pContainer.DisplayOrganization ());

   if pContainer.Trust () >= kSNEEZE_ABI_TRUST_VERIFIED
   {
      pConsole.Info ("fabric is verified");
   }
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - how to obtain the view.
- [SIGNATURE](SIGNATURE.md) - the verification detail behind the trust level.
