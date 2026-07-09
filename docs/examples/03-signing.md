---
title: "Example 03 - Publishing a Signed Fabric"
tier: Examples
audience: [author]
sources:
  - examples/03-signing/signed-stool-and-bucket.json
  - examples/03-signing/README.md
verified: c9029f4
nav:
  prev: examples/02-stool-and-bucket.md
  next: examples/building-signmsf.md
---

# Example 03 - Publishing a Signed Fabric

This example builds on [Example 02](02-stool-and-bucket.md), but it does not change the scene at all. The stool, the bucket, and the lighting -- the `primary` block's ambient and directional light plus the two spot lights -- are exactly the same. What changes is how the fabric is prepared for the world to load: we pin its module to an exact version with a hash, and then we sign the whole fabric into a `.msf` file. This is the step Examples 01 and 02 kept pointing forward to when they said "this is not the preferred way to publish." This is the preferred way. The complete source lives in the repository at [`examples/03-signing/`](../../examples/03-signing/) -- copy it and follow along.

## ⚠️ The following warning paragraph is different from the ones in Examples 01 & 02
## ⚠️ Fabric security measures are not fully utilized... yet

At present, the browser computes and reports a fabric's trust level, but it does not yet refuse a fabric for being unsigned or untrusted -- it loads them all today. So signing right now is about attaching a real, verifiable identity to your work and being ready for when enforcement arrives; it is not yet a gate that keeps unsigned fabrics out. Sign your published fabrics anyway -- the identity inside them is genuine and future-proof.

## Why sign a fabric?

A plain JSON fabric works while you are learning, but it makes two promises it cannot keep. It cannot prove who wrote it, and it cannot prove that what the browser downloaded is what the author actually published. Anyone could alter the file in transit, or serve a different file entirely, and the browser would have no way to tell. Signing fixes both. A signed fabric carries the author's certificate inside it, so the browser learns who published it, and it carries a cryptographic signature over the exact bytes of the payload, so the browser can tell if a single character was changed. The signed file is the real, publishable form of a fabric.

## What this example teaches

- How to pin a module to an exact version with a hash, so the browser refuses a module that does not match.
- Where the ready-made signing credentials live for local testing, and how you get your own for real publishing.
- How to sign a fabric into a `.msf` file using the metaverse browser you already have, and how to verify the result before you publish it.

## What is new since Example 02

The scene is identical to Example 02, so nothing in the `data` tree or the `primary` lighting block is new. The two new things are a `hash` on the module, and the signing step that turns the plain `signed-stool-and-bucket.json` into the signed `signed-stool-and-bucket.msf`. Everything about the scene itself -- `container`, the `primary` lighting, the `Children` tree, `Transform`, and the spot lights -- works exactly as [Example 02](02-stool-and-bucket.md) explained.

## The files

| File | What it is |
|---|---|
| `signed-stool-and-bucket.json` | The payload: the plain fabric you write and then sign. Same scene as Example 02, plus a module hash. |
| `signed-stool-and-bucket.msf` | The signed fabric: the payload wrapped with a signature and the author's certificate. This is the file you actually publish. |
| `wasm/map.wasm` | The stock module, shared with the other examples. |
| `assets/Stool.glb`, `assets/Bucket.glb` | The models, reused from Examples 01 and 02. |

## Step 1 - Pin the module with a hash

A fabric names its module by address, not by content, so the module is downloaded separately when the fabric loads. A **hash** lets you nail that download to one exact version of the file. You compute a fingerprint of the module's bytes, write that fingerprint into the fabric, and from then on the browser will only accept a module whose bytes produce the same fingerprint. If the file at that address is ever swapped or altered, even by one byte, its fingerprint no longer matches and the browser rejects it. Pinning the version you signed is part of what makes a signed fabric trustworthy end to end.

The hash goes on the module entry, beside its `url`:

```json
"modules":
[
   {
      "url": "wasm/map.wasm",
      "hash": "sha256-96d4c0b6f98adb9819e1f242aa6664d7bd825025dc0159b1ca4420ea08e6c857"
   }
]
```

The value has a strict shape: the algorithm name, a hyphen, then the digest written as lowercase hexadecimal. The engine accepts `sha256`, `sha384`, and `sha512`; `sha256` is the usual choice. The hex must be lowercase -- the engine compares it exactly, so an uppercase digest will fail to match. Leaving the `hash` out, or setting it to an empty string, simply means the module loads without an integrity check.

You do not type the digest by hand; you compute it from the exact file you are going to publish. On Windows, in PowerShell, `Get-FileHash` gives you the digest but in uppercase, so lower it and add the prefix:

