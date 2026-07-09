---
title: "Building SignMsf from source"
tier: Examples
audience: [author]
sources:
  - examples/03-signing/building-signmsf.md
  - tools/SignMsf/main.cpp
  - scripts/build-windows.ps1
  - scripts/build-linux.sh
verified: c9029f4
nav:
  prev: examples/03-signing.md
---

# Building SignMsf from source

This page is the long way around, and most people should not need it. [Example 03](03-signing.md) signs and verifies fabrics with the metaverse browser you already have, which is the recommended path. This page is here for the small number of people who want the standalone `SignMsf` command-line tool built straight from the Sneeze source -- for a build server, a headless signing machine, or simply to work with the engine's own tool rather than the browser.

`SignMsf` is a small command-line program that lives in the Sneeze source tree under `tools/SignMsf/`. It wraps a payload into a signed `.msf` and verifies signed files, and it produces byte-for-byte identical results to the browser's built-in signing. You build it once from the engine's own build system; afterwards the executable stays on disk and you reuse it for every fabric you ever sign.

## Before you start

This assumes you have already built the Sneeze engine at least once, so its third-party dependencies are in place. Building the engine and its dependencies from scratch is a separate, much longer task; if you have not done that yet, follow the [Building Sneeze](../guides/building.md) guide first and then come back here. The commands below only regenerate the project files and compile the small tool -- they never rebuild the dependencies.

Every command is run from the root of your Sneeze checkout: the folder that contains `src`, `tools`, and `scripts`. Every path shown is relative to that folder, so the commands work exactly as written no matter where you cloned the repository -- there is nothing to change for your own machine.

## Windows

Open PowerShell in the checkout root and run these two commands in order:

```powershell
.\scripts\build-windows.ps1 -Fresh
cmake --build builds\windows-x64\build --target SignMsf --config Release
```

The first command regenerates the build files; it is quick and builds nothing -- it neither compiles the engine nor touches the dependencies, it just makes sure the tool is present in the build. The second command compiles `SignMsf` itself. When it finishes, your copy of the tool is here, relative to the checkout root:

```
builds\windows-x64\install\release\bin\SignMsf.exe
```

## Linux and macOS

The same two steps use the platform build script instead:

```bash
./scripts/build-linux.sh --fresh
cmake --build builds/<your-platform>/build --target SignMsf --config Release
```

Here `<your-platform>` is the folder the build script creates for your system, something like `linux-x64`, and the tool lands at `builds/<your-platform>/install/release/bin/SignMsf`.

## Using it

`SignMsf` takes the same arguments as the browser's `--sign` and `--verify` modes, with one difference: it infers signing from the presence of `--payload` and `--out`, so it needs no `--sign` flag.

To sign:

```powershell
SignMsf.exe --payload signed-stool-and-bucket.json --key tests\certs\provider-key.pem --cert tests\certs\provider-cert.pem --chain tests\certs\ca-cert.pem --out signed-stool-and-bucket.msf
```

To verify:

```powershell
SignMsf.exe --verify signed-stool-and-bucket.msf --trust tests\certs\ca-cert.pem
```

On Linux or macOS, use `SignMsf` in place of `SignMsf.exe`. Everything else about signing, verifying, and the credentials is exactly as [Example 03](03-signing.md) describes.

## See also

- [Example 03 - Publishing a Signed Fabric](03-signing.md) -- the recommended path, using the browser's built-in signing.
- [Building Sneeze](../guides/building.md) -- how to build the full engine and its dependencies.

---

[Example 03](03-signing.md) | [Examples](index.md) | [Home](../Home.md)
