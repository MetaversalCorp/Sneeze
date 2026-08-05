# Dependency Management

Sneeze depends on 23 third-party and first-party repositories, several of which
depend on one another (e.g. `rmap` -> `socketio` -> `boringssl`/`asio`/`websocketpp`).
This document describes the single-source-of-truth system that pins every
repository's version, records the dependency graph, and enforces both on every
build without ever touching a checkout unless explicitly told to.

## The one hard rule

**Building never moves a checkout.** Compiling Sneeze, building a dependency,
or configuring either CMake project is *read-only* with respect to the source
clones in `deps/repos/`. The only path that fetches, checks out, or
fast-forwards a clone is the scripts' explicit `-Sync` (`--sync`) flag. A
checkout that does not match the manifest is a hard build error, not a silent
repair.

## The single source of truth: `dependencies.json`

Everything lives in `deps/dependencies.json`. Nothing is duplicated in the
recipes, the build scripts, or CI.

```json
{
  "versions": {
    "<name>": { "url": "<git url>", "folder": "<clone dir under deps/repos>", "ref": "<tag | sha | branch>" }
  },
  "dependencies": {
    "<name>": ["<direct dep>", "..."]
  }
}
```

- **`versions`** — one entry per repository: its clone URL, the folder it clones
  into under `deps/repos/`, and the pinned `ref` (a tag like `v1.1.9`, a full
  SHA, or a branch like `main`).
- **`dependencies`** — **direct edges only**. If `B` depends on `A`, list `A`
  under `B`. Do **not** repeat `A`'s own dependencies under `B`; the transitive
  closure is computed. Only deps with at least one edge need an entry.

To change a version, edit exactly one `ref` here. To change the graph, edit one
edge list here. Everything downstream (build order, verification, CI cache keys)
is derived.

## How the pieces consume it

| Consumer | File | Role |
|---|---|---|
| CMake variable exporter | `deps/DepGraph.cmake` | Parses the JSON, exposes `DEP_URL_<name>`, `DEP_FOLDER_<name>`, `DEP_REF_<name>`, `DEP_DEPENDS_<name>`, `DEP_NAMES`, and `sneeze_dep_closure()`. |
| Recipes | `deps/<name>.cmake` | Read `GIT_REPOSITORY ${DEP_URL_<name>}`, `GIT_TAG ${DEP_REF_<name>}`, and clone into `${SNEEZE_DEP_REPO}/${DEP_FOLDER_<name>}`. No literals. |
| Build-order generator | `deps/CMakeLists.txt` | Validates every `SNEEZE_DEPS` member has a manifest entry, then generates `add_dependencies()` from the direct-edge graph. |
| Verify gate | `deps/DepVerify.cmake` | The one read-only "is this checkout what the manifest pins?" implementation. |
| Verify entry point | `deps/verify.cmake` | Thin `cmake -P` wrapper the scripts and CI call. |
| Reporting / emitter | `tools/DepGraph/depgraph.py` | Read-only topo order + pin lookups for the build scripts, plus human queries. |

## The verify gate: two modes

`deps/DepVerify.cmake` compares a clone's `HEAD` against the manifest pin. It has
two modes because there are two moments that matter:

- **OFFLINE** — local only, no network. `HEAD` must equal the commit the pinned
  ref resolves to locally. Runs on **every build** (the deps `-DDEP=<x>` path,
  and `sneeze_verify_all` from the Sneeze src build when `USE_LOCAL_DEPS`). It
  catches a tag/SHA the manifest bumped but the clone never moved. It does **not**
  detect a branch's upstream moving — that needs the network.
- **FRESHNESS** — OFFLINE plus, for a branch ref, one read-only `git ls-remote`
  to compare against the upstream tip: local equal-or-ahead is OK, behind is
  flagged. Runs **only** in the explicit sync step (`-Verify` / `-Sync`), which
  is allowed to reach the network.

This split is deliberate: normal builds stay offline and fast; the network is
touched only when you deliberately ask "am I up to date with upstream?" right
after pulling latest Sneeze.

Status values: `OK`, `SKIP` (dep not enabled/cloned), `MISMATCH` (tag/SHA
wrong), `BEHIND` (branch behind upstream, FRESHNESS only), `UNKNOWN`, and
`STALE`.

### `STALE` — the built library is out of date

A dep whose own checkout is `OK` is **`STALE`** when its *built library* is out
of date, for either of two reasons:

1. **A dependency moved.** An out-of-date checkout sits somewhere in the dep's
   dependency closure. Example: `boringssl` is `MISMATCH`, so `curl` (which
   depends on it) reports `STALE (dependency out of date: boringssl)` even though
   `curl`'s own checkout is correct. This is derived **purely from git state +
   the graph** — nothing is recorded on disk about what a dep was built against.
   The rule that makes it sound: *a correct checkout implies it is built*, because
   `-Sync` is the only thing that moves a checkout and it always rebuilds.

