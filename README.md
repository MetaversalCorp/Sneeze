# Sneeze — Open Metaverse Browser Engine

Sneeze is the engine behind the Open Metaverse Browser, developed by the Open Metaverse Browser Initiative (OMBI), a project under the Metaverse Standards Forum. It handles rendering (via ANARI and Halogen), sandboxed code execution (via WebAssembly/Wasmtime), SPIR-V shader validation (via SPIRV-Tools), GPU compute dispatch (via Vox), XR device access (via OpenXR), HTTP networking (via curl), realtime spatial networking (via RMAP and Map), UI (via RmlUi), 3D model loading (via fastgltf), cryptographic trust verification (via BoringSSL and jwt-cpp), and structured data interchange (via nlohmann/json).

Sneeze builds as a **static library** (`Sneeze.lib` on Windows, `libSneeze.a` elsewhere). It is consumed by a host application via CMake's `add_subdirectory`. The application provides windowing and input; the engine renders into a surface the application supplies.

Building Sneeze conceptually has two phases, keyed by `(platform, config)`:

1. **Deps** — build every third-party library from source into `deps/builds/<platform>/<config>/libs/`. Slow, one-time (~1–2 hours on a fresh machine). Per-config trees (`debug/` and `release/` are independent).
2. **Sneeze** — compile + link the Sneeze static library. Single multi-config tree at `builds/<platform>/build/` with per-config outputs at `builds/<platform>/install/{debug,release}/{bin,lib}/`. Fast, every edit (seconds). On Windows, `Sneeze.sln` opens once and both Debug and Release build from the IDE dropdown.

One script per platform drives both. By default it runs phase 2 — that's the 99% command. Pass `-All` / `--all` for both phases (first-time setup), or `-Deps` / `--deps` for phase 1 only (dep refresh). Details in [Quick Start](#quick-start).

`<platform>` is a platform slug — `windows-x64`, `linux-x64`, `macos-arm64`, etc. `<config>` is `debug` or `release`. Debug and Release live in fully separate trees, so you can keep both populated side-by-side without rebuilding.

Dependency builds are **stamp-cached** — once a dep builds successfully, the script skips it on every later deps run until you explicitly tell it otherwise. Source clones are shared across configs via `deps/repos/`, so switching between Debug and Release does not re-clone anything.

---

## Documentation

This README covers building. Two other bodies of documentation exist:

- **In-repo module notes** — terse reference notes live beside the code as `*.md` files next to the source they describe (for example `src/sneeze/network/Network.md`, `src/context/scene/Scene.md`). Each describes what its module is now and how to use it. These are the authoritative in-repo reference and the single source of truth is the code itself.
- **The curated reference manual (wiki)** — the newcomer-facing manual (overview, architecture, per-subsystem systems pages, per-class API, and guides) is maintained in a **separate repository**, `MetaversalCorp/SneezeDoc`, and is no longer part of this repo.

---

## Prerequisites

You need the following installed before building. Open a terminal and check each one:

| Tool | Purpose | Check command | Minimum version |
|------|---------|---------------|-----------------|
| **Git** | Clones this repo and all dependencies | `git --version` | any |
| **CMake** | Generates build files, orchestrates dependency builds | `cmake --version` | 3.20 |
| **C/C++ compiler** | Compiles all C/C++ code | Windows: `cl` ^1 / Linux: `g++ --version` / macOS: `clang++ --version` | C++17 support |
| **Rust / Cargo** | Builds Wasmtime (WebAssembly runtime) from source | `rustc --version` | any |
| **Python 3** | Drives the build scripts' dependency-graph tooling (reads `deps/dependencies.json` for build order, pins, and verification — runs on *every* invocation); also used by glslang's build to generate source tables | `python --version` (Win) or `python3 --version` | 3.x |
| **Go** | Used by BoringSSL's build for code generation | `go version` | any |
| **NASM** | Assembler for BoringSSL's optimized crypto routines (x86/x64 only) | `nasm --version` | any |
| **System dev packages** (Linux only) | Vulkan/X11/GL headers needed by Filament + OpenXR builds | `dpkg -l libvulkan-dev` (Debian/Ubuntu) | any |

^1 On Windows, run `cl` from a **"Developer PowerShell for VS 2022"** window (search for it in the Start Menu), not a regular terminal. That window pre-sets every environment variable MSVC needs. The build script in this repo is PowerShell-native and expects that environment.

