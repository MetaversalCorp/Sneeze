# Session Log — Dean Abramson

## 2026-05-27 (Wed) ~6:50 PM – 7:25 PM PDT

- Completed storage rename: `STORAGE::SASSET` -> `STORAGE::UNIT` across project.mdc (all storage-related ASSET references updated to UNIT, network ASSET references preserved)
- Merged orphan `Stream.h` into private `Console.h` — discovered `Stream.h` was never included by anything (public `include/Console.h` already had the full `CONSOLE::STREAM` declaration); removed the redundant copy from private `Console.h` after merge
- Merged `Network_Asset.h` into `Network.h` — `NASSET` class moved alongside `INETWORK_IMPL`; updated includes in `Network_Asset.cpp`, `Network.cpp`, `File.cpp`; carried over `Control.h` include; removed from MSVC project files; deleted `Network_Asset.h`
- Dean renamed `NASSET` back to `ASSET` after the merge (done manually)
- Ran full test suite: 10/10 suites, 295/295 assertions all passing
- Dean committed all changes
- Updated `project.mdc`: console descriptions updated for `m_umpStream` (was `m_apStream`), removed stale `m_umpBlock`/`Block_Open`/`Block_Close` references, noted single-stream-per-CID enforcement, updated file listings (removed `Block.h`/`Stream.h`), added private header notes for Network and Console
- Console module declared ~90% complete; next step is Inspector work in the host application

## 2026-06-02 (Mon) ~afternoon PDT

- **Phase 1 of scene bootstrap re-architecture: SCENE ownership refactor**
- Planned multi-phase architecture for MSF-driven scene bootstrapping (9 phases total)
- Discussed and resolved CONTEXT/CONTAINER/FABRIC/MSF/WASM relationships: each attachment point spawns one fabric, identified by one MSF file, housed in one container (keyed by fingerprint + container name)
- Moved SCENE ownership from VIEWPORT to CONTEXT — SCENE is now `SNEEZE::SCENE` (un-nested from VIEWPORT), created/destroyed by CONTEXT::Impl alongside CONSOLE, NETWORK, STORAGE
- Created `include/Scene.h` as the new public header for SCENE
- Un-nested FABRIC (`SCENE::FABRIC`) and NODE (`SCENE::FABRIC::NODE`) from VIEWPORT
- Updated VIEWPORT to delegate `Scene()` to `Context()->Scene()`
- Performed large-scale file reorganization: moved `viewport/scene/`, `viewport/msf/`, `sneeze/console/`, `sneeze/network/`, `sneeze/storage/`, and `Context.cpp` into new `src/context/` directory using `git mv`
- Updated CMakeLists.txt, MSVC project files (.vcxproj, .vcxproj.filters), and build scripts for new paths
- Fixed broken includes after moves (Network.h relative path, SneezeTest.vcxproj include directories)
- Updated subsystem documentation (Scene.md, Console.md, Storage.md) for new file paths
- Updated project.mdc for new directory structure and SCENE ownership model
- All tests passing, solar system renders correctly

## June 3, 2026 — ~5:00 PM – 6:10 PM PDT

**Phase 2 cleanup: pImpl for NODE and FABRIC, un-nesting into public header**

- Implemented pImpl for NODE: moved all internal members and NETWORK::IFILE inheritance into NODE::Impl in Node.cpp. Removed m_umpNode (unordered_map keyed by twObjectIx — unreliable at construction time), simplified to linear vector only. Removed unused public methods (Node_Children, Node_Find, ObjectIx_Set, IsPrimary, SetPrimary)
- Implemented pImpl for FABRIC: moved all internal members into FABRIC::Impl in Fabric.cpp
- Fixed crash in NODE construction: m_pImpl was uninitialized when Node_Add was called from Impl's constructor. Fix: moved parent-child linking from Impl constructor to NODE constructor body
- Two-step NODE construction pattern: constructor links into tree, Initialize(MAP_OBJECT*) assigns 3D payload
- Un-nested NODE from FABRIC: moved from SCENE::FABRIC::NODE to top-level SNEEZE::NODE in include/Scene.h. Deleted src/context/scene/Node.h. Updated all SCENE::FABRIC::NODE references across codebase
- Un-nested FABRIC and FABRIC_ROOT from SCENE: moved from SCENE::FABRIC to top-level SNEEZE::FABRIC in include/Scene.h. Deleted src/context/scene/Fabric.h. Updated all SCENE::FABRIC / SCENE::FABRIC_ROOT references across codebase
- All four scene classes (SCENE, FABRIC, FABRIC_ROOT, NODE) are now peers in the SNEEZE namespace, all declared in include/Scene.h
- Dean renamed NETWORK::IENUM to IENUM_FILE
- Updated project.mdc with new architecture
- Builds and runs correctly

## June 3, 2026 — ~7:00 PM – 12:00 AM PDT

**Phase 3 groundwork: MSF refactoring, CID overhaul, Container identity model**

- Relocated MSF class from VIEWPORT::MSF to top-level SNEEZE::MSF in new include/Msf.h public header
- Renamed CertChain.cpp to Chain.cpp, updated class from CERT_CHAIN to MSF::CHAIN
- MSF now accepts both JWS compact serialization (signed) and plain JSON (unsigned) via heuristic detection
- Implemented 5-level trust enum (eTRUST): kTRUST_NONE (black), kTRUST_UNTRUSTED (red), kTRUST_UNVERIFIED (orange), kTRUST_EXPIRED (yellow), kTRUST_VERIFIED (green)
- Refactored CID struct: sCommonName -> sOrganizationHash, sContainerName -> sContainer, bValidated -> eTrust
- Organization now extracted from X.509 cert O field (not MSF payload); hashed to sOrganizationHash via CHAIN::HashString
- Removed "namespace" and "organization" as MSF payload fields; added "container" field
- Eliminated CID_Pool / m_umCID from CONTEXT — replaced with CONTAINER as identity registry (Option B)
- Containers now persist at refcount 0 as lightweight identity shells, deleted only on CONTEXT destruction
- CONTAINER::Identity() returns const CID& as the single authoritative CID source
- Added diagnostic log in CONTAINER::~Impl() for nonzero refcount at destruction
- Updated CID field references across: Node.cpp, Stream.cpp, Silo.cpp, File.cpp, Unit.cpp, ConsoleTest.cpp, StorageTest.cpp, NetworkTest.cpp
- Fixed NetworkTest.cpp: replaced aggregate initialization with factory function (CID has user-defined constructor, non-aggregate in C++17)
- Synced msvc/Sneeze.vcxproj: CertChain.cpp -> Chain.cpp, removed stale Node.h/Fabric.h, updated Msf.h path
- Removed stale Node.h and Fabric.h from src/CMakeLists.txt SOM_HEADERS
- Updated subsystem documentation (Console.md, Storage.md, Network.md) for new CID fields
- Builds and runs (both Sneeze and the host application)
- Planned next step: create first official MSF file via SignMsf, then wire fetch/parse/verify pipeline in Fabric::Initialize

## June 3–4, 2026 — ~late evening through June 4 ~12:30 PM PDT

**Phase 3 completion: first official MSF file + async fetch/parse/verify pipeline**

- Created first official MSF file: `tests/data/solar-system.json` (payload) + `tests/data/solar-system.msf` (signed JWS via SignMsf with test certs, container name "solar-system", empty services/modules)
- MSF hosted at `https://cdn.rp1.com/test/solar-system.msf` for live testing
- Wired async MSF loading pipeline in FABRIC::Impl:
  - FABRIC::Impl now implements NETWORK::IFILE (OnFileReady/OnFileFailed)
  - Added MSF*, NETWORK::FILE*, CONTAINER* members to FABRIC::Impl
  - Initialize(sUrl) fetches MSF via NETWORK::File_Open using parent fabric's Container()->Identity() as CID
  - OnFileReady: reads data, parses MSF, verifies signature+chain, calls Container_Open to create container
  - astro::InjectSolarSystem moved from FABRIC_ROOT::Initialize to FABRIC::Impl::OnFileReady (conditional on MSF "container" == "solar-system")