```powershell
"sha256-" + (Get-FileHash -Algorithm SHA256 map.wasm).Hash.ToLower()
```

On Linux or macOS, `sha256sum` already prints lowercase, so:

```bash
echo "sha256-$(sha256sum map.wasm | cut -d' ' -f1)"
```

Either command prints the exact string to paste into the fabric. The hash in this example was computed from the published `map.wasm` this way. One rule to remember: the hash describes one specific build of the file. If you ever rebuild or re-optimize the module, its bytes change, so you must recompute the hash and re-sign the fabric.

## Step 2 - The signing credentials

Signing needs three things: a **private key** that produces the signature, a **leaf certificate** that holds the matching public key and your identity, and one or more **chain certificates** that connect your leaf up to a recognized authority. The private key is the secret half and never leaves your control; the certificates are public and get embedded in the signed file so that anyone loading the fabric can read who signed it and check the signature.

For local testing you do not need to obtain anything -- the repository ships a ready-made set under `tests/certs/`:

| File | Role |
|---|---|
| `provider-key.pem` | The private key you sign with. |
| `provider-cert.pem` | The leaf certificate ("Test Provider"), embedded in the signed file. |
| `ca-cert.pem` | The test root authority. Use it as the `--chain` when signing and as the `--trust` anchor when verifying. |

These are test credentials, safe to use because they only prove "signed by the test provider." For a fabric you actually publish, you supply your own. There are two ways to get a real certificate. If you want the wider world to recognize you, you obtain a certificate from a public certificate authority -- the same kind of organization that issues the certificates behind `https` web sites -- whose root is already trusted by the operating system. If you only need trust within your own organization or for your own testing, you can create your own certificate authority and issue your own leaf certificate from it; anyone who adds your authority to their trust store will then recognize your fabrics. Either way, the engine checks a signed fabric's chain against the operating system's certificate store.

## Step 3 - The signing tool

Signing and verifying are done from the command line. The reference implementation is a small tool called `SignMsf`, whose source lives inside the Sneeze engine project under `tools/SignMsf/`. You are welcome to build it yourself from source, but you do not need to: Artemis has the exact same signing and verifying tool built in, so there is nothing extra to download or compile.

Artemis exposes this through two command-line options, `--sign` and `--verify`. When you run it with either one, it does that job on the command line and exits instead of opening the browser window. Every command in the next two steps is `Artemis.exe` (just `Artemis` on Linux or macOS), run from wherever Artemis is installed. The example commands use paths relative to this example's folder, so run them from here, or adjust the paths to point at your own files.

If you would rather build the standalone `SignMsf` tool from the Sneeze source instead of using the browser, see [Building SignMsf from source](building-signmsf.md). It takes the same arguments (except that it infers signing, so it needs no `--sign` flag) and produces byte-for-byte identical results.

## Step 4 - Sign the fabric

Point Artemis at your payload, your key, your leaf certificate, and your chain certificate, and give it an output path. The `--sign` flag is what tells it to sign on the command line rather than open a window:

```powershell
Artemis.exe --sign --payload signed-stool-and-bucket.json --key tests\certs\provider-key.pem --cert tests\certs\provider-cert.pem --chain tests\certs\ca-cert.pem --out signed-stool-and-bucket.msf
```

On success it prints a line like `Signed signed-stool-and-bucket.msf (5009 bytes)`. That `.msf` file is your payload plus the signature plus the embedded certificates, all in one file. It is the only file from this example you publish; the plain `.json` stays home as your editable source.

The result is a single line of JWS compact form -- three base64url segments (the header carrying the embedded certificate chain, the payload, and the signature) joined by dots. You can view the full signed file in the repository at [`examples/03-signing/signed-stool-and-bucket.msf`](../../examples/03-signing/signed-stool-and-bucket.msf).

