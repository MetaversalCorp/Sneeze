# Sneeze CI

Per-platform build matrix for `linux`, `macos`, `ios`, `android`.
Each platform builds all 11 deps in parallel tiers, then Sneeze itself.

## Workflows

- **`build.yml`** — orchestrator, one job per platform, calls the reusable workflow
- **`build-platform.yml`** — reusable workflow called per platform; runs the tiered dep build
- **`docs.yml`** — documentation drift check for `docs/` still in this repo (warn-only)

## Documentation

Curated wiki source and live publish moved to **[MetaversalCorp/SneezeDoc](https://github.com/MetaversalCorp/SneezeDoc)**.
That repo owns `docs/**/*.md`, `scripts/publish-wiki.py`, and `.github/workflows/deploy-wiki.yml`
(omb.wiki `/sneeze/...`).

### `docs.yml` (Sneeze — drift check only)

| Job | When | What |
|-----|------|------|
| `docdrift` | PR + push + dispatch | `tools/DocDrift/docdrift.py` (warn-only) |

### Sneezedoc wiki CI

| Workflow | When | What |
|----------|------|------|
| `docs.yml` | PR + push | `publish-wiki.py --dry-run --all` |
| `deploy-wiki.yml` | Push to `main` when `docs/` changes | `publish-wiki.py --all --skip-render` |

**Secrets** (SneezeDoc repo → Settings → Actions): `WIKIJS_API_TOKEN` (`write:pages` + `read:pages`).
Optional: `WIKIJS_GRAPHQL_URL`, `CF_ACCESS_CLIENT_ID`, `CF_ACCESS_CLIENT_SECRET`.

**Cloudflare 403 / error 1010:** If publish fails with `HTTP 403: error code: 1010`, Cloudflare
is blocking GitHub Actions before Wiki.js sees the request. The wiki admin must add a WAF rule
to skip bot checks for `POST /graphql`, or issue a Cloudflare Access service token and store it
in the optional secrets above. This is not a Wiki.js token permissions problem.

**Wiki.js `Forbidden` + `INTERNAL_SERVER_ERROR` on create/update:** This is a permission denial,
not a server fault. Wiki.js `@auth` throws `Error('Forbidden')`, which GraphQL surfaces as
`extensions.code: INTERNAL_SERVER_ERROR`. The mutation never runs. `pages.list` can succeed with
only `read:pages`; create/update need `write:pages`, `manage:pages`, or `manage:system`.

**Full Access API keys are not a special bypass.** Wiki.js signs them with `grp: 1` and loads
the Administrators group's `permissions` array from the database into `req.user.permissions`.
If group id 1 is missing `write:pages` / `manage:system` (or the groups cache is stale after key
creation), Full Access tokens fail create/update even though the UI label says Full Access.
Restart Wiki.js after API key or group changes.

**Wiki.js v2 `6013` on `singleByPath`:** Wiki.js 2.x incorrectly requires `manage:pages` and
`delete:pages` in the `singleByPath` / `single` resolvers (see
[requarks/wiki#3205](https://github.com/requarks/wiki/issues/3205)). `pages.list` works with
`read:pages` only. `publish-wiki.py` indexes paths from `pages.list` for upsert lookups. Create
and update still require `write:pages` on the API group and a matching page rule under `/sneeze`.

Config: Sneezedoc `docs/wiki/publish.json`. Script: Sneezedoc `scripts/publish-wiki.py`.

## Dependency tiers

Deps don't all build in parallel — some depend on others:

```
tier0 (parallel):  spirv-headers, anari-sdk, openxr-sdk, openssl, curl,
                   rmlui, nlohmann-json, wasmtime, filament
tier1:             spirv-tools (needs spirv-headers)
                   halogen     (needs anari-sdk + filament)
tier2:             glslang     (needs spirv-tools)
sneeze:            needs tier0 + tier1 + tier2
```

Each dep = one matrix job so failures are individually visible in the GH UI.

## Per-dep CMake files

Each dep is defined in `deps/<dep>.cmake` as a single `ExternalProject_Add`.
`deps/CMakeLists.txt` is the CI entry point: `cmake -S deps -DDEP=<dep> -DLIBS_DIR=<path>`
configures and builds only that one dep. The user-facing root `CMakeLists.txt`
still builds everything (SuperBuild workflow unchanged).

## Sneeze lib build

The `sneeze` job uses `cmake -S src` (NOT the SuperBuild) to build directly
against pre-built deps. Faster — no ExternalProject overhead, no dep update
steps. `src/CMakeLists.txt` uses find modules in `src/cmake/` to locate each
dep under `LIBS_DIR`.

## Caching

Each dep is cached per-platform. Cache key:
`<platform>-<dep>-<hash of CMakeLists.txt + deps/<dep>.cmake + deps/CMakeLists.txt>`

Cache hit → no rebuild. Filament (30+ min) benefits most.

## Artifacts

Each tier0/1/2 job uploads its dep's install dir as artifact
`<platform>-<dep>`. Downstream tiers + sneeze job download all tier0-2
artifacts and arrange them into `libs-<platform>/` for the build.

## Cross-platform specifics

- **Filament host tools** — Android/iOS need matc/resgen etc. from the native
  host build. The Android job waits for Linux tier0 filament to finish and
  downloads its artifact as `filament-host`. iOS likewise depends on macOS.
- **Dir name case** — ExternalProject dirs use mixed case (SPIRV-Headers) to
  match upstream repos, but matrix `dep` names are lowercase (spirv-headers).
  A resolve step in `build-platform.yml` maps target names → dir names for
  cache paths.
- **Toolchain path** — `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-...` is
  relative to repo root. The sneeze job (`cmake -S src`) rewrites it to
  absolute before passing to cmake.