If all seven commands print a version number, skip ahead to [Quick Start](#quick-start). Otherwise, install what's missing:

---

### Git

- **Windows:** Download from [git-scm.com](https://git-scm.com/). Accept defaults. When asked about PATH, choose "Git from the command line and also from 3rd-party software."
- **Linux:** `sudo apt install git` (Debian/Ubuntu) or `sudo dnf install git` (Fedora)
- **macOS:** `xcode-select --install`

---

### CMake

CMake is the build system generator that orchestrates downloading and compiling all dependencies. Several dependencies also expect `cmake` to be on PATH.

- **Windows:** Download the `.msi` installer from [cmake.org/download](https://cmake.org/download/). During installation, select **"Add CMake to the system PATH for all users"**.
- **Linux:** `sudo apt install cmake` (Debian/Ubuntu) or `sudo dnf install cmake` (Fedora). If your distro's version is older than 3.20, download a newer release from [cmake.org/download](https://cmake.org/download/).
- **macOS:** `brew install cmake` (requires [Homebrew](https://brew.sh/))

---

### C/C++ Compiler

- **Windows:** Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community edition is free). Select the **"Desktop development with C++"** workload. This includes the MSVC compiler, linker, and Windows SDK. You don't have to use the Visual Studio IDE — just having it installed provides the compiler toolchain that our script calls.
- **Linux:** `sudo apt install build-essential` (Debian/Ubuntu) or `sudo dnf install gcc-c++` (Fedora). Our Linux script defaults to clang with libc++; install clang via your package manager if `clang++` isn't already present.
- **macOS:** `xcode-select --install`

---

### Rust / Cargo

We aren't writing Rust code — we just need its compiler to build Wasmtime from source. Pick whichever install method you prefer:

- **Option A — Official website (all platforms):** Visit [rust-lang.org/tools/install](https://rust-lang.org/tools/install/). On Windows, download and run `rustup-init.exe`. On Linux/macOS, follow the one-line install command. Accept the defaults. Installs to your home directory only — no system-wide changes.
- **Option B — winget (Windows only):** `winget install Rustlang.Rustup`. You can inspect the package first with `winget show Rustlang.Rustup`.

After installing, close and reopen your terminal so `rustc` and `cargo` are on your PATH. To uninstall later: `rustup self uninstall`.

---

### Python 3

Python is a build-time requirement, not a runtime one — no Python runs inside the engine. The build **scripts** invoke `tools/DepGraph/depgraph.py` on every run to read the dependency manifest (`deps/dependencies.json`) for build order, version pins, and verification, so `python` must be on PATH even for a plain Sneeze build. glslang's build also uses Python for code generation. Standard-library only — no `pip install` step.

- **Windows:** Download from [python.org](https://www.python.org/downloads/). During installation, check **"Add python.exe to PATH"**. You may also need to disable the Windows Store alias: **Settings > Apps > Advanced app settings > App execution aliases** — turn off `python.exe` and `python3.exe`.
- **Linux:** `sudo apt install python3` (Debian/Ubuntu) or `sudo dnf install python3` (Fedora). Usually pre-installed.
- **macOS:** `brew install python3` or download from [python.org](https://www.python.org/downloads/)

---

### Go

Go is used only at build time by BoringSSL's code generation scripts. No Go code runs at runtime.

- **Windows:** `winget install GoLang.Go`, then close and reopen your terminal for PATH to update. Or download the `.msi` installer from [go.dev/dl](https://go.dev/dl/).
- **Linux:** `sudo apt install golang-go` (Debian/Ubuntu) or `sudo dnf install golang` (Fedora)
- **macOS:** `brew install go`

---

### NASM

NASM (Netwide Assembler) is used by BoringSSL to compile optimized assembly routines for cryptographic operations on x86/x64 processors. On macOS with Apple Silicon, NASM is **not** required — BoringSSL uses the system ARM assembler instead.

- **Windows:** `winget install NASM.NASM`. You may need to add the install directory to your PATH manually (typically `%LOCALAPPDATA%\bin\NASM`).
- **Linux:** `sudo apt install nasm` (Debian/Ubuntu) or `sudo dnf install nasm` (Fedora)
- **macOS (Intel):** `brew install nasm`
- **macOS (Apple Silicon):** Not required.

---

### Linux system development packages

The Linux dep build needs Vulkan headers, OpenGL/X11 development libraries, and the clang+libc++ toolchain that the default `cmake/toolchain-linux-clang.cmake` selects. Required on Debian/Ubuntu/WSL:

```bash
sudo apt-get update
sudo apt-get install -y clang lld libc++-dev libc++abi-dev libvulkan-dev libgl-dev libx11-dev ninja-build
```

This is the same package set that CI installs (`.github/workflows/build.yml`, the `linux` job's `system-deps`). On Fedora or other distributions, translate the package names — the names above (`libvulkan-dev`, `libgl-dev`, etc.) are Debian-flavor.

Optional — silences a few "Could NOT find" status messages during configure. The deps fall back to bundled or unused paths, so the build still succeeds without these:

```bash
sudo apt-get install -y libegl1-mesa-dev libjsoncpp-dev libwayland-dev libxcb1-dev libxkbcommon-dev
```

| Skipped optional | What's affected | Why it's still fine |
|---|---|---|
| `libegl1-mesa-dev` | Filament probes EGL for OpenGL/GLES platform integration | Vulkan is the active backend per `deps/filament.cmake` — EGL path is unused |
| `libjsoncpp-dev` | OpenXR loader uses jsoncpp to parse runtime manifests | OpenXR-SDK ships a vendored copy in `src/external/jsoncpp/` and uses it as a fallback |

If you skip an optional package you'll see a one-line "Could NOT find &lt;name&gt;" message during configure — that's expected, not an error.

---

## Quick Start

One script per platform. No flag = build Sneeze (fast, what you want 99% of the time). `-All` / `--all` = build deps then Sneeze (first-time setup). `-Deps` / `--deps` = build deps only (rare refresh).

### First time — build deps and Sneeze (1–2 hours)

**Windows** — open **"Developer PowerShell for VS 2022"** (Start Menu) so MSVC is on your PATH, then from the repo root:

```powershell
.\scripts\build-windows.ps1 -All
```

If PowerShell refuses to run unsigned scripts, override the execution policy for just this command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -All
```

**Linux** — uses clang with libc++ by default (see `cmake/toolchain-linux-clang.cmake`). Arch auto-detected (`x64` on Intel/AMD, `arm64` on ARM):

```bash
./scripts/build-linux.sh --all
```

**macOS** — produces universal-binary dep libraries (arm64 + x86_64) targeting macOS 12.0+. The host-arch slug (`macos-arm64` on Apple Silicon, `macos-x64` on Intel) keys the output directories:

```bash
./scripts/build-macos.sh --all
```

Add `-Config Debug` / `--config Debug` to any of these for a Debug build. Debug and Release live in separate trees, so running both populates both side-by-side.

> **Debug requires Release filament first.** Halogen's Debug build uses the Release `matc` binary (Debug `matc` is 10–100x slower and makes a fresh Halogen Debug build take hours). Run the Release deps flow before the Debug deps flow on any given platform — `-All` / `--all` above builds Release by default, so the usual first-time sequence (Release first, then Debug) just works. If you try to build Debug Halogen without Release `matc` on disk, configure halts with the exact command to run first.

### Every day after that — build Sneeze (seconds)

```powershell
.\scripts\build-windows.ps1                    # Release (default)
.\scripts\build-windows.ps1 -Config Debug      # Debug
```

```bash
./scripts/build-linux.sh                       # Release (default)
./scripts/build-linux.sh --config Debug        # Debug
```

```bash
./scripts/build-macos.sh                       # Release (default)
./scripts/build-macos.sh --config Debug        # Debug
```

No dep checks, no configure step — this is a plain `cmake --build` against the Sneeze tree. If deps aren't there it will fail at link time; if the Sneeze tree itself doesn't exist yet (fresh checkout, rename, cache corruption), CMake will complain about a missing `CMakeCache.txt`. Fix by running with `-Fresh` / `--fresh` (reconfigures Sneeze only) or `-All` / `--all` (also builds deps first).

### Rare — refresh deps only (no Sneeze)

```powershell
.\scripts\build-windows.ps1 -Deps
```

```bash
./scripts/build-linux.sh --deps
./scripts/build-macos.sh --deps
```

Stamp-cached — only missing deps rebuild. Useful when an upstream dep changed and you want to refresh without also building Sneeze.

### Build artifacts

- `deps/builds/<platform>/<config>/libs/` — installed dep headers + libraries (per-config)
- `deps/builds/<platform>/<config>/build/` — deps CMake scratch + stamp files (per-config)
- `builds/<platform>/build/` — Sneeze CMake scratch (single multi-config tree)
- `builds/<platform>/install/<config>/lib/` — `Sneeze.lib` / `libSneeze.a`
- `builds/<platform>/install/<config>/bin/` — test executables and tools

The Sneeze side is a single multi-config tree (Visual Studio, Xcode, or Ninja Multi-Config): one `build/` directory carries both Debug and Release, and the `install/{debug,release}/` siblings receive whichever config is selected at build time via `cmake --build --config`. The deps side remains per-config — Debug and Release dep trees are independent. The `install/` wrapper mirrors each dep's `libs/<Name>/install/{bin,lib}` so the Sneeze output tree stays structurally symmetric with the deps output tree.

### What the script actually does

The three modes:

- **Default (Sneeze)** — one call to `cmake --build builds/<platform>/build --config <config>`. No deps work, no configure, no probing. Fails naturally if the tree or libs aren't there.
- **`-Deps` / `--deps`** — configure the deps tree via `cmake -S deps -B deps/builds/<platform>/<config>/build -D...`, then for each dep in order run `cmake --build <build-dir> --target <dep> --config <config>`. On success, drop a stamp file at `<build-dir>/.dep-stamps/<dep>.done` so the next run skips it. The Sneeze tree is *not* touched.
- **`-All` / `--all`** — deps flow (above), then `cmake -S src -B builds/<platform>/build -D...` to configure the single multi-config Sneeze tree (using the Visual Studio generator on Windows, Ninja Multi-Config on Linux/macOS), then `cmake --build … --config <config>` to emit that config. The two `cmake -S` invocations are separate; the deps tree and the Sneeze tree never see each other.

---

## Verifying the Build

After the script finishes, the static library lives in `builds/<platform>/install/<config>/lib/` and test executables in `builds/<platform>/install/<config>/bin/`. Substitute the slug and config that matches your run.

All subsystem tests are compiled into a **single `SneezeTest` executable**. Run it with no arguments for every suite, or pass a suite flag to run one. `--help` lists the suites. The current suite flags are `--wasm`, `--spv`, `--xr`, `--net`, `--ui`, `--compute`, `--vox`, `--jws`, `--network`, `--storage`, and `--console`.

**Windows (Release):**
```powershell
dir builds\windows-x64\install\release\lib\Sneeze.lib
dir builds\windows-x64\install\release\bin\SneezeTest.exe
builds\windows-x64\install\release\bin\SneezeTest.exe            # all suites
builds\windows-x64\install\release\bin\SneezeTest.exe --wasm     # one suite
```

**Linux (Release, x64):**
```bash
ls builds/linux-x64/install/release/lib/libSneeze.a
builds/linux-x64/install/release/bin/SneezeTest                  # all suites
builds/linux-x64/install/release/bin/SneezeTest --help           # list suites
```

**macOS (Release, Apple Silicon):**
```bash
ls builds/macos-arm64/install/release/lib/libSneeze.a
builds/macos-arm64/install/release/bin/SneezeTest
```

Each suite prints `ALL TESTS PASSED` (or similar) and the run exits cleanly. Two known exceptions:

- **`--xr`** prints "failed to find active runtime" on machines without a VR headset or XR runtime (SteamVR, Oculus). That's expected and the test handles it gracefully.
- **`--compute`** reports whether a native GPU backend (Vulkan, DX12, or Metal via Vox) was available. On headless CI or machines without a supported GPU it falls back to the CPU path — still a pass.

Alongside `SneezeTest`, the `bin/` directory also holds the CLI tools built from `tools/` — notably `SignMsf` (JWS signing/verification) and `GenCerts` (test certificate generation).

---

## Rebuilding After Code Changes

### You edited Sneeze source

Just run the script with no flags. It's a plain `cmake --build` against the pre-configured Sneeze tree — no dep checks, no reconfigure. Typical rebuild on one changed `.cpp` is a few seconds.

```powershell
.\scripts\build-windows.ps1                    # Release
.\scripts\build-windows.ps1 -Config Debug      # Debug
```

```bash
./scripts/build-linux.sh
./scripts/build-macos.sh
```

### One dependency changed upstream

Full-scrub rebuild that one dep — every other dep stays cached. `-Rebuild` wipes the dep's build tree, install tree, and all stamps (script + ExternalProject). The source clone in `deps/repos/` is preserved. Rebuilds are per-config, so rebuilding Release doesn't touch Debug.

**Windows:**
```powershell
.\scripts\build-windows.ps1 -Only filament -Rebuild
```

**Linux / macOS:**
```bash
./scripts/build-linux.sh --only filament --rebuild
```

(`-Only` and `-List` imply deps mode; the script won't touch Sneeze. `-Rebuild` is a *modifier* — it composes with whichever target set you pick, and it will NEVER cross the src ↔ deps wall on its own.)

### Halogen — moving an existing clone onto the pinned tag

Halogen is pinned in the manifest to an immutable release tag (`v1.1.9`). That pin only governs the **first** clone: if `deps/repos/Halogen` already exists on a different commit — e.g. a full `main` checkout left over from an earlier setup — a normal build reports it as a `MISMATCH` and halts (it never silently builds the wrong Halogen, and it never moves a clone on its own). `-Sync` / `--sync` reconciles it: fetch the pinned tag, check it out detached, and rebuild.

```bash
# First build from the pinned tag (fresh clone) — one config
./scripts/build-macos.sh --deps --only halogen

# Reconcile a stray clone onto the pin and rebuild BOTH configs (Release first,
# then Debug — the right order is automatic)
./scripts/build-macos.sh --only halogen --sync
```

`-Sync` builds Release before Debug within a single run, which matters for Halogen: its Debug build reuses the Release `matc` binary (see the [Quick Start](#quick-start) note). A Debug-only Halogen build with no Release Halogen on disk halts at configure with the exact command to run first.

### You want to inspect which deps are cached

```powershell
.\scripts\build-windows.ps1 -List
```

```bash
./scripts/build-linux.sh --list
```

### `-Rebuild` is a modifier, not a mode

`-Rebuild` is the universal "I don't care what state it was in — build it fresh" lever. It composes with whatever target set the other flags pick out, and it **never crosses the src ↔ deps wall on its own**. The deps folder can only be scrubbed when `-Deps`, `-Only`, or `-All` is explicitly on the command line.

| Invocation (Windows shown; bash is identical with lowercase `--` flags) | Deps touched? | Sneeze touched? | What happens |
|---|---|---|---|
| `-Rebuild` | no | yes | Run `cmake --build --target clean --config <cfg>` against the existing `builds/<platform>/build/` tree (cleans only the current config's compiled artifacts) and wipe `builds/<platform>/install/<cfg>/`. Preserves `CMakeCache.txt`, `CMakeFiles/`, and the generated `.sln`/`.vcxproj`, so an open Visual Studio solution doesn't need to reload. The other config's install tree and intermediates are untouched. If the tree has never been configured, falls back to a full configure + build. |
| `-Rebuild -Only filament` | that one dep | no | Scrub + rebuild one dep |
| `-Rebuild -Deps` | all deps | no | Scrub + rebuild every dep |
| `-Rebuild -All` | all deps | yes | Scrub + rebuild deps, then scrub + rebuild Sneeze |

**Rebuild just Sneeze** (common: you pulled src changes and want a clean build from scratch):
```powershell
.\scripts\build-windows.ps1 -Rebuild -Config Debug
```
```bash
./scripts/build-linux.sh --rebuild --config Debug
```

**Rebuild the entire per-config dep root:**
```powershell
.\scripts\build-windows.ps1 -Deps -Rebuild -Config Release
```
```bash
./scripts/build-linux.sh --deps --rebuild --config Release
```

**Rebuild deps *and* Sneeze from scratch:**
```powershell
.\scripts\build-windows.ps1 -All -Rebuild -Config Release
```
```bash
./scripts/build-linux.sh --all --rebuild --config Release
```

Source clones in `deps/repos/` are preserved across all forms of `-Rebuild` — no re-download. To wipe those too, delete `deps/repos/` manually.

### Build-script flags at a glance

Default (no flag) builds Sneeze only. Mode flags and the convenience flags that imply them:

| Windows (PowerShell) | Linux / macOS (bash) | Purpose |
|----------------------|----------------------|---------|
| *(none)* | *(none)* | Build Sneeze only — fast, no dep checks |
| `-Deps` | `--deps` | Build dependencies only — Sneeze is not touched |
| `-Fresh` | `--fresh` | Reconfigure the Sneeze tree **from scratch** (passes `cmake --fresh`, wiping `CMakeCache.txt` + `CMakeFiles/`), then build it. Deps tree not touched. Requires CMake >= 3.24. |
| `-All` | `--all` | Build dependencies, then configure + build Sneeze |
| `-Config Debug\|Release` | `--config Debug\|Release` | Build configuration (default: Release) |
| `-Only <dep>` | `--only <dep>` | Build one dep if not cached (implies deps mode) |
| `-List` | `--list` | Show dep order + cached/pending status (implies deps mode) |
| `-Rebuild` | `--rebuild` | **Modifier.** Force a full rebuild of whichever target(s) the other flags select, regardless of prior state. Alone → Sneeze. With `-Deps` / `-Only` / `-All` → scrubs + rebuilds the matching deps. NEVER touches deps unless a deps flag is explicitly present. Source clones preserved. |
| `-Verify` | `--verify` | **Read-only.** Report every dependency's status against the manifest and exit non-zero if any is out of date. Reaches the network for branch refs (`git ls-remote`) to detect being behind upstream. Modifies nothing. Statuses: `OK` / `BEHIND` / `MISMATCH` / `STALE` (see [Keeping dependencies in sync](#keeping-dependencies-in-sync)). |
| `-Sync` | `--sync` | **Modifier** (implies deps-targeting). The only path that moves a checkout. Brings the dep(s) in scope onto the manifest ref (tag/SHA → fetch + detached checkout; branch → fetch + **fast-forward**, never a hard reset), then rebuilds in **both Debug and Release** everything that needs it: what moved, what's missing a build stamp, and anything transitively depending on a dep being rebuilt. Resumable — a run that fails partway resumes on the next `-Sync`. With `-Only <dep>`, scope is that dep + its transitive dependents (not its dependencies). |

`-Deps`, `-Fresh`, and `-All` are mutually exclusive. `-Rebuild`, `-Verify`, and `-Sync` are modifiers, not modes.

---

## Keeping dependencies in sync

Sneeze depends on many repos, several of which depend on each other, and a few track a moving branch. [`deps/dependencies.json`](deps/dependencies.json) is the single source of truth for every dep's pinned ref and its dependency edges. Two flags manage the relationship between what the manifest says and what is on your disk — and **neither a normal build nor `-Verify` ever modifies a clone.** Only `-Sync` moves anything.

**The everyday workflow after pulling latest Sneeze:**

1. Pull Sneeze.
2. `-Verify` — are my dep checkouts current with the manifest?
3. `-Sync` if anything is out of date — bring them into line and rebuild what's affected.
4. Build normally (no flags) — fast, fully offline, no dep probing.

```powershell
.\scripts\build-windows.ps1 -Verify        # report only, never modifies anything
.\scripts\build-windows.ps1 -Sync          # bring everything into line + rebuild (Debug + Release)
```

```bash
./scripts/build-linux.sh --verify
./scripts/build-linux.sh --sync
```

### What `-Verify` reports

`-Verify` checks each dep's on-disk checkout against the manifest and reaches the network (`git ls-remote`) for branch refs so it can tell you when a branch has advanced upstream. It reports one status per dep and exits non-zero if any is not `OK`:

| Status | Meaning |
|--------|---------|
| `OK` | Checkout matches the manifest and the built lib is current. |
| `MISMATCH` | The clone is not on the pinned tag/SHA (or a branch is on a different commit). Fix with `-Sync`. |
| `BEHIND` | A branch-tracking dep is behind its upstream tip. Fix with `-Sync` (fast-forward). |
| `STALE` | The checkout is fine but the **built** lib is out of date, for either of two reasons: (a) a dependency of this dep moved, so this dep must relink against it; or (b) it is "not built" in a config (no build stamp — e.g. a `-Sync` that failed partway, or a config you never built). |

`STALE` is derived purely from git state, the dependency graph, and the per-config build stamps — nothing extra is recorded on disk. Because a dep whose checkout is correct is assumed to have been built (that is what `-Sync` guarantees), the whole system stays inspectable from git alone.

The build itself runs the same gate in **offline** mode (no network): configuring the deps tree halts if any clone is off its pinned ref, naming the offending dep. This is how "just building" can never silently use the wrong version — and never modifies anything to fix it.

### What `-Sync` does

`-Sync` is the one command that moves a checkout. For every dep in scope it:

1. **Reconciles the checkout** to the manifest ref — a tag or SHA is fetched and checked out detached; a branch is fetched and **fast-forwarded** (never a hard reset, so local commits worked ahead of a branch are preserved; a genuinely diverged branch is left alone with a warning).
2. **Rebuilds in both Debug and Release** every dep that needs it: anything that moved, anything missing a build stamp, and anything that transitively depends on a dep being rebuilt (rebuilding a dep invalidates its dependents' cached libs, so they come along automatically).

It is **resumable**: the rebuild set is scrubbed up front in both configs, so a build that fails partway leaves honest "not built" state, and re-running `-Sync` picks up exactly where it stopped rather than starting over.

Scope follows `-Only`:

- `-Sync` (no `-Only`) — reconcile and rebuild everything that needs it.
- `-Sync -Only <dep>` — reconcile that one dep and rebuild it plus its transitive **dependents** (the repos that consume it). Its own dependencies are *not* touched — an out-of-date dependency of the target is reported by `-Verify`, not silently changed here.

Source clones in `deps/repos/` are the only thing `-Sync` ever fetches into; the shared clone is reused across both configs.

---

## How the Build Works

If you just want to build Sneeze and get on with your life, the previous sections are all you need. This section is for when something breaks, or when you want to add a new dependency, or when you're curious about the architecture.

### Two isolated trees, nothing crosses

The repo has **two completely independent CMake projects** and no top-level that spans both:

- **`deps/CMakeLists.txt`** — the deps project. Knows only about files under `deps/`. Its only job is to orchestrate the third-party library builds: it includes every `deps/<name>.cmake`, sets up cross-dep ordering with `add_dependencies(...)`, and otherwise gets out of the way. Never references `src/`, never writes outside `deps/`.
- **`src/CMakeLists.txt`** — the Sneeze project. Knows only about files under `src/` and `tests/`. It `find_package()`s every installed dep under `${LIBS_DIR}/<Name>/install/` and produces `Sneeze.lib` + test executables. Never references `deps/`, never writes outside `builds/<platform>/`. Single multi-config tree: emits Debug into `install/debug/{bin,lib}` and Release into `install/release/{bin,lib}` using `$<LOWER_CASE:$<CONFIG>>` generator expressions on the output directory variables.

The scripts in `scripts/` are the only glue between the two. In `-All` / `--all` mode, a script builds the deps tree, then invokes CMake a second time on the Sneeze tree. Neither CMakeLists ever sees the other.

### The moving parts

- **`deps/dependencies.json`** — **the single source of truth** for every dep's repository URL, clone folder, pinned ref, and its direct dependency edges. Nothing else duplicates a version literal.
- **`deps/DepGraph.cmake`** — reads the manifest and exposes it to CMake: `DEP_URL_*`, `DEP_REF_*`, `DEP_FOLDER_*`, `DEP_DEPENDS_*`, `DEP_NAMES`, and `sneeze_dep_closure()`. The Python `tools/DepGraph/depgraph.py` reads the same manifest for the build scripts (topological `order`, per-dep `pin`, transitive `dependents`).
- **`deps/DepVerify.cmake`** / **`deps/verify.cmake`** — the read-only verification gate. `DepVerify.cmake` is `include()`d by `deps/CMakeLists.txt` to halt an offline build if any clone is off its pinned ref; `verify.cmake` is the standalone entry the scripts drive for `-Verify` (offline or network freshness mode). Neither ever modifies a clone.
- **`deps/CMakeLists.txt`** — standalone CMake project for the deps tree. Two modes: `cmake -S deps` (no `-DDEP`) builds all deps; `cmake -S deps -DDEP=<name>` builds a single dep (CI path). Reads the manifest via `DepGraph.cmake`, generates cross-dep `add_dependencies(...)` ordering from the manifest's edges, and gates the build (offline) via `DepVerify.cmake`. Derives `SNEEZE_CONFIG`, `SNEEZE_PLATFORM`, `SNEEZE_DEP_REPO`, and `LIBS_DIR` if not passed explicitly.
- **`deps/<name>.cmake`** — one file per third-party library. Each contains a single `ExternalProject_Add(...)` call that reads its URL/ref/folder from the manifest variables (`DEP_URL_<name>`, `DEP_REF_<name>`, `DEP_FOLDER_<name>`), clones into `${SNEEZE_DEP_REPO}/<Folder>/` (shared across configs), configures, builds, and installs under `${LIBS_DIR}/<Folder>/install/`. Both `deps/CMakeLists.txt` and the per-tier CI jobs in `.github/workflows/` `include()` these same files.
- **`src/CMakeLists.txt`** — standalone CMake project for Sneeze. `find_package()`s each installed dep under `${LIBS_DIR}/<name>/install/`. Forces `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` and `CMAKE_RUNTIME_OUTPUT_DIRECTORY` so `Sneeze.lib` always lands in `${SNEEZE_BUILD_ROOT}/lib/` and executables always in `${SNEEZE_BUILD_ROOT}/bin/`, regardless of generator.
- **`src/cmake/Find<Name>.cmake`** — small "find modules" for deps that don't ship their own CMake package config (BoringSSL, Wasmtime) or whose shipped config is fragile (Glslang, SPIRV-Tools). Each just looks under `${LIBS_DIR}/<name>/install/{lib,include}/` and reports back.
- **`scripts/build-*.{sh,ps1}`** — the glue. Default mode: `cmake --build <sneeze-tree>`. `-Deps` mode: `cmake -S deps` + per-dep stamped loop. `-All` mode: deps flow, then `cmake -S src` configure + build. The scripts compute per-config directories and pass them explicitly.
- **`scripts/build-deps.sh`** — shared bash helper used by Linux/macOS scripts. Runs `cmake -S deps` and the per-dep build loop with stamp caching.
- **`cmake/toolchain-*.cmake`** — optional CMake toolchain files for cross-compilation (e.g. AArch64 Linux, Linux clang). The platform scripts pass these to both `cmake -S deps` and `cmake -S src` when applicable.
- **`.github/workflows/build-platform.yml`** — CI orchestration. Each dependency gets its own job in a tier (tier0: no deps, tier1: depends on tier0, etc.) for parallelism. CI uses `cmake -S deps -DDEP=<name>` for single-dep builds and `cmake -S src` for the Sneeze build, exactly the same entry points as the scripts.

### Adding a new dependency

1. Add an entry to `deps/dependencies.json`: its `url`, `folder` (the clone/install directory name), and `ref` under `versions`, plus any direct dependency edges under `dependencies`. This is the only place versions and edges live.
2. Drop a new `deps/<name>.cmake` file with its `ExternalProject_Add`. Read the manifest variables (`DEP_URL_<name>` / `DEP_REF_<name>` / `DEP_FOLDER_<name>`) rather than hard-coding a URL or ref. Clone into `${SNEEZE_DEP_REPO}/<Folder>` (shared), build into `${LIBS_DIR}/<Folder>/build`, install into `${LIBS_DIR}/<Folder>/install`. Pass `-DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}`.
3. Add the dep name to the `SNEEZE_DEPS` list in `deps/CMakeLists.txt`. Build order and cross-dep `add_dependencies(...)` are generated automatically from the manifest edges — there is no hand-kept ordered list in the scripts to update.
4. If it doesn't ship a clean CMake package config, add a matching `src/cmake/Find<Name>.cmake`.
5. Reference it from `src/CMakeLists.txt` with `find_package(<Name> ...)` and link it into the `sneeze` target.
6. Slot it into the right tier in `.github/workflows/build-platform.yml`.

If the dep is built by another project that we control (like Vox), you can skip step 3 by making that upstream project ship a portable `<name>-config.cmake`.

---

## Directory Layout

```
Sneeze/
├── README.md
├── LICENSE                    Apache 2.0
├── NOTICE                     Third-party attributions
├── .gitignore
├── vcpkg.json                 Alternative package manifest (not required)
├── .github/
│   ├── CI.md                  CI/CD design doc
│   └── workflows/
│       ├── build.yml          CI entry point
│       └── build-platform.yml Reusable per-platform job
├── deps/                      Deps CMake project (standalone, never references src/)
│   ├── dependencies.json      SINGLE SOURCE OF TRUTH: every dep's url/folder/ref + edges
│   ├── DepGraph.cmake         Exposes the manifest to CMake (DEP_URL_*, DEP_REF_*, ...)
│   ├── DepVerify.cmake        Read-only gate included by CMakeLists (offline)
│   ├── verify.cmake           Standalone read-only gate the scripts drive for -Verify
│   ├── CMakeLists.txt         Deps entry: all deps by default, -DDEP=<name> for CI
│   ├── <name>.cmake           One recipe per dependency (ExternalProject_Add; reads the manifest)
│   │   --- Generated at build time, gitignored ---
│   ├── repos/                 Shared source clones (one per dep folder, reused across configs)
│   └── builds/<platform>/<config>/
│       ├── build/             Deps CMake scratch + .dep-stamps/
│       └── libs/<Folder>/{build,install}/
├── include/                   Public API headers (consumed by the host application)
├── src/                       Sneeze CMake project (standalone, never references deps/)
│   ├── CMakeLists.txt         Sneeze library target (forces lib/ + bin/ output)
│   ├── cmake/                 Find<Name>.cmake modules for deps w/o clean configs
│   ├── sneeze/                Engine core + engine-wide singletons
│   │   ├── control/           Engine thread, agent pools, metronome, job queues
│   │   ├── console/           Developer console
│   │   ├── network/           HTTP fetch + disk cache
│   │   └── storage/           Persistent JSON storage
│   ├── context/               Per-context (per-tab) subsystems
│   │   ├── scene/             Scene Object Model
│   │   ├── viewport/          Rendering pipeline + camera (ANARI, glTF bridge)
│   │   └── msf/               MSF/JWS signing + verification
│   ├── deps/                  Dependency wrappers (wasm, spirv, xr, ui, compute, gltf, stb)
│   └── persona/               Local identity proxy
├── tests/                     Test source + data (one *Test.cpp per subsystem)
├── tools/                     Standalone utilities (SignMsf JWS CLI, MsfViewer, DepGraph
│                              manifest tool, GenCerts, HelloWasm, MakeGlb, ...)
├── scripts/                   Platform build drivers
│   ├── build-deps.sh          Shared dep-iteration logic (Unix)
│   ├── build-linux.sh         Linux entry point
│   ├── build-macos.sh         macOS entry point
│   └── build-windows.ps1      Windows entry point (includes dep logic inline)
├── msvc/                      Hand-maintained Visual Studio project (mirrors CMake)
├── cmake/                     Cross-compilation toolchain files
│   ├── toolchain-linux-clang.cmake
│   └── toolchain-aarch64-linux.cmake
│
│   --- Generated at build time, gitignored ---
└── builds/<platform>/
    ├── build/                 Sneeze CMake scratch (single multi-config tree)
    └── install/<config>/
        ├── lib/               Sneeze.lib / libSneeze.a
        └── bin/               test executables + CLI tools (SignMsf, ...)
```

`<platform>` is a platform slug: `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, `macos-x64`, `ios-arm64`, `android-arm64`. `<config>` is `debug` or `release`. The Sneeze build tree is single-multi-config: one `build/` at `builds/<platform>/` drives both configs; each config lands in its own `install/<config>/` sibling. The deps tree remains per-config under `deps/builds/<platform>/<config>/`.

---

## Dependencies

All dependencies are built from source by the deps tree. No pre-built binaries.

**The single source of truth for every dependency's version, clone folder, and dependency edges is [`deps/dependencies.json`](deps/dependencies.json).** Each `deps/<name>.cmake` recipe reads its URL / ref / folder from that manifest (via `deps/DepGraph.cmake`, which exposes `DEP_URL_*`, `DEP_REF_*`, `DEP_FOLDER_*`, `DEP_DEPENDS_*`) — no version literal is duplicated anywhere. The build order, the CI cache keys, and the read-only verification gate all derive from the same manifest. This table is a convenience copy; the manifest wins if they ever drift.

A **ref** is a git tag (immutable), a raw commit SHA (immutable), or a branch name (tracks latest). Immutable pins are hard-enforced on every build. Branch pins ("track-latest") are only ever advanced by an explicit `-Sync` / `--sync` (see [Keeping dependencies in sync](#keeping-dependencies-in-sync)); a normal build never moves them.

| Dependency | Ref | Repository | Purpose |
|------------|-----|------------|---------|
| ANARI-SDK | `next_release` (branch) | [KhronosGroup/ANARI-SDK](https://github.com/KhronosGroup/ANARI-SDK) | Rendering abstraction API (core loader + backend headers; no bundled devices) |
| Halogen | `v1.1.9` (tag) | [MetaversalCorp/Halogen](https://github.com/MetaversalCorp/Halogen) | ANARI device built on Filament |
| Filament | `v1.71.0.mv.2` (tag) | [MetaversalCorp/filament](https://github.com/MetaversalCorp/filament) | PBR rendering engine (Metaversal fork of Google Filament) |
| Vox | `main` (branch) | [MetaversalCorp/Vox](https://github.com/MetaversalCorp/Vox) | GPU compute dispatch (Vulkan, DX12, Metal) |
| SPIRV-Headers | `vulkan-sdk-1.4.341.0` (tag) | [KhronosGroup/SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | SPIR-V spec headers (dep of SPIRV-Tools) |
| SPIRV-Tools | `vulkan-sdk-1.4.341.0` (tag) | [KhronosGroup/SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V assembler, validator, optimizer |
| SPIRV-Cross | `vulkan-sdk-1.4.341.0` (tag) | [KhronosGroup/SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | SPIR-V cross-compiler (used by Vox for DX12/Metal) |
| glslang | `vulkan-sdk-1.4.341.0` (tag) | [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) | GLSL-to-SPIR-V compiler (build-time only) |
| Wasmtime | `v43.0.0` (tag) | [bytecodealliance/wasmtime](https://github.com/bytecodealliance/wasmtime) | WebAssembly sandbox runtime |
| OpenXR-SDK | `release-1.1.58` (tag) | [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) | XR device abstraction |
| curl | `curl-8_9_1` (tag) | [curl/curl](https://github.com/curl/curl) | HTTP/HTTPS client (BoringSSL TLS backend) |
| BoringSSL | `93531a28b0…` (commit) | [google/boringssl](https://github.com/google/boringssl) | Cryptographic primitives for JWS signing/verification |
| jwt-cpp | `v0.7.0` (tag) | [Thalhammer/jwt-cpp](https://github.com/Thalhammer/jwt-cpp) | Header-only JWS/JWT creation and verification |
| nlohmann/json | `v3.11.3` (tag) | [nlohmann/json](https://github.com/nlohmann/json) | Header-only JSON library |
| RmlUi | `6.2` (tag) | [mikke89/RmlUi](https://github.com/mikke89/RmlUi) | HTML/CSS retained-mode UI toolkit |
| FreeType | `VER-2-13-3` (tag) | [freetype/freetype](https://github.com/freetype/freetype) | Font rasterization (dep of RmlUi) |
| fastgltf | `v0.9.0` (tag) | [spnda/fastgltf](https://github.com/spnda/fastgltf) | glTF / GLB model parser |
| SneezeSDK | `main` (branch) | [MetaversalCorp/SneezeSDK](https://github.com/MetaversalCorp/SneezeSDK) | WebAssembly guest SDK C headers (header-only; copied into the install prefix) |
| asio | `asio-1-30-2` (tag) | [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) | Header-only standalone (non-Boost) networking (consumed by socket.io / RMAP) |
| websocketpp | `0.8.2` (tag) | [zaphoyd/websocketpp](https://github.com/zaphoyd/websocketpp) | Header-only WebSocket library layered on asio (consumed by socket.io / RMAP) |
| socket.io-client-cpp | `3b7be7e…` (commit) | [socketio/socket.io-client-cpp](https://github.com/socketio/socket.io-client-cpp) | Socket.IO client (`sioclient_tls`, TLS via BoringSSL; consumed by RMAP) |
| RMAP | `main` (branch) | [MetaversalCorp/RMAP](https://github.com/MetaversalCorp/RMAP) | Realtime model-access / networking library (`RMAP.lib`, target `RMAP::RMAP`; folds in SB, REST, SocketIO service modules; linked into `Sneeze.lib`). Consumes Sneeze's copies of nlohmann/json, asio, websocketpp, BoringSSL, curl, and socket.io-client-cpp (full sharing — no re-fetch/rebuild). |
| Map | `main` (branch) | [MetaversalCorp/Map](https://github.com/MetaversalCorp/Map) | Metaversal C++ library built on RMAP (`Map.lib`, target `Map::Map`; mirrors RMAP's full-sharing dependency policy — reuses Sneeze's RMAP + shared deps rather than rebuilding its own). |

---

## Troubleshooting

| Problem | Likely cause | Fix |
|---------|-------------|-----|
| `cmake` command not found | CMake not installed or not on PATH | Install CMake and ensure its `bin/` directory is on your system PATH |
| `cl` not recognized on Windows | You're not in a Developer PowerShell for VS 2022 | Close your terminal and open "Developer PowerShell for VS 2022" from the Start Menu |
| `.\scripts\build-windows.ps1 cannot be loaded because running scripts is disabled` | PowerShell execution policy blocks local scripts | `powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1` |
| `rustc` command not found | Rust not installed | Install from [rust-lang.org/tools/install](https://rust-lang.org/tools/install/) or `winget install Rustlang.Rustup` (Windows), then restart your terminal |
| `go` command not found | Go not installed | `winget install GoLang.Go` (Windows), `brew install go` (macOS), or `sudo apt install golang-go` (Linux), then restart your terminal |
| `nasm` command not found | NASM not installed or not on PATH | `winget install NASM.NASM` (Windows), `brew install nasm` (macOS Intel), or `sudo apt install nasm` (Linux). On Windows, you may need to add the install directory to PATH. |
| Wasmtime build fails with "cmake not found" | Cargo's build system needs cmake on PATH (not just installed) | Add cmake's directory to your system PATH |
| BoringSSL build fails with "Go not found" | Go not installed | Install Go (see Prerequisites) and restart your terminal |
| `socketio` reports `[FAIL]` with MSB8066 exit code -1 on the *first* (cold) build | Spurious code from MSBuild's aggregate custom-build on the fresh git clone + recursive submodule fetch — the `sioclient_tls` lib and headers actually installed | Re-run the dep (`.\scripts\build-windows.ps1 -Deps -Only socketio`); the second pass is green. Harmless — the completion stamp was written, so a re-run is instant. |
| `rmap` configure fails: `Could not find a package configuration file provided by "asio"` (or `websocketpp`) | The asio/websocketpp CMake config shims aren't in their install trees — those installs predate `deps/asio.cmake` / `deps/websocketpp.cmake` gaining the shim step | Rebuild the two header-only deps so the shims regenerate: `.\scripts\build-windows.ps1 -Only asio -Rebuild` and `-Only websocketpp -Rebuild`, then rebuild `rmap`. |
| `rmap` configure fails: `The link interface of target "RMAP::RMAP" contains "asio::asio" but the target was not found` (in a Sneeze/consumer build) | The consumer didn't put asio/websocketpp/nlohmann-json install roots on `CMAKE_PREFIX_PATH` before `find_package(RMAP)`, so RMAP's `find_dependency` calls can't recreate the imported targets | Sneeze's `src/CMakeLists.txt` handles this; if wiring RMAP into another consumer, append those three `LIBS_DIR/<dep>/install` roots to `CMAKE_PREFIX_PATH` first. |
| A fresh/CI `rmap` clone builds its OWN boringssl/curl/socketio instead of reusing Sneeze's | RMAP's system-find `else()` branches for the compiled deps live only in the local `deps/repos/RMAP` clone and haven't been pushed to `MetaversalCorp/RMAP` — `deps/rmap.cmake` re-clones from GitHub when `.git` is absent | Push the RMAP `CMakeLists.txt` / `cmake/RMAPConfig.cmake.in` changes upstream, or preserve the existing `deps/repos/RMAP` clone (the recipe uses it as-is when `.git` exists). |
| ANARI "failed to load halogen library" | `anari_library_halogen.dll` not next to the application executable | The application's post-build step should copy it from `deps/builds/<platform>/<config>/libs/Halogen/install/bin/`. |
| `https://` requests fail with `curl=1` / `CURLE_UNSUPPORTED_PROTOCOL` | curl was built with no TLS backend — a stale ExternalProject cache configured it HTTP-only (e.g. curl configured before BoringSSL was on disk) | Scrub + rebuild curl so it reconfigures against BoringSSL: `./scripts/build-macos.sh --deps --only curl --rebuild` (`build-windows.ps1 -Deps -Only curl -Rebuild` / `build-linux.sh --deps --only curl --rebuild` elsewhere), then rebuild the consumer so it relinks the new static libcurl. |
| OpenXR test prints "failed to find active runtime" | No VR runtime installed (SteamVR, Oculus, etc.) | Expected on machines without a headset. The test handles this gracefully. |
| `SneezeTest --net` fails with connection errors | No internet connection | The HTTP tests make live requests. Expected to fail offline. |
| `python` opens the Microsoft Store | Windows Store alias is intercepting | Settings > Apps > Advanced app settings > App execution aliases — turn off `python.exe` and `python3.exe` |
| Build takes extremely long on first run | Wasmtime Rust compilation + Filament C++ compilation dominate | Normal — each is ~30 minutes on a fast machine. Subsequent runs skip both. Use `--list` to see stamp status. |
| One dep fails but others succeeded | Stamp caching — only the failed dep needs re-running | Fix the underlying issue (missing tool, network blip), then rerun with `--only <dep>` |
| You pulled a dep update from upstream and the script still skips it | Stamp files are by dep name, not content hash | `--rebuild --only <dep>` forces that one to rebuild from scratch. Rebuilds are per-config — rebuilding Release does not affect Debug. |
| `LNK2038 _ITERATOR_DEBUG_LEVEL mismatch` | You linked a Release Sneeze against Debug deps (or vice versa) | Debug and Release live in fully separate trees; this only happens if you manually mixed `LIBS_DIR` paths. Check that the `-Config` you passed to the build script matches the config your consumer is using. |
| Wasmtime rebuilds when I switch to Debug even though nothing in Rust changed | Wasmtime is built in Cargo release mode regardless of `SNEEZE_CONFIG`, but its install tree lives per-config | Each config gets its own copy under `deps/builds/<platform>/<config>/libs/Wasmtime/`. A Debug Rust build would be ~50x slower and ~10x larger, so Cargo stays in `--release` mode — but the install tree is duplicated so `find_package(Wasmtime)` works cleanly per config. |

---

## License

Apache License 2.0 — see individual source files for the full header..