Your output file will look something like this:
```
eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXUyIsIng1YyI6WyJNSUlEWFRDQ0FrV2dBd0lCQWdJQkFqQU5CZ2txaGtpRzl3MEJBUXNGQURCQU1Rc3dDUVlEVlFRR0V3SlZVekVWTUJNR0ExVUVDZ3dNVDAxQ1NTQlVaWE4wSUVOQk1Sb3dHQVlEVlFRRERCRlBUVUpKSUZSbGMzUWdVbTl2ZENCRFFUQWVGdzB5TmpBME1UY3lNakkwTkRsYUZ3MHlOekEwTVRjeU1qSTBORGxhTUVJeEN6QUpCZ05WQkFZVEFsVlRNUnN3R1FZRFZRUUtEQkpVWlhOMElGQnliM1pwWkdWeUlFbHVZeTR4RmpBVUJnTlZCQU1NRFZSbGMzUWdVSEp2ZG1sa1pYSXdnZ0VpTUEwR0NTcUdTSWIzRFFFQkFRVUFBNElCRHdBd2dnRUtBb0lCQVFEbjZybTdhc25BOVRMYmpUY3NXMEJHdWwra1lCNWtLYW0xTm9UMWxUcW1WZGZzYzVENVRERXEwT0RabjZxQjRXRXExVHN5dERkRld0VEpyNTg3RW1XbkhicFNocS96VllMaDZQcXoweDZPZ2M1d1F1VldzTXNLS2plMmU0cjdldnIrVG55K0NvL2RYWXNBS2NUTnV5ZWZ0TlI3cEtrT1dvRG9MVmFDa1UvU1pwaWRiUzJUektJYUgrN2x3bVFPWU1ld1lxOTkrcW5pNVh4WW10blJXdDNlNVVqbHFqQmFocTU3Q1VnaDArYWliTXkzU001ZHlIZi81d2RHNXRuSUI1YmlVSjE0ajNhekdNL0IvMnI3UXhhbGdmWU1ERnFxQlY1dzlHMkdxZ1NkWlJCLzJKNjhUWHFicEVLdWtNN2thWnJpMURZbGNsYnNXYlJnaUVGcWJnR1JBZ01CQUFHallEQmVNQXdHQTFVZEV3RUIvd1FDTUFBd0RnWURWUjBQQVFIL0JBUURBZ2VBTUIwR0ExVWREZ1FXQkJTbThqazIrQWpFbDZDRndiM0hRSlN2K3JWNUVqQWZCZ05WSFNNRUdEQVdnQlFHN1NUQWtJVkJjY0IyS2xIOVJ5YTB6YjNQWnpBTkJna3Foa2lHOXcwQkFRc0ZBQU9DQVFFQWxrVFR0Z0pTWXRoMDJnVGhHRW5vbHdYU1lETTdUYjZVRGFxck9hVHNhUmpWcXJIY3I3cU9LL1hCVm8xb1BDanBWT29tY0NYTGlycTlMREJLZEZpRWcwSWtLN2NWMldQU0hGUkt1M1ZqMmpmd01WaHlhaDM0cDl5TjY4cXlWcGNUZnZXN1hyeTJEenVIUEdRa2Y5RnN3Vml2bTE0YWNSVytWUG9nRzdZNjkrb2RsTkNsWU1USW9SSStBdHdnL0c1NUtIZjRiQ1RLaHRFOUJwaUVuUmJDcmNiZXRtdUhDR0pYb1FUbTcvd0JBbU5rNU0zSFVzc0ZueEExT3d1OVlpSFVoZ2Vjd1pFNy9UVTZKK3o3dnkxSzQwV1FjRkU0Z2V3T2NCTnV3VXN5KzR2TlNRU0RWbElTS3dYMTlacFZ0ZklkNVZTY0cxb2ZqUmtTWGpMVHE2bHkyQT09IiwiTUlJRFBUQ0NBaVdnQXdJQkFnSUJBVEFOQmdrcWhraUc5dzBCQVFzRkFEQkFNUXN3Q1FZRFZRUUdFd0pWVXpFVk1CTUdBMVVFQ2d3TVQwMUNTU0JVWlhOMElFTkJNUm93R0FZRFZRUUREQkZQVFVKSklGUmxjM1FnVW05dmRDQkRRVEFlRncweU5qQTBNVGN5TWpJME5EbGFGdzB5TnpBME1UY3lNakkwTkRsYU1FQXhDekFKQmdOVkJBWVRBbFZUTVJVd0V3WURWUVFLREF4UFRVSkpJRlJsYzNRZ1EwRXhHakFZQmdOVkJBTU1FVTlOUWtrZ1ZHVnpkQ0JTYjI5MElFTkJNSUlCSWpBTkJna3Foa2lHOXcwQkFRRUZBQU9DQVE4QU1JSUJDZ0tDQVFFQW5ldWtlTFRqdDQrNFJ5MTVFajlmeitsWmlxV0xVTXhUYldnMlhjWk1uTXZKRGZ5QnMrRHFhNDV3ZDdudFpkUGlkczJMK3JSSis5cGFRNlhqZWFvTHV6clM1bmpiU3pPL3dSelkyczJRVk9OQnpYVW55Rmw4N2ZjNUMrRmwrSFRqb1pPZk5zQkVITlZpWVdLZGZlb2gzNDVGeTFPNFYxUGFlZkx0Z3NPRVhaWjNHRlMxcks1THdZNENLaEoxRzJnQ05JeTZlWlBOSlNQbVZiUFhVVDlVNXUzWWE3UUxUcVRvUkVTdkZWUTdDb2hEYVZENEl3bm5McG8vNEdkWVRGM2F5TmNZRDhlSUhEa3RIL1NSNlBaRUQzdHo5TUNKOExjcFpxMURESzZIOGxrc2o3VmZTTmpGcTh3Z0ExK2xsaEE4VnhsQmVrTDZLM2FKK1NteVhocTUrUUlEQVFBQm8wSXdRREFQQmdOVkhSTUJBZjhFQlRBREFRSC9NQTRHQTFVZER3RUIvd1FFQXdJQkJqQWRCZ05WSFE0RUZnUVVCdTBrd0pDRlFYSEFkaXBSL1VjbXRNMjl6MmN3RFFZSktvWklodmNOQVFFTEJRQURnZ0VCQUR2UzdQenZ4UnNJODM3c3p4MUpGVjFEVk1GZmZOQ3hpcW5oUHl3WFNBSlVFMTA5VVhCaWwxSVB6VXNZTnQ1WEhaUFhUVEhYdFF4ZW5xVndYbUtrU3QwWVIzcEorQThQOEFzdVhHWE16bmZPZ2lpSkY2WkFLc3lETzZOR3B1ZzFqL1BGUVRJR3MyS0RmWTJQcTNBWUdRbW1kL0ErZkN5TnRCK29uY0RXSmM2N1NRRXp0dVlkYlcyRnZzaTdBNFpBdDJnTUdBM3lrOVNROU9nNWo4NDQ1TWR1Z2NWczRESWN1RWpuZ0xVWFZrNCszZUtPUTVDeW5rRjd2SmtPZjFvSi9UcjQzNmRURzRoMmF2S3YwbDh3NE1mcWhRb2pzd3BZT1V3WDZMRmFhb0VrbWxvdGRaYWROZklSNG83OEwrVElFTUx0WUVEcldkemVOZWVaQUN4VXA3WT0iXX0.eyJkYXRhIjoie1wiY29udGFpbmVyXCI6XCJleGFtcGxlLXNpZ25lZFwiLFwiZGF0YVwiOntcInNjZW5lXCI6e1wiQ2hpbGRyZW5cIjpbe1wiSGVhZFwiOntcIlNlbGZcIjpcIlAtP1wifSxcIk5hbWVcIjpcIkJ1Y2tldFwiLFwiUmVzb3VyY2VcIjp7XCJzUmVmZXJlbmNlXCI6XCJhc3NldHMvQnVja2V0LmdsYlwifSxcIlRyYW5zZm9ybVwiOntcIlBvc2l0aW9uXCI6WzAuMCwwLjAsMC40MjhdfX0se1wiSGVhZFwiOntcIlNlbGZcIjpcIkwtP1wifSxcIk5hbWVcIjpcIkZpbGwgTGlnaHRcIixcIlByb3BlcnRpZXNcIjp7XCJmQW5nbGVGYWxsb2ZmXCI6MTIuMCxcImZBbmdsZU9wZW5pbmdcIjo0MC4wLFwiZkJyaWdodG5lc3NcIjowLjQsXCJmQ29sb3JcIjpcIjB4QjhDQ0ZGXCJ9LFwiVHJhbnNmb3JtXCI6e1wiUG9zaXRpb25cIjpbLTAuNSwtMC40NSwwLjUxNF0sXCJSb3RhdGlvblwiOlswLjAsLTAuMDExOSwwLjM1ODIsMC45MzM2XX0sXCJUeXBlXCI6e1wiYlR5cGVcIjoyfX0se1wiSGVhZFwiOntcIlNlbGZcIjpcIkwtP1wifSxcIk5hbWVcIjpcIlJpbSBMaWdodFwiLFwiUHJvcGVydGllc1wiOntcImZBbmdsZUZhbGxvZmZcIjoxMC4wLFwiZkFuZ2xlT3BlbmluZ1wiOjM1LjAsXCJmQnJpZ2h0bmVzc1wiOjAuNCxcImZDb2xvclwiOlwiMHhGRkUwQjBcIn0sXCJUcmFuc2Zvcm1cIjp7XCJQb3NpdGlvblwiOlstMC4xNSwwLjU1LDAuODE0XSxcIlJvdGF0aW9uXCI6WzAuMCwwLjI4NDYsLTAuNTQ4OSwwLjc4Nl19LFwiVHlwZVwiOntcImJUeXBlXCI6Mn19XSxcIkhlYWRcIjp7XCJTZWxmXCI6XCJQLT9cIn0sXCJOYW1lXCI6XCJTdG9vbFwiLFwiUmVzb3VyY2VcIjp7XCJzUmVmZXJlbmNlXCI6XCJhc3NldHMvU3Rvb2wuZ2xiXCJ9fX0sXCJtb2R1bGVzXCI6W3tcImhhc2hcIjpcInNoYTI1Ni05NmQ0YzBiNmY5OGFkYjk4MTllMWYyNDJhYTY2NjRkN2JkODI1MDI1ZGMwMTU5YjFjYTQ0MjBlYTA4ZTZjODU3XCIsXCJ1cmxcIjpcIndhc20vbWFwLndhc21cIn1dLFwicHJpbWFyeVwiOntcImFtYmllbnRcIjp7XCJmQnJpZ2h0bmVzc1wiOjAuMDUsXCJmQ29sb3JcIjpcIjB4RkZGRkZGXCJ9LFwiZGlyZWN0aW9uYWxcIjp7XCJSb3RhdGlvblwiOlswLjAsMC41ODA4LDAuNjY2MywwLjQ2OF0sXCJmQnJpZ2h0bmVzc1wiOjAuNSxcImZDb2xvclwiOlwiMHhGRkZGRkZcIn19LFwic2VydmljZXNcIjpbXX0ifQ.DDe_rUbJuJbdfNOzppECIO34-AFJQ6uHdgH2APJ6GXf_BqkyBigXLRfTndnhHbQifzmkCxidJdxDfzz27uXf4gu5ILcITCD0WU9KZ2E69Qp36dXDjR-enZAXZjDAHZZ5OMqJCL3ynjAcQggrKvG3LpHSbrfvxFk8kx_YsqR019Xi9PYffW3iaDA_pNFDksC09i-CmnldSmKdZfDpK9sCHmFweNWFIio3hLypznwZ5wqGIGaFKmDpoNv7eUdnUlp8JYRMRNYTkInP9ndbJVvbArMlFFoMtCetwB9ltEIy9ksqM_Q2LX-kYudiePvnmh5nANc2uQHcco96YjweewtAyQ
```