- Resolved CID chicken-and-egg problem: FABRIC_ROOT gets synthetic root CID (sFingerprint="Sneeze", sContainer="Root", eTrust=kTRUST_ROOT) via Container_Open detecting null Msf(); child fabrics borrow parent's CID for MSF fetch caching
- Added kTRUST_ROOT to eTRUST enum (6 levels now)
- Changed Container_Open/Close signatures from void* to FABRIC*; added FABRIC forward declaration in Context.h
- Changed CONTAINER::Identity() from const CID& to const CID* (pointer identity required for deduplication)
- Changed all NETWORK::File_Open overloads + FILE constructors to accept const CONTAINER::CID* (const-correctness)
- FABRIC::~Impl destructor: close m_pFile_Msf, delete m_pNode_Root, Container_Close, delete m_pMsf, detach from parent
- Fixed build script: PCH clearing in build-windows.ps1 now conditional on -Fresh or -Rebuild only (was causing full rebuilds on every invocation)
- End-to-end verified: MSF fetched from CDN, parsed, cert data extracted, container created with correct CID and kTRUST_UNVERIFIED
- Discovered bug: inspector attachment re-triggers OnFileReady after Close() due to m_pListener not being nulled in Pending_Close() — Dean studying and fixing
- Phase 3 declared complete; next steps: fix OnFileReady bug, polish Phases 1-4, then Phase 5+6 (WASM solar system)

## June 4, 2026 — ~7:30 PM – ~7:45 PM PDT

**Phase 5 WASM infrastructure cleanup + WasmRuntime symmetry refactor**

- Eliminated `DEP::STORE_IDENTITY` struct — redundant with CONTAINER's uniqueness enforcement
- Simplified `WASM_RUNTIME` API: replaced `FindOrCreateStore()`/`DestroyStore()` with `Store_Open()`/`Store_Close(WASM_STORE*)`; internal storage changed from identity-keyed map to simple `vector<WASM_STORE*>` + mutex
- Added `WasmRuntime()` accessor to `CONTEXT` (pass-through to `Engine()->WasmRuntime()`) — symmetry with `Console()`, `Network()`, `Storage()`, etc.
- Updated `CONTAINER::Open()`/`Close()` to use `m_pContext->WasmRuntime()->Store_Open/Close()` instead of reaching through Engine
- Fixed outdated Storage test assertions: `containerName` -> `container` (field rename), `m_nCreatedCount == 1` -> `>= 1` (root container now creates its own silo during Context_Open)
- All tests pass: 44/44 storage, 40/40 WASM, full suite green
- Host application verified working

## June 5, 2026 — ~11:30 AM – ~11:55 AM PDT

**WASM pipeline test data + Node.cpp CID blocker fix**

- Created `tests/data/hello-world.json` — MSF payload referencing `hello_wasm.wasm` at `https://cdn.rp1.com/test/hello_wasm.wasm` with SHA-256 hash `c816eb074d864b9a9637213b613536b5447f982573513f6cf7d0f76485bba77a`
- Signed payload with `SignMsf.exe` → `tests/data/hello-world.msf` (3,909 bytes, RS256, test provider cert chain)
- Verified round-trip: signature verified, fingerprint matches (`482cda4c...`), payload round-trips cleanly — first MSF with a hashed WASM module
- Fixed blocker in `Node.cpp`: `Texture_Request()` was fabricating a fake CID with hardcoded fingerprint/org/container; replaced with `m_pFabric->Container()->Identity()` — the real CID from the fabric's container
- Added `m_pFabric->Container()` null guard (no texture fetch when fabric has no container yet — protects solar system kludge path)
- Removed all fake CID code, persona hash lookup, and ENGINE/Persona references from Node.cpp
- Committed for Dave at session end

## June 5-6, 2026 — ~evening – ~12:22 AM PDT

**WASM scene bootstrap — end-to-end milestone achieved**

- Replaced `m_apNode` (`std::vector<NODE*>`) with `m_umpNode` (`std::unordered_map<uint64_t, NODE*>`) in Container.cpp — vector was catastrophic for sparse 48-bit object indices
- Added `m_umpFabric` (`unordered_map<uint64_t, FABRIC*>`) for fabric handle management with `m_twFabricIx_Next` monotonic counter
- Implemented `Node_Root`, `Node_Open`, `Node_Close`, `Node_Find` on CONTAINER — full handle table with RMCOBJECT wire-format deserialization
- Moved mutex guard from internal `Node_Create` to public entry points (`Node_Root`, `Node_Open`)
- Introduced `using WASM_HOST_FN` alias in HostFunctions.h — reduced 32 host function declarations to one-liners
- Host function count grew from 29 to 32 (Scene module: Node_Root, Node_Open, Node_Close, Node_Position, Node_Scale, Node_Bound, Node_Color, Node_Name, Node_Radius, Node_Texture)
- Implemented `Scene_Node_Root` and `Scene_Node_Open` host functions — read RMCOBJECT from WASM linear memory, create MAP_OBJECT_CELESTIAL + NODE, return twObjectIx handle
- Consolidated test files: `scene-test.*` and `hello-world.*` merged into `solar-system.*` (single MSF + single WASM module)
- Created Rust WASM module `tests/wasm/solar_system/` — calls Scene host functions to create Root/Sun/Earth/Moon nodes
- Fixed Rust compilation issues: removed semicolons after return expressions (Rust expression-oriented semantics), added `#![allow(dead_code)]`
- Fixed `WASM_INSTANCE::CallOpen` — was passing 4 args (leftover from hello_wasm), corrected to 3 (i64 twFabricIx, i32 dwOffset, i32 dwLength)
- Built and signed solar-system.msf with solar_system.wasm module reference + SHA-256 hash
- Commented out `astro::InjectSolarSystem()` to let WASM module populate the scene
- **End-to-end success**: MSF fetched -> parsed -> WASM compiled -> instantiated -> Init called -> Open called -> 4 scene nodes created via host functions -> rendered as spheres in viewport
- Vertically aligned Scene.h class declarations, grouped by accessors/mutators/methods
- Updated project.mdc with all architectural changes

## June 6, 2026 — ~afternoon PDT

**WASM full solar system — 1,245 objects, hierarchical rendering, textures**