2. **Not built.** The dep is missing its build stamp
   (`deps/builds/<platform>/<config>/build/.dep-stamps/<dep>.done`) in a config —
   i.e. a `-Sync` that failed partway, or a config you have never built. This is
   only reported by `-Verify` (which passes both configs' stamp dirs to the gate);
   the OFFLINE build gate never checks it, so a normal build is never blocked by a
   not-yet-built dep.

Both reasons make `-Verify` exit non-zero (action needed) and are cleared by
`-Sync`. Because `-Sync` builds both Debug and Release together, a fully synced
tree shows every dep `OK` in both configs.

## The workflow

Three steps, matching how you actually work:

1. **Update Sneeze to latest** — `git pull` on the Sneeze repo. This may bump
   `ref`s in `dependencies.json`.
2. **Make sure dependencies are up to date** — the networked check:
   - Windows: `.\scripts\build-windows.ps1 -Verify`
   - Linux/macOS: `./scripts/build-deps.sh --verify`

   Read-only. Reports every dep in FRESHNESS mode (`OK` / `BEHIND` / `MISMATCH` /
   `STALE`); exits non-zero if anything is stale. If it flags something,
   bring it into line:
   - `.\scripts\build-windows.ps1 -Sync` (all) or `-Only <dep> -Sync` (one)
   - `./scripts/build-deps.sh --sync` (all) or `--only <dep> --sync` (one)

   `-Sync` is the **only** thing that moves a checkout, and it does the whole
   catch-up in one shot:
   - **Moves** the out-of-date checkouts: a tag/SHA ref is fetched and checked
     out; a branch ref is fetched and **fast-forwarded** (never a hard reset —
     local commits worked ahead of the branch are preserved; a diverged branch is
     left as-is with a warning).
   - **Rebuilds**, in **both Debug and Release** (the checkout is shared across
     configs; the two build trees are not), every in-scope dep that moved, is
     **missing a build stamp**, or **transitively depends on any dep being
     rebuilt** (rebuilding a dep invalidates its dependents' cached libs — so a
     dep that moved in an earlier crashed run, and is rebuilt here for a missing
     stamp, still drags its already-stamped dependents along).
   - With `-Only <dep>`, the scope is that dep **and its dependents** (never its
     dependencies — an out-of-date dependency of the target is reported by
     `-Verify`, not touched here).
   - **Resumable.** The rebuild set is scrubbed in both configs **up front**, so a
     build that fails partway leaves honest "not built" state (no stamp) in both
     configs. Fix the error and re-run `-Sync`: the missing-stamp clause makes it
     rebuild only what is still unbuilt, resuming where it stopped. (This also
     means the first `-Sync` builds any config you have never built.)
3. **Build normally** — `.\scripts\build-windows.ps1` (or `-All` for first-time
   deps+Sneeze). The OFFLINE gate confirms checkouts match the manifest and halts
   with the offending dep named if not. Nothing is fetched or moved.

## Reporting: `tools/DepGraph/depgraph.py`

Python 3 stdlib only, read-only, never modifies anything. The build scripts use
`order` and `pin`; the rest are for humans.

```
depgraph.py order            topo order, deps before dependents (one per line)
depgraph.py pin <dep>        "<folder>\t<ref>\t<url>" for one dep
depgraph.py closure <dep>    transitive dependencies of <dep>
depgraph.py dependents <dep> transitive dependents of <dep>, topo-ordered
                             (what -Sync rebuilds when <dep> moves)
depgraph.py why <dep>        direct dependents + direct dependencies
depgraph.py validate         check for cycles / dangling edges (exit non-zero)
```

Manifest path defaults to `../../deps/dependencies.json`; override with the
`SNEEZE_DEP_MANIFEST` environment variable.

## CI

`.github/workflows/build-platform.yml`:

- A **`plan`** job runs `depgraph.py validate` then
  `scripts/ci-tier-matrix.py --github-output`. That script assigns each manifest
  dep to a longest-path layer (`tier0`…`tierN`) and emits GitHub Actions matrix
  JSON. Tier jobs consume those matrices — there is no hand-maintained dep list
  in the YAML. If the graph needs more layers than the workflow's fixed tier
  jobs (currently 0–4), the script fails and you add another tier job.
- **Every tier** caches each dep with `scripts/ci-dep-fingerprint.sh` (full
  transitive closure: refs, branch tips via authenticated `ls-remote`, recipes).
  Auth (`DEP_GIT_TOKEN`) runs **before** the fingerprint so private branch tips
  invalidate correctly. Cache hit skips build/apt/upstream downloads; cache miss
  rebuilds. Uploading the install tree as an artifact still happens on hit so
  `sneeze` can assemble `libs-<platform>/`.
- **`scripts/ci-dep-fingerprint.sh <dep>`** computes a transitive fingerprint:
  the dep's whole closure, each member's resolved ref (branch tips resolved via
  `git ls-remote`) plus a hash of its recipe file, combined into one SHA256. The
  `actions/cache` key for a dep is keyed on this fingerprint, so a change to any
  transitive dependency invalidates the caches of everything downstream — the
  bug the old per-recipe cache key had.

## Adding or changing a dependency

1. Edit `deps/dependencies.json` — add/update the `versions` entry and any
   `dependencies` edges.
2. If new, create `deps/<name>.cmake` following the manifest-driven pattern
   (`GIT_REPOSITORY ${DEP_URL_<name>}`, `GIT_TAG ${DEP_REF_<name>}`, clone into
   `${SNEEZE_DEP_REPO}/${DEP_FOLDER_<name>}`) and add it to `SNEEZE_DEPS` in
   `deps/CMakeLists.txt`. CI tier placement is automatic from the graph.
3. Run `python tools/DepGraph/depgraph.py validate` and
   `python scripts/ci-tier-matrix.py` to confirm the layer assignment.

## Encoding note

On Windows, the editor has intermittently written new files as UTF-16LE, which
breaks CMake/JSON/shell parsing. After writing or editing any file in this
system, byte-verify it is UTF-8 (no NUL bytes) and that `.sh` files use LF line
endings; re-encode via PowerShell `System.Text.UTF8Encoding($false)` if not.