## Step 5 - Verify before you publish

Always check the file you just produced before you put it anywhere. Artemis verifies with `--verify`:

```powershell
Artemis.exe --verify signed-stool-and-bucket.msf --trust tests\certs\ca-cert.pem
```

`--trust` tells it which authority to check the chain against -- here, the same test root we signed with. A good result prints `Signature: VERIFIED`, followed by the certificate chain (each certificate's subject, issuer, validity dates, and key) and the full decoded payload. Reading that payload back is a useful habit: it shows you exactly what got signed, so you can confirm the container name, the module hash, and the scene are all what you intended. If the signature had failed or the chain did not reach the trusted authority, it would print `Signature: FAILED` instead, along with the reason.

## Deploying it

Publishing a signed fabric works like the earlier examples, with one change: you publish the `.msf`, not the `.json`.

1. Upload `signed-stool-and-bucket.msf` to your web server. A signed copy of this example is also hosted at `https://cdn.rp1.com/sneeze/examples/signed-stool-and-bucket.msf`.
2. Make sure everything the fabric references still resolves. The module and models are named relative (`wasm/map.wasm`, `assets/Stool.glb`, `assets/Bucket.glb`), and they resolve against the fabric's own address just as before -- so they must sit beside the `.msf`, under `https://cdn.rp1.com/sneeze/examples/`. Signing does not change how references resolve; it only wraps the fabric.
3. Give the browser the address of the `.msf`. It fetches the signed fabric, verifies it, reads the author's identity, then loads the module -- checking it against the pinned hash -- and the models, and builds the scene.

As always, do not depend on RP1's server for your own work. Host your own copies of the module and assets and point the fabric at addresses you control.

## What is next

Example 04 introduces attaching a whole separate fabric as a child, so you can compose large spaces out of independently authored and independently signed pieces.

## See also

- [Building SignMsf from source](building-signmsf.md) -- how to build the standalone signing tool from the Sneeze project, for the few who want it instead of using the browser.
- [MSF system](../systems/msf.md) -- the engine internals of parsing, verification, and the certificate chain.

---

[Example 02](02-stool-and-bucket.md) | [Examples](index.md) | [Home](../Home.md)