- Created `gen_solar_system.py` — Python generator that parses the JavaScript Solar System project data (`E:\Dev\SolarSystem\`), computes MSF orbital parameters (quaternions, semi-axes, periods, precession vectors, tilt, spin), and generates 10 Rust source files for the WASM module
- Generated Rust data files: `star.rs`, `planets.rs`, `moons_{earth,mars,jupiter,saturn,uranus,neptune,pluto}.rs`, `debris.rs` — 1,245 total objects in 3-level hierarchy (System→Body→Surface)
- RMCOBJECT wire format fields fully utilized: `m_Orbit` (dA/dB/tmPeriod/tmOrigin for orbital parameters and surface spin), `m_Transform.d4Rotation` (tilt quaternion on bodies, orbital quaternion on systems), `m_Transform.d3Position` (precession axis-angle rate vector), `m_Resource.sReference` (texture URLs)
- Implemented `MAP_OBJECT_CELESTIAL::Rotation()` override with three code paths: BODY subtypes (tilt quaternion + precession composition), SURFACE subtypes (spin around Y-axis using dA=W0Rad and tmPeriod=spin period), SYSTEM subtypes (identity)
- Refactored `Compositor.cpp::TraverseNode` to use `WORLD_FRAME` struct for hierarchical world-space accumulation — SYSTEM nodes compute orbital position and propagate to children, BODY nodes capture radius/color/star-flag, only SURFACE nodes render as spheres (using inherited parent attributes)
- Fixed texture CDN URL: changed `TEX_BASE` from `cdn2-david.rp1.dev/img/` to `cdn.rp1.com/res/texture/celestial/` — resolved HTTP 404 errors
- Fixed Python generator to output single-line Rust function calls (no line splitting)
- Performance discovery: all 1,245 objects cause 1-2 FPS; selectively disabling Jupiter (79 moons) and Saturn (83 moons) restores 60 FPS — bottleneck is in the renderer
- Current configuration: star + planets + Earth/Mars/Uranus/Neptune/Pluto moons + debris = 60 FPS with textures and orbit trails
- `astro::InjectSolarSystem()` fully superseded by WASM module — pending removal after validation
- Updated project.mdc with all architectural changes

## June 7, 2026 — ~late evening PDT (June 6 session continued)

**Console bSystem flag, dual logging, un-nesting Console/Storage/Network types**

- Added `m_bSystem` (bool) to `ENTRY` — distinguishes browser-injected system entries from container-generated entries; always serialized to JSONL as `"system"` key
- Added `bSystem = false` default parameter to `STREAM` logging methods (Log/Debug/Info/Warn/Error)
- Console tests pass without modification (44/44)
- Un-nested `eLEVEL`, `ENTRY`, `STREAM`, `IENUM_ENTRY`, `IENUM_STREAM` from `CONSOLE` into `SNEEZE` namespace — resolved circular header dependency preventing `Container.h` from forward-declaring `STREAM`
- Renamed `eLEVEL` to `eENTRY_LEVEL` with constants `kENTRY_LEVEL_DEBUG`, etc.
- Un-nested `eSCOPE`, `SILO`, `IENUM_SILO` from `STORAGE` into `SNEEZE` namespace
- Renamed `eSCOPE` to `eSILO_SCOPE` with constants `kSILO_SCOPE_PERMANENT_ORG`, etc.
- Un-nested `STATE`, `DISKFILE`, `FILE`, `IFILE`, `IENUM_FILE` from `NETWORK` into `SNEEZE` namespace
- Renamed `STATE` to `eASSET_STATE` with constants `kASSET_STATE_IDLE`, etc.
- Renamed `DISKFILE` to `eASSET_EXT` with constants `kASSET_EXT_DATA`, `kASSET_EXT_TEMP`, `kASSET_EXT_META`
- Kept `FILE` as `FILE` (not `NETFILE`) — qualify as `SNEEZE::FILE` where ambiguity with C's `::FILE` arises
- Added `STREAM* Stream()` accessor to `CONTAINER` (forward-declared in `Container.h`)
- Implemented dual logging in `Fabric.cpp` — MSF/WASM lifecycle events logged to both `Engine()->Log()` and `Container->Stream()->Info/Error(msg, true)` with `bSystem = true`; pre-container errors log to parent fabric's container stream
- Removed obsolete `Test-SourceHeaderSync` function from `build-windows.ps1`
- All console (44/44) and storage (44/44) tests pass; network tests pass for non-external cases
- Updated project.mdc with all type un-nesting and enum rename changes

## June 7, 2026 — ~morning PDT

**Deleted astro module and unused scene files, fixed build errors, orbit trail analysis**

- Fixed `CID::Key()` crash on empty/short strings — moved `DisplayName()` and `Key()` from `Container.h` to `Container.cpp`, added `substr` guards
- Implemented synthetic fingerprint for plain JSON MSF files — `SHA-256(url + content)` for `sFingerprint`, `SHA-256(url)` for `sOrganizationHash`
- Made `MSF::Parse` URL parameter required (removed default)
- Deleted entire `src/astro/` folder (BodyData.h/cpp, RMCObject.h/cpp) — WASM module is sole scene population path
- Deleted `Orbit.h`/`Orbit.cpp` — moved `ORBIT_POSITION` struct to `MapObject.h`, `SolveKepler` to `MapObject.cpp` (file-local static)
- Deleted `Celestial.h`/`Celestial.cpp` — moved `QuatMultiply` and `RotateByQuat` to `MapObject.cpp` (file-local statics)
- Deleted `Epoch.h`/`Epoch.cpp` — unused after WASM pipeline replaced astro injection
- Deleted `Events.h`/`Events.cpp` from build (dead code — never instantiated)
- Deleted `SpatialIndex.h`/`SpatialIndex.cpp` from build (dead code — never queried)
- Fixed build errors: added `#include "sneeze/Types.h"` to `MapObject.h`, replaced `CELESTIAL::` qualifications with local calls in `MapObject.cpp`
- Cleaned up `CMakeLists.txt`, `Sneeze.vcxproj`, `Sneeze.vcxproj.filters` — removed all 10 deleted files
- Analyzed compositor orbit trail rendering: identified ANARI `"curve"` geometry as source of visual quality issues (thick PBR-lit tubes vs. desired thin screen-space lines) and major performance bottleneck (~80-90% of 1 FPS drop with all moons)
- Decided to prioritize visual quality improvements (unlit/emissive material, camera-distance radius scaling) before performance optimizations
- Updated project.mdc with all deletions, architectural changes, and orbit trail analysis

## June 7, 2026 — ~evening PDT

**Fabric refactoring finalization, mutex, MSF ownership, fabric ownership modes**

- Continued fabric loading refactoring from earlier session (SCENE orchestrates fabric lifecycle)
- Added `m_mxScene` (recursive_mutex) to `SCENE::Impl` — guards `Fabric_Close`, `Fabric_Find`, and the critical section in `OnMsfReady` (index increment + fabric creation + map insert). Removed unnecessary guard from `Fabric_Open` (no shared state mutation — only kicks off async MSF fetch). Dean further localized the guard in `OnMsfReady` to wrap only the three mutation lines.
- Moved MSF ownership from FABRIC to SCENE — `SCENE::OnMsfReady` creates the `MSF*`, `SCENE::Fabric_Close` deletes it. FABRIC holds a non-owning pointer. Symmetric with creation order: MSF → Container → Fabric, teardown: Fabric → Container → MSF.
- Removed defensive null checks for `m_pContainer` in Fabric.cpp (dead code — FABRIC is always constructed with a valid container)
- Removed `if (m_pFabric_Attachment) Fabric_Close()` guard from `NODE::Impl::Fabric_Open` — URL changes would be handled by explicit close-then-open, not implicit close inside open
- Renamed `m_mutex` to `m_mxNetwork` in Network.cpp (coding standards)
- Discussed and documented fabric ownership modes (WASM-managed vs map-managed) and the duplicate index problem when same MSF is loaded into multiple fabrics sharing one container
- Added "Fabric Ownership Modes" section to Scene.md with full discussion
- Added REVISIT comment in Container.cpp at Scene Node Handle Table section
- All 10 test suites pass (312 assertions, 0 failures), host application runs clean
- Updated project.mdc with fabric lifecycle, ownership modes, and duplicate index problem

## June 8, 2026 — Dean Abramson (morning session)

- Replaced the `Asset_Lock`/`Asset_Unlock` deadlock solution with a lightweight per-FILE atomic guard flag (`m_bGuarded`)
- Consolidated the two `Fetch_Complete` overloads into a single function (carried over from prior session)
- New deadlock avoidance: `FILE::Guard(bool)` uses `std::atomic<bool>::exchange` to arm/disarm a guard flag; `NETWORK::Impl::File_Close` checks the guard before acquiring `m_mxNetwork` — if guarded, the entire close is deferred
- `ASSET::Impl::Fetch_Complete` arms guards before the notification loop, collects deferred closes after the loop, processes them after releasing `m_mxAsset`
- Removed all `Asset_Lock`/`Asset_Unlock`/`Fetch_Lock`/`Fetch_Unlock` code from Asset.cpp, Network.cpp, and Network.h (both public and private headers)
- Updated Network.md thread safety documentation with the guard flag mechanism, replacing the old lock-ordering section

## June 9, 2026 — Dean Abramson

**SCENE navigation, WASM host wiring, container callbacks, renderer scene invalidation**

- Implemented `SCENE::Url(sUrl)` (swap root fabric) and `SCENE::Reload(bReset)` (re-issue with current URL); FABRIC records its URL at `Initialize()`, so the scene reads the current URL from `Fabric_Root()->Url()` rather than storing a duplicate
- Wired all Storage WASM host functions (`Storage_Get/Set/Remove/Has/GetJson/SetJson`) to forward to the calling container's `Silo()`; re-verified Console host functions forward to `Stream()`
- Storage ABI: scope selector + dot/bracket path keys + JSON values both directions; added `WriteWasmString` helper that returns the full size needed (callers detect truncation; length 0 queries size)
- Added `SILO* CONTAINER::Silo()` accessor (with `class SILO;` forward decl in Container.h)
- Replaced `ICONTEXT::OnConsoleStreamCreated/Deleted` with `OnContainerCreated/Deleted`, fired from `CONTAINER::Open/Close` at the 0→1 / 1→0 refcount transitions (guarded by `m_bNotified_Open` so delete only fires if create did)
- Fixed ANARI renderer retaining old geometry: `BuildScene()` now unsets the world `"instance"` parameter when there are no instances (empty-scene transition clears the screen)
- Added explicit scene-invalidation path for non-structural scene swaps: `RENDERER::InvalidateScene()` (dirty flag → full rebuild in `EndFrame()`), `VIEWPORT::Scene_Invalidate()/Scene_Invalidate_Consume()` (atomic, cross-thread), compositor consumes it in `Execute_Render`, `SCENE::Url()` triggers it
- Updated docs: Wasm.md (Console→STREAM / Storage→SILO forwarding, storage ABI, WriteWasmString), Viewport.md (empty-scene clear + invalidation), Scene.md (Url/Reload navigation), Console.md and Sneeze.md (container lifecycle callbacks)

## June 9, 2026 — Dean Abramson (late night)

**FABRIC_ROOT removal, node handle table moved to SCENE, review pass**

- Removed the `FABRIC_ROOT` class entirely; SCENE now owns a plain root FABRIC plus `m_pNode_Primary` directly, building the root node and primary attachment node in `Fabric_Root_Create`
- Moved the scene node handle table (`Node_Root` / `Node_Open` / `Node_Close` / `Node_Find` and members `m_umpNode` / `m_apMap_Object` / `m_twObjectIx_Next`) from CONTAINER to SCENE — node indices are now scene-global; updated Container.h/.cpp and Scene.h/.cpp
- Dropped the `pFabric->Container() == m_pContainer` ownership check in `Node_Root` (no longer a per-container table); read/write enforcement moves up to the WASM host-function layer
- Reviewed Scene.cpp/Fabric.cpp/Node.cpp/Context.cpp/Container.cpp for lurking issues — the node-destruction cascade is clean (no dangling map handles, no double-free)
- Fixed a dangling `const&` in `Reload`: it passed `m_pFabric_Root->Url()` (a reference into the fabric) to `Url()`, which then destroys that fabric before reading the string — now copies the URL into a local first
- Confirmed deferred (logged in Scene.md "Known Limitations"): async MSF-fetch cancellation, compositor-traversal safety during `Url()`/`Reload()` teardown, and a future scene-revision-counter to unify rebuild-detection with traversal safety
- Confirmed intentional/temporary: the `CID.eTrust = kTRUST_EXPIRED` override in `Container_Open` (no trusted cert yet)
- Updated Scene.md to current state (node table on SCENE, scene-global indices, access-check responsibility, Known Limitations)

## June 12, 2026 — Dean Abramson (night)

**Object class moved into OBJECTIX; celestial type moved into bType; node handle table keyed by composed value**

- Class (root/celestial/terrestrial/physical) no longer lives in `MAP_OBJECT_TYPE` — it now lives in the upper 16 bits of the `OBJECTIX` (`qwComposed`) on `Head.Self`/`Head.Parent`. `OBJECTIX::Class()` reads it; `OBJECTIX::ObjectIx()` is the low 48 bits. Added `OBJECTIX_COMPOSE(eClass, twObjectIx)` macro in MapObject.h
- Deleted the old `MAP_OBJECT_TYPE_TYPE__` enum; `MAP_OBJECT::Class()` derives class from `m_Head.Self.Class()`. Derived ctors (ROOT/CELESTIAL/TERRESTRIAL/PHYSICAL) now take an `OBJECT_HEAD` and forward it to the base
- `SCENE::Node_Create` switches on `Head.Self.Class()` to build the matching derived MAP_OBJECT; `m_umpNode` is now keyed by the full composed `qwComposed` (class + index), so parent lookups carry class; `NODE::ObjectIx()` returns the composed handle. (Dean refactored Node_Create to carry the result in `Head.Self.qwComposed` rather than a separate local)
- The scene's two built-in root-fabric nodes are now class `MAP_OBJECT_CLASS_ROOT`
- Second change same session: the values formerly in `bSubtype` (celestial type) moved into `bType`; `bSubtype__` is now the poisoned reserved byte. Renamed enum to `MAP_OBJECT_TYPE_TYPE_CELESTIAL_*`. Updated all reads (MapObject.cpp, Compositor.cpp) and the `== 255` attachment marker (Node.cpp, Scene.cpp)
- Rust producer (solar_system): removed the redundant index-1 ROOT node; "Solar System" (index 2, STARSYSTEM, CELESTIAL) is now created via `Node_Root` as the fabric root (star.rs line 5 commented out by Dean, count 3→2). Added `objectix_compose` helper + `MAP_OBJECT_CLASS_CELESTIAL`/celestial type consts (u8); pack class into Self/Parent; write type into `bType`, zero `bSubtype__`. wasm rebuilds clean; fresh `solar_system.wasm` ready for CDN
- C++ builds (Dean confirmed); produced a host-application hand-off note covering composed handles, the class enum, and the poisoned bytes
- Updated internal Scene.md (OBJECTIX/compose, composed-key handle table, Node_Create class switch, MAP_OBJECT class-derived model, bType celestial type). Wiki pages (docs/) deferred at Dean's request

## June 13, 2026 (Sat) ~9:37 AM – 11:02 AM PDT — Dean Abramson

**Node table moved to CONTAINER (per-container identity); attachment marker moved to bSubtype; star-driven lighting; zoom; multi-fabric composition fix**

- **Node handle table moved from SCENE back to CONTAINER** (reversing the June 9 late-night move). Dean did the bulk of the move; this session resolved the call sites. Rationale: node identity is unique per container, not scene-global. `Node_Root/Open/Close/Find` are now public on CONTAINER (backed by `m_umpNode` / `m_apMap_Object` / `m_twObjectIx_Next`, guarded by `m_mxContainer`). `CONTAINER::Node_Root` resolves the fabric via `m_pContext->Scene()->Fabric_Find`. Removed the dead `SCENE::Node_*` declarations/wrappers. Updated callers: HostFunctions.cpp (use `Container(pEnv)->Node_*`, incl. `Map_Open_Children`/`Scene_Node_Map`), `SCENE::Fabric_Root_Create` (via `m_pFabric_Root->Container()`), `Node.cpp` and `Fabric.cpp` destructors (via `Container()->Node_Close`)
- **Attachment marker is `bSubtype == 255` (not `bType`)** — already changed earlier; confirmed across Scene.cpp/Node.cpp/HostFunctions so an attachment point keeps a meaningful `bType` (e.g. `PLANETSYSTEM`) and can carry orbit data + render an orbit trail while spawning a child fabric
- **Star-driven lighting** (replaced the hardcoded point light at origin): added `LIGHT_DATA` + `RENDERER::SetLights(vector<LIGHT_DATA>)`. Compositor collects one light per `STAR` node at its world position (`TraverseNode` now takes `vector<LIGHT_DATA>&`). `BuildScene` makes one `"point"` light per star; if none, one `"ambient"` fallback. `SCENE_STATE` holds `vector<ANARILight> aLight`; rebuild triggers when light **count** changes. Diagnosed the earth-system-primary "moon glowing" bug as the origin light sitting inside the Earth (Dean diagnosed it himself)
- **Ambient fallback caveat (deferred):** the starless scene still shows a directional light — Halogen almost certainly ignores the ANARI `"ambient"` light/`"radiance"` param and supplies its own default. Dean is fine with it for now. Would swap to an explicit `"directional"` light to own it deterministically
- **Zoom:** lowered `MIN_DISTANCE` from `0.1` to `0.0001` AU in Viewport.cpp (Earth's rendered radius ~0.002 AU; near plane 0.0001) so the camera can approach a surface
- **Multi-fabric composition fix (the headline bug):** secondary fabric (earth-system attached under the solar system) loaded — nodes existed, textures loaded — but Earth/Moon didn't render. Cause: the compositor traversed a child's attachment with the **parent's** frame, not the child's accumulated frame. Worked one level deep only because the browser root attachment node is `ROOT` class (frame-neutral); the `earthsystem` attachment is `PLANETSYSTEM` and applies an orbital offset, so Earth/Moon were placed at the Sun (origin) and lost. Fix: each node traverses **its own** attachment at the end of its own `TraverseNode`, using its own `childFrame`. Result: "Touchdown! We have a multi-fabric scene!"
- Side fixes confirmed earlier in the broader effort and still in play: `CHAIN::HashString` now emits the full 64-char SHA-256 (was truncated to 12); `MsfFile::Parse` detects plain JSON (`{`/`[` after optional BOM/whitespace) before the JWS dot-heuristic; generic `map.wasm` (tests/wasm/map) calls `Scene.Node_Map` so the host injects nodes from the MSF `data` block; `Submit_Surface` writes an identity quaternion
- Deferred: empty persona hash in the container key (no persona logged in — host-application lifecycle concern, not a key-logic bug); true-proportion scaling (sqrt size compression makes the Moon ~half an Earth); the ambient-light caveat above
- Updated docs: Scene.md (node table now on CONTAINER, per-container identity, bSubtype attachment marker, threading split, duplicate-index scope narrowed, files table) and Viewport.md (SetLights/LIGHT_DATA lighting model + ambient caveat, zoom range). Wiki pages (docs/) not touched
- Dean to commit this morning's work himself

## June 13, 2026 (Sat) evening – ~10:50 PM PDT — Dean Abramson

**DFW import + display: universal TRS, per-scene render scale, box rendering, lighting, RMCOBJECT field expansion, data-format cleanup**

Goal for the day (Dean leaves for a conference tomorrow): (a) get DFW airport data importing correctly, then (b) get it to display. Both achieved — DFW now renders its buildings and its resource URLs load in the network panel.

- **RMCOBJECT field expansion (528 bytes).** `MAP_OBJECT_RESOURCE.sName` `char[32]→[64]`; `sReference` `char[64]→[128]`. Total RMCOBJECT `432→528`; updated `static_assert` and layout comments in MapObject.h and HostFunctions.cpp. Mirrored in the Rust producer (`tests/wasm/solar_system/src/lib.rs`): resource block `104→200`, struct size assert `432→528`, `Reference_Set` clamp `63→127`. wasm rebuilt clean (the `size_of==528` compile-time assert is the cross-language layout check). **Dean uploaded `solar_system.wasm` to `cdn.rp1.com/test/` and confirmed it works.** Payoff: DFW's long resource URLs are no longer clipped — all assets load in the network panel.
- **Universal TRS — all nodes, no exceptions.** `METERS_TO_AU` removed entirely. `WORLD_FRAME` now carries a `MAT4` (double, column-major, Types.h); every node from synthetic `ROOT` → bottom-most `PHYSICAL`, celestial included, composes full T·R·S and inherits its parent's matrix. Dean was emphatic and repeated this many times after the AI kept trying to argue celestial should be translation-only ("positions nest, orientations absolute") — that reasoning is wrong; the whole system is designed for all bodies to inherit parent TRS. No class-specific shortcuts.
- **Moon-inside-Earth bug.** Synthetic root nodes built in `Scene::Fabric_Root_Create` were `memset`-zeroed → `scale=(0,0,0)`, which collapsed every descendant to the origin under universal TRS (Moon sat inside Earth at the origin; Dean saw the Moon flash inside Earth and confirmed by zooming through the surface). Fix: added `RmcObject_Init` (zero-clear + identity transform: scale `(1,1,1)`, rotation `(0,0,0,1)`); applied at both synthesis sites.
- **Per-scene render scale (interim precision solution).** Discussed real-world 32-bit precision approaches at length; Dean has a patented superior technique (not used today) so we picked the simplest stopgap he proposed: one uniform factor per scene since individual scenes have bounded range. `dMaxReach` = root-anchored bounding sphere radius (`|world_pos| + body_radius` per node); `dRenderScale = TARGET_EXTENT(5.0) / dMaxReach`, computed once per build, applied at a single flatten seam to all sphere/curve/light/box coordinates. Bounding sphere anchored at the SOM root origin (Dean's call).
- **Body magnification.** `MagnifyRadius` = `BODY_MAG * (radius_render ^ BODY_EXP)`, currently `BODY_MAG=1.25`, `BODY_EXP=0.7` (sub-1 exponent so small planets grow faster than big ones — Earth/Jupiter visibly differ without the Sun ballooning). `MIN_SPHERE_RADIUS=0.0`. Dean tuned these himself to numbers he can live with after several iterations (the AI's earlier "1:1" and curve attempts weren't right; Dean noted "these numbers don't work the way you think").
- **Moon kludge (moon-only special case).** `MOON_ORBIT_BOOST=5.0` multiplies `MOONSYSTEM` local position + its orbit-trail points so the orbit clears the magnified planet; `MOON_SIZE_FACTOR=1.0`. Valid while the data has ≤ a handful of moons (next week: likely just Earth's Moon). Dean explicitly OK'd a kludge here; promised no mixing of planets with terrestrial/physical.
- **Orbit trails cut ~10×.** `TRAIL_RADIUS_PLANET=0.0002`, `TRAIL_RADIUS_MOON=0.00005` (moon/debris systems use the thin one).
- **Box rendering for terrestrial/physical.** Unit-cube mesh + per-box `MAT4` instance (mirrors textured-sphere instancing). **GLBs are NOT parsed in the compositor** — Dean was firm that picking apart GLB ourselves is never happening; whole-GLB handoff to Filament via Halogen is deferred and should go to someone else (it's below ANARI = "above my paygrade"). Boxes are the top-down interim. **Action nodes skipped:** physical nodes whose resource reference starts with `"action:"` (e.g. `action://collider`) are invisible. This also fixed the "DFW is one flat sheet" bug — a multi-km "Sector Floor Collider" was dominating `dMaxReach` and shrinking every real building to sub-pixel (diagnosed via temporary log lines Dean asked for, then removed).
- **Lighting.** Sun `"point"` light intensity tuned to `4.0` (was overbrightening everything to white). Starless scenes (DFW, or a planetary system loaded primary) now use `"ambient"` (radiance `3.0`) **plus** a `"directional"` key light from above-front (`{-0.4,-1.0,-0.3}`, irradiance `1.0` — Dean dropped it to 1.0 and likes it) because Filament's ambient alone is weak and Halogen likely ignores ANARI ambient radiance.
- **Camera less jumpy.** `MOUSE_SENSITIVITY` `0.005→0.0025`, `SCROLL_FACTOR` `1.1→1.075`, `MIN_DISTANCE` `0.001`. Note: the real fix for true 1:1 scale is click-to-target camera retargeting (like the old Three.js/WebXR solar-system model) — deferred, "not a tonight job."
- **Data-format cleanup + migration.** Wrote a Python converter `dfw.json → dfw.msf.json` in compact form: vectors on single lines, leaf nodes compact, strip redundant fields (all-zero Type, empty Resource, `Event:0`), round float noise (keep precision beyond ~e-10), human-readable IDs as `"class:index"` strings (e.g. `P-5039`, `T-…`). Migrated `earth-system.msf.json` and `solar-system.msf.json` to the same compact standard (lossless), kept in one container. `RmcObject_FromJson` updated to: default identity rotation + unit scale when omitted; parse `"class:index"` string IDs with backward-compat for composed `uint64` `Head.Self`.
- **Debugging discipline (correction).** The AI went down a wrong track (claimed spheres rendered as flat camera-aligned disks, started editing AnariRenderer.cpp). Dean stopped it hard — "no more debugging without me," textures were loading fine, spheres worked before. All AnariRenderer experiments reverted. Lesson: debug collaboratively, trust Dean's symptom reads.
- **Docs updated:** Control.md (new "Rendering pipeline / scaling" section — universal TRS, no-AU, per-scene render scale, magnification, moon kludge, trails, boxes, action-skip, lighting collection, with current constants) and Viewport.md (lighting fallback now ambient+directional with values; zoom range/units corrected from AU to per-scene render space; sensitivity/scroll values). Wiki pages (docs/) not touched.
- Deferred: click-to-target camera navigation (the proper path to 1:1 scale); whole-GLB → Filament via Halogen (assign out, below ANARI); the patented precision technique (future). Dean commits all of this himself.

## June 25–26, 2026 — Dean Abramson

**Network subsystem refactor — Phase 1 (mutex split) and Phase 2 (on-disk layout); JWS test crash fix**

Phase 1 (committed earlier in the effort):
- Split `NETWORK::Impl::m_mxNetwork` into three `mutable std::recursive_mutex`: `m_mxRules` (all `Rules_*`), `m_mxCache` (all `Cache_*`), `m_mxAsset` (all `Asset_*`). Caches and Assets are unrelated lists that happened to share a lock; separating them removes the lock-order concern. `Asset_Index` now takes `m_mxRules` (shares `m_nNextAssetIx` with `Rules_Load`/`Rules_Save`).
- Added a per-container `CACHE` handle (analogous to `SILO`/`STREAM`): `CACHE::Initialize`, `DisplayName`, `Path`/`Filename`/`Pathname`. Added `ICONTEXT::OnNetworkCacheCreated`/`OnNetworkCacheDeleted` callbacks.

Phase 2 (this commit) — on-disk folder restructure:
- New layout: `Root/<Durability>/<persona>/<fp2>/<fp22>/<container>/<Subsystem>/<data>`. Durability = `Persistent` or `Transitory/v<hex>`. Subsystem folders (`Network`, `Storage`, `Console`) nest under the container; org-scoped Storage sits one level up at the fingerprint (`<persona>/<fp2>/<fp22>/Storage`).
- `CONTAINER::CID::Key_Org()` (persona/fp2/fp22) + `Key_All()` (…/container, was `Key()`). New cached `CONTAINER` accessors: `Path_Permanent_All/Temporary_All` (triad+container) and `Path_Permanent_Org/Temporary_Org` (triad only). Every object now appends its segment to its parent's path — no object re-derives the full path.
- Filenames simplified now that data is container-scoped: Storage company stem `container` (was `container-<id>`); Console block `NNNN.log`, meta `console.meta`; `CACHE::Filename` is a placeholder dummy (`"container"`) — unused by engine code yet.
- Persona segment is never empty: `PERSONA::m_sHash` defaults to `000000000000` (12 zeros) in ctor and on `Logout()`.
- Folder-creation responsibility: ENGINE precreates roots (`Persistent`, `Transitory`, session `s<hex>`, context `v<hex>`); `CONTAINER::Open` eagerly creates its identity triad under both permanent and temporary roots but NOT subsystem-named folders; each handle (`CACHE`/`STREAM`/`SILO::Initialize`) creates its own subsystem dir; file-bearing objects (`ASSET`/`UNIT`/`BLOCK`) re-ensure their leaf dir (`parent_path`) at file-open-for-write. Precreate eagerly; recreate only on first file open (not every write).
- Removed dead path members/accessors: `INETWORK_IMPL`/`ICACHE_IMPL` `Path_Permanent`, `ISTORAGE_IMPL`/`ICONSOLE_IMPL` `Path_*`; `STORAGE`/`CONSOLE` no longer store a path member (they become singletons in Phase 3, so paths are derived on demand). `NETWORK` likewise derives its cache path in `Path_Cache()` from `pContext->Path_Permanent()`.
- `ASSET::Path(eType)` was misnamed (returned a full pathname) → renamed `Pathname(eType)`; new `ASSET::Path()` returns the directory. `ICACHE_IMPL::Path()` returns the cache dir.
- Variable rename pass: `sXxxxPath` → `sPath_Xxxx` (directories), `sYyyyPath` → `sPathname_Yyyy` (full paths w/ filename) across the touched .cpp files.

Verification:
- `--storage` and `--console` pass 44/44 each. Disk layout confirmed correct in the live app (Artemis): persona default `000000000000`, fp2/fp22 split, subsystem folders under container, org Storage at fingerprint level, `Persistent` + `Transitory/v<hex>` present. Empty `Transitory/s<hex>` explained: it is the per-session *permanent* root for incognito (transitory-session) contexts only; empty when browsing persistent, scrubbed on clean shutdown.

The rabbit-hole and the real bug:
- Spent the session chasing a "mutex destroyed while busy" crash by reasoning instead of reading the stack — wrong. Once Dean read the debugger, the abort was `JwsTest.cpp:320` indexing `reader.Modules()[0]` on an empty vector: when CWD isn't the repo root, `tests/certs/*.pem` don't load, every sign returns empty, the unguarded `[0]` aborts (exit `0x80000003`). Fixes: guarded the composition section like the parsing section (`if (!empty())`), and baked the certs dir in at build time via `SNEEZE_TEST_CERTS_DIR` (CMake `target_compile_definitions`, default `tests/certs`, overridable by `argv[1]`) so `--jws` passes from any CWD. NOTE: the CMake-generated `Sneeze.sln` gets the define; the hand-maintained `msvc/` project does not (run sync-build if building tests there).
- Dean's drain fix in `~NETWORK::Impl`: wrap the `m_umpAsset.size()` poll in `m_mxAsset`. A genuine data-race fix (concurrent read of an `unordered_map` being `erase`d), but NOT the cause of the test crash — latent, possibly never fired.

Deferred:
- `NetworkTest` cache-reload cluster: 6 tests fail deterministically (e.g. "Meta survived shutdown", "Hash matches after reload", "Second fetch IS served from cache"). Files DO land on disk at the correct new paths and reload works in the live app, so prime suspect is the two-`NETWORK`-instance test harness (reusing one `CONTEXT`/`CONTAINER`), not the engine. Breadcrumbs: disk key = `SHA1(url)[0:12]` (`File.cpp:219`), asset pathname = `pFile->Pathname("")` (`Network.cpp:337`), reload load in `ASSET::Meta_Load` (`Asset.cpp:98`) marks READY only if `jMeta["url"]==m_sUrl && exists(data)` (`Asset.cpp:122`); check the test before the engine.
- Phase 3 (end of): relocate `rules.json` from the shared context-level `Network/` folder into the primary fabric's container; simplify rules to a single time-based staleness watermark stored in the primary fabric's container and applied to the context's network layer at load ("clear cache" scopes to the primary fabric's container only, web-consistent, does not propagate to subsidiary containers). Subsystems (`NETWORK`/`STORAGE`/`CONSOLE`) return to singleton status.
- Dean commits Phase 2 himself.

## June 26, 2026 (afternoon) — Dean Abramson

**Network/Console/Storage refactor — Phase 3 (engine singletons)**

- Migrated all three disk-backed subsystems from per-context to engine-owned singletons, one at a time: `NETWORK` (done earlier this phase), then `CONSOLE`, then `STORAGE` (this session). Pattern per subsystem: `git mv src/context/<sub>` → `src/sneeze/<sub>`; update `src/CMakeLists.txt` + `msvc/Sneeze.vcxproj(.filters)`; ctor `CONTEXT*`→`ENGINE*`; `m_pContext`→`m_pEngine`; construct in `ENGINE::Impl::Initialize` (after paths) + delete in `~ENGINE::Impl`; add `ENGINE::<Sub>()` accessor (declared in `Sneeze.h`); `CONTEXT::<Sub>()` forwards to engine.
- Host resolution: removed `INETWORK_IMPL::Host`, `ISTORAGE_IMPL::Host`, `CONSOLE::Context`; per-container handles (`CACHE`/`STREAM`/`SILO`) self-resolve host via `Container()->Context()->Host()`. Close signatures now take the container: `Cache_Close(CONTAINER*,CACHE*)`, `Silo_Close(CONTAINER*,SILO*)`. `~Impl` leak nets direct-delete (no callback). Removed `NETWORK::Clear/Reset`; `CONTEXT::Logout` is a no-op.
- Dependency ctors (`WASM_RUNTIME`, `SPV_PIPELINE`, `XR_RUNTIME`, `UI_CONTEXT`) now take `ENGINE*` in the constructor instead of `Initialize()`.
- Added `NETWORK::Cache_Enum(IENUM_CACHE*)` + `IENUM_CACHE`/`OnCache`, symmetric with `Stream_Enum`/`Silo_Enum`.
- Fixed two bugs in `~CONSOLE::Impl` cleanup Dean introduced via copy-paste: `m_umpStream` is a map (delete `pair.second`, not `auto* pStream`); `m_apEntry` holds `shared_ptr` (just `clear()`, no `delete`).
- Builds clean; `--storage` 44/44 and `--console` pass. App (Artemis) runs.
- Discussed singleton consequences: engine-wide UNIT/ASSET dedup eliminates two-tabs-same-file write races but makes same-file state live-shared; enums now span all contexts (inspector must filter). **Dean flagged the cross-context change-notification fan-out gap as a real problem to fix later.**
- Docs: updated `project.mdc` (directory structure, module table, new "Subsystem Ownership Model" section) and the per-module `src/sneeze/{network,console,storage}/*.md`. Wiki (`docs/`) front-matter `sources:` paths repointed to `src/sneeze/...`, but **wiki bodies left stale and flagged** — the `docs/` network pages are out of date back to before Phase 1 (no `CACHE` page; `NETWORK.md` still shows the monolithic class), so the wiki needs a full Phase 1+2+3 reconciliation, deferred.
- Dean commits Phase 3 himself.

## June 26, 2026 (evening, ~7:45–9:50 PM PT) — Dean Abramson

**Post-Phase-3 Item 1 of 5 — Storage change-notification fan-out across contexts**

- Fixed the cross-context gap flagged in the afternoon session. Old `OnStorageSiloChanged` fired only on the writing silo, so contexts sharing an org-scoped `UNIT` were never notified.
- Replaced the single storage `Changed` callback with a two-tier set mirroring network's Cache/File tiers: kept `OnStorageSilo{Created,Deleted}` (handle, from `STORAGE`); removed `OnStorageSiloChanged`; added `OnStorageUnit{Created,Changed,Deleted}` (leaf, from `Unit.cpp`).
- `UNIT::Impl` now holds `m_apSilo` (populated in `Open(pSilo)`/cleared in `Close(pSilo)`, mirror of `ASSET::m_apFile`) and an `m_pUnit` back-pointer. Dir creation moved from `SILO::Initialize` into `UNIT::Open`. `UNIT` makes all `OnStorageUnit*` calls via `pSilo->Container()->Context()->Host()`; added public `SILO::Container()` accessor (no `ISILO_IMPL` needed — `UNIT` holds `SILO*` directly, unlike network where `FILE` carries `ICACHE_IMPL*`). `Changed` fans out over `m_apSilo`; `Created`/`Deleted` fire for the one attaching/closing silo. `SILO::Set/Remove/Json` no longer call the host.
- Bonus durability fix (Dean blessed): bulk `Json` setter now appends a root `["Set","",<doc>]` to the `.log`; `Log_Replay` handles the empty-path/root case.
- `ISTORAGE_IMPL` on `UNIT` briefly removed (write-only) then restored at Dean's request as the parent back-pointer (it's the upcast `STORAGE::Impl` `this`).
- Added `StorageTest.cpp` Test 11 proving fan-out (one write → 2 callbacks). Debug build clean; 45/45 storage tests pass.
- Updated `Storage.md` + wiki `ICONTEXT.md`/`SILO.md`. Full detail in `project.mdc` "Post-Phase-3 Jobs" section.
- Remaining jobs 2–5 (Reset/Clear cache, GLB files, Home page, Documentation) deferred to Saturday. Dean commits this himself.

## June 27, 2026 (Sat, ~8 hrs) — Dean Abramson

**Post-Phase-3 Item 2 of 5 — Reset / Clear the cache**

- Long design discussion first (Dean drove), then implementation. Cache-clear for the multi-origin browser: the clear applies to the whole context, but the record is keyed to the context's primary fabric's container; contexts sharing a primary share the clear, different-primary contexts do not (web-consistent).
- **Key pivot:** staleness currency switched from the monotonic asset index to an ISO-8601 `createdAt` timestamp string (asset survives iff `createdAt >= staleTime`). The index was fragile — losing/corrupting the counter file forced a restart-at-1 that collides with scattered on-disk files and gives no cheap invalidation; a wall-clock floor makes invalidation implicit (no tree-walk/purge). `nAssetIx` kept for fetch identity only (Dean's correction — it's not retired).
- Single engine-wide `network_reset.json` at the cache root holds `nAssetIx_Next`, the global stale floor `sTime_Stale`, and the per-primary-key `resets` map. Reserve-ahead counter (`RESERVE_BLOCK=1000`) keeps disk writes rare. Atomic temp+rename writes.
- `Reset_Load` is all-or-nothing (`bValid` flag + `IsIso8601` validation of the floor and every map entry); any failure → fresh state (`nAssetIx=1`, map clear, floor = now). Dean caught a durability flaw: the floor must be persisted (else a clean shutdown after a corruption-recovery silently "unclears" the cache) — fixed, plus `Reset_Stale` returns `max(floor, per-key)`.
- Wiring: `NETWORK::Reset(sKey)` stamps now; `CONTEXT::Key_Reset()`/`Reset()` + `m_sKey_Reset` (primary container's `Key_All`); `bReset` reload flag; staleness resolved through `FILE::Attach`→`CACHE::Reset_Stale`→`NETWORK::Reset_Stale(sKey)`→`ASSET::Attach(..., sStale)` (resolved before the asset lock). `m_mxNetwork_Rules`→`m_mxNetwork_Reset`; `Rules_*`→`Reset_*`; "watermark" purged from code/comments.
- Tests: root-caused and fixed the long-standing 5-failure "cluster" as a test-harness timing bug (asserting `FILE` snapshot accessors before the async `Fetch_Complete`; added `listener.WaitFor`). **71/71 `--network` pass**, Debug build clean.
- `src/sneeze/network/Network.md` fully reconciled. Wiki (`docs/`) left for Item 5. Full detail in `project.mdc` "Item 2 detail". Dean wiring F5 = Reload / Ctrl+Alt+F5 = Reload-with-Reset in Artemis; commits this himself.

## June 27 (evening) – June 28, 2026 (Sun, to ~noon) — Dean Abramson

**Post-Phase-3 Item 3 of 5 — GLB / glTF files**

- Per Jonathan Hale's guidance, glTF loading lives in Sneeze (framework), not pushed through ANARI (pure rendering-engine abstraction). Sneeze loads + decodes; ANARI gets the geometry. Library: **fastgltf** 0.9.0, built from source as a static lib.
- Four-tier layering: (1) `src/deps/gltf/` wrapper — `DEP::GLTF::Load` parses GLB/glTF → CPU `GLTF_MODEL` (textures kept encoded); (2) `src/context/viewport/GltfMesh.cpp` bridge — `Gltf_Render_Model_Build` flattens the node hierarchy into a `GLTF_RENDER_MODEL` (baked world transforms, RGBA8 texture decode via `IMAGE::Decode`, AABB→center/radius); (3) the built model is stored on the **base `MAP_OBJECT`** (`Gltf_Render_Model` get/set, atomic write-once) so a model can sit at any level; (4) `Compositor.cpp` emits the model's meshes at the node's world frame, class-independent, with a box only as the no-model fallback. ANARI side: `SubmitMeshes` → one `"triangle"` geom + `"physicallyBased"` material + instance per mesh, optional `image2D` base-color sampler.
- Night session: fastgltf build wiring (deps cmake, FindFastgltf, CMakeLists, msvc proj, vcpkg, CI), the deps wrapper, the bridge, the ANARI mesh path, and an injected single-GLB smoke test (rendered grey — correct, asset is untextured).
- Morning session: wired GLBs to real fabrics (DFW renders its buildings/roads), removed the smoke-test scaffolding + injected test panel, then three review-driven refactors: folded `GltfMesh.h` into `Viewport.h` (one-header-per-system); moved render-model storage off `NODE` onto `MAP_OBJECT` (it's visual appearance, and GLBs can be celestial/terrestrial/physical); renamed accessor/builder to `Gltf_Render_Model`/`Gltf_Render_Model_Build`; made GLB rendering class-independent in the compositor; and collapsed `NODE`'s dual texture/glTF fetch into one fetch-by-URL + dispatch-by-content path (`Resource_Request`/`Resource_Load`, sniffing GLB magic / glTF JSON / image).
- Static-lib gotcha: Artemis had to add `fastgltf.lib` to its own vcxproj (static libs don't propagate deps). Tooling bug all session: Cursor on Windows wrote new files as UTF-16 — worked around with PowerShell UTF-8 writes + byte-verify.
- Debug build clean, gltf tests pass. Docs: new `Gltf.md`; updated `Viewport.md`, `Scene.md`, `project.mdc` (Item 3 detail). Wiki (`docs/`) deferred to Item 5. Dean commits this himself.

## June 30, 2026 (afternoon/evening) — Dean Abramson

**Post-Phase-3 Item 4 — Home/Error page groundwork: scene lights + backdrop, then pre-commit cleanup**

- **Scene light-node system (kept).** New object class `MAP_OBJECT_CLASS_LIGHT` (75) + `MAP_OBJECT_LIGHT` (subtypes point=1/ambient=2/directional=3) reusing the wire payload (colour in `Properties.fColor` 0xRRGGBB, intensity in `Properties.fBrightness`, kind in `Type.bType`). Instantiated in `Container::Node_Create`; a `Scene`/WASM host path exists. Compositor `TraverseNode` now gathers lights from both `STAR` celestial nodes and explicit light nodes into `LIGHT_BUILD`; `AnariRenderer::SetLights` switches on `eType` to build ANARI point/ambient/directional lights (starless two-light fallback unchanged).
- **Point-light intensity invariance (kept).** `LIGHT_BUILD` carries `dWorldScale` (avg of world-frame upper-3×3 column norms) and `bCompensate`; a point light's intensity is multiplied by `(dWorldScale · dRenderScale)²` at the flatten seam so a unit-scale-authored light illuminates identically regardless of embed scale or per-scene render scale. Star light opts out (`bCompensate=false`, tuned `intensity 0.09`); ambient/directional pass unscaled.
- **Backdrop system (kept).** `SCENE::Background(r,g,b,a)` + atomic changed-flag + `Backdrop_Consume(aColor)`; compositor pushes to `RENDERER::SetBackground` only on change. `Fabric_Root_Create` resets to black each load. `Primary_Apply(pMsf)` reads an optional MSF `"primary"` block (camera position/rotation, `background` RRGGBB hex) — primary fabric only.
- **Cleanup pass before commit (reverted/removed):**
  - Removed `Scene::Fabric_Inject_Internal` (the wrong-way error page) + its `Node_Open_Light` helper + all four failure-path call sites (`OnMsfReady` open/parse/empty, `OnMsfFailed`).
  - Reverted the Halogen working-tree changes (`toneMapping` param in `Frame.cpp/.h`, `Renderer.h`, `FilamentResource.cpp`) back to v1.1.0 — clean tree.
  - Removed the tone-mapping plumbing entirely (it was only used by the deleted error page): `SCENE::Tone_Mapping`, `m_bToneMapping`, the `Backdrop_Consume` tonemapping out-param, `RENDERER::SetToneMapping` (virtual + ANARI override that set the `"toneMapping"` param) across `Scene.cpp/.h`, `Compositor.cpp`, `Viewport.h`, `AnariRenderer.h/.cpp`.
  - Removed the `LogPanelDiag` cross-machine panel diagnostic helper + its gated `BuildScene` call site + the now-unused `nPanelIdx` counter (`AnariRenderer.cpp`).
  - Dean reverted `cube.glb` + `make_cube_glb.py` himself.
- **Verified** no leftover debug lines from the unsolved solar-scale logo-lighting experiments: no `std::max` intensity floor, no `MAX_REACH` cap, no temporary `dRenderScale = 1.0`, no stray logging.
- Docs updated for current state: `Scene.md` (MAP_OBJECT_LIGHT section + derived-types row, Backdrop + Primary Presentation), `Control.md` (Lighting rewrite with invariance, Backdrop note), `Viewport.md` (`LIGHT_DATA` row + Lighting per-type). Wiki (`docs/`) still deferred to Item 5.
- **Context (not code):** long discussions this session on the Dave-vs-Dean GPU rendering discrepancy (panel dark on NVIDIA/HDR-post vs bright on Intel); deferred. Dean commits this himself.

## July 3, 2026 (~9:20 AM - 2:00 PM PT) - Dean Abramson

**Examples suite kickoff (example 01) + supporting engine features. (Continued a chat that Anthropic killed; recovered context from the prior transcript.)**

- **Example 01 (`examples/01-stool/`).** Built the first fabric authoring example: `stool.json` (map-managed, single physical node, model via relative path) + a beginner-facing `README.md` walkthrough. Established the house voice for these docs (plain language, why/what/how, no smugness, one line per paragraph, ASCII only). Confirmed rendering live.
- **Asset pipeline.** Turned 5 CC0 downloads into optimized GLBs (`Stool/Tin/Container/Bucket/Crate`) via `npx gltf-pipeline`/`obj2gltf` + a sharp texture optimizer (1024px, JPEG/PNG), ~90-97% smaller; stripped a transmission material on the food container. In `examples/assets/` + CDN.
- **Engine feature - `P-?` auto-index.** `ComposeFromId` treats a `?` index as `OBJECTIX_IDENTITY`, which `Node_Create` resolves to the next free per-container index. Needed because multiple signed fabrics can share a container, so hard-coded indices collide.
- **Engine feature - relative URLs.** New `FABRIC::Resolve` resolves module/resource references against the fabric's own URL by standard RFC-3986 rules (`scheme://` absolute, `/` host root, else fabric-folder-relative, `..`/`.` collapse). Wired at 3 fetch sites. Confirmed working live.
- **Engine change - removed the model-less-physical box fallback** in `Compositor.cpp` (+ dead `ColorFromIndex`); rest of the box machinery left as dead code per Dean.
- **Wiki.** Added `docs/examples/index.md` + `docs/examples/01-stool.md`, a `### 6. Examples` section on `docs/Home.md`, and an examples pointer in "Choose your path" (addressing the "hard to find / needs copyable examples" feedback).
- **Parked:** lightless-scene ambient via Halogen/Filament IBL produces nothing at any value; wiring verified correct in source but unexplained; note sent to Jonathan Hale. The AnariRenderer.cpp ambient rework was **reverted** (never verified working). Cursor's UTF-16 new-file write bug is still intermittently active post-update (hit the 2 new wiki pages; re-encoded to UTF-8).
- **Deferred:** example 02 (place the model with a Transform); per-example wiki pages as the suite grows; hashing + signing examples; Jonathan's ambient answer. Full detail in `project.mdc` "Examples Suite" section. Dean commits this himself.

## July 3, 2026 (~2:11 PM - 5:37 PM PT) - Dean Abramson

**Example 02 built + finished; spot-light support and fColor fixes added; all GLB origins recentered. (Spanned two chats; Anthropic killed the first mid-turn, recovered context from transcripts.)**

- **Example 02 (`examples/02-stool-and-bucket/`) - DONE.** Stool as root with a bucket and three lights as `Children`. Teaches `Children` (scene as a tree), `Transform` placement, and authoring lights. `README.md` in house voice; wiki page `docs/examples/02-stool-and-bucket.md` created and wired into the index, `Home.md`, and Example 01's prev/next nav. Added a warning emoji (⚠️) to every "This is not the preferred way..." heading across both READMEs and both wiki pages.
- **fColor fixes (approved).** Lights default to white when `fColor` unset (`Compositor.cpp`; also fixes the broken plaza guide example); `fColor` authorable as `0xRRGGBB` int / `"0x.."` / `"#.."` (`HostFunctions.cpp`, bits memcpy'd into the float).
- **Spot-light support (builds clean; cone doesn't render).** Light enums renumbered/aligned `NONE=0,AMBIENT=1,DIRECTIONAL=2,POINT=3,SPOT=4`; added `kSPOT`, cone fields, spot direction, ANARI `"spot"` branch; `SetLights` rebuilds on content change. Halogen spot cone is inert - note sent to Jonathan; Dean keeps spots as the documented pattern anyway.
- **GLB origins recentered to bottom-center.** All 5 shared GLBs re-authored (base at y=0, centered x/z) via a self-contained Node script; originals in `e:\dev\gltf\_orig\`. Example 02 updated accordingly (bucket y=0.428, lights +0.214).
- **Key handoff:** committing the current (uncommitted) engine changes will break lighting in all existing fabrics (enum renumber + white default). Dean's plan: commit -> Y->Z-up conversion -> rewrite ALL fabrics in one pass. Taking a break; will resume to do the Z-up + fabric rewrites. Nothing committed - Dean commits himself.
- **Tooling:** Cursor UTF-16 write bug still active - this session it flipped some `StrReplace` saves too, not just new-file `Write`s. Byte-verify after any write.
