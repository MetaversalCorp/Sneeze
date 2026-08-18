# Control — Engine Thread, Agent Pools, and Metronome

The `control` module owns the engine thread, agent pools, the drift-free
metronome, and the job queues. It is the engine's scheduler.

## Architecture

```
CONTROL (inherits THREAD)
 ├── Engine thread (Main)
 ├── Metronome (drift-free, fixed-origin, 1ms resolution)
 ├── Pool factory table (AGENT_INIT)
 └── Pool vector (m_apPool)

POOL (base class, non-template)
 ├── Agent vector (m_apAgent)
 ├── Metronome state (nHertz, nLastTick)
 └── Lifecycle (Initialize, ~POOL tears down agents)

POOL_QUEUE<JOB_PTR> (template, inherits POOL)
 ├── Typed job queue (m_apJob, m_mxQueue)
 ├── Post (targeted wake of first idle agent)
 └── Grab (pops next job)

POOL_CYCLE (inherits POOL)
 ├── Perpetual job pool for compositor
 ├── Post/Remove lifecycle
 └── Grab routes Create/Destroy to agent 0 (Filament thread affinity)

AGENT (inherits THREAD, abstract base)
 ├── COMPOSITOR  (queue-driven via POOL_CYCLE, 1 agent)
 ├── SCRUB       (queue-driven via POOL_QUEUE, 2 agents)
 ├── FETCH       (queue-driven via POOL_QUEUE, 16 agents)
 └── TIMER       (signal-driven, ~1000 Hz, 4 agents; drives the WASM timer service)
```

## Ownership Chain

```
ENGINE -> CONTROL -> POOL -> AGENT
```

Each child holds a pointer to its immediate owner. Agents access engine services
via `m_pPool->Engine()`.

## Job Classes

All jobs derive from `IJOB` (Cancel/IsCancelled/Complete). Concrete jobs are
heap-allocated and self-cleaning (`Release()` calls `delete this`).

| Job | Agent | Key Fields |
|-----|-------|------------|
| `JOB_FETCH` | FETCH | `Url()`, `Path_Temp()`, `Path_Data()`, `Hash()`, `IsFetch()` |
| `JOB_SCRUB` | SCRUB | `Path()` |
| `JOB_COMPOSITOR` | COMPOSITOR | `Viewport()`, state machine, `m_nLastFrame` |

`JOB_FETCH` carries `bool m_bFetch` to distinguish real HTTP fetches from
notify-only jobs (asynchronous notifications for cached/failed files).

`JOB_COMPOSITOR` has a state machine: kSTATE_CREATE -> kSTATE_RENDER ->
kSTATE_PRESENT -> kSTATE_DESTROY. `Cancel()` blocks until the compositor
thread completes destruction on agent 0.

## Pool Configuration

| Index | Pool Type | Agent | Size | Hz | Behavior |
|-------|-----------|-------|------|----|----------|
| 0 | POOL_CYCLE | COMPOSITOR | 1 | 0 | Queue-driven (Filament thread affinity) |
| 1 | POOL_QUEUE\<JOB_SCRUB*\> | SCRUB | 2 | 0 | Queue-driven (disk cleanup) |
| 2 | POOL_QUEUE\<JOB_FETCH*\> | FETCH | 16 | 0 | Queue-driven (HTTP downloads) |
| 3 | POOL | TIMER | 4 | 1000 | Metronome-driven; fires due guest timers |

## CONTROL

Inherits THREAD. Constructor takes `ENGINE*`. `Main()` creates pools from the
static `AGENT_INIT` factory table, then runs the drift-free metronome loop
(`Wait(1ms)`, fixed-origin scheduling, `timeBeginPeriod(1)` on Windows).

Public API:
- `Queue_Post_Fetch(JOB_FETCH*)` — routes to fetch pool
- `Queue_Post_Scrub(JOB_SCRUB*)` — routes to scrub pool
- `Queue_Post_Compositor(JOB_COMPOSITOR*)` — routes to compositor pool
- `Engine()` — returns `m_pEngine`

## AGENT Patterns

### Queue-Driven (SCRUB, FETCH)

```
Main():  Ready() → Wait([this] { return Job(); })
```

`Job()` loops: Grab from queue, process, Release. Returns `IsShutdown()`.

### Signal-Driven (TIMER)

```
Main():  Ready() → Wait([this] { return Tick(); })
```

The metronome ticks the TIMER pool once per wake (~1000 Hz), waking all four
agents. `Tick()` drains every currently-due timer from the engine WASM timer
service (`WASM_TIMERS`, owned by `WASM_RUNTIME`): `Claim` hands each agent a
distinct in-flight entry, the agent calls `WASM_STORE::Notify_Timer` (which
takes the per-store lock, so same-store fires serialize while different stores
run in parallel), then `Complete` reschedules a repeat or drops a one-shot.
`Tick()` returns `IsShutdown()`, so the agent sleeps until the next signal. The
timer queue, due math, and store-teardown drain all live in `WASM_TIMERS` — the
control module owns only the metronome cadence, never timer state.

### AGENT::COMPOSITOR

Queue-driven via POOL_CYCLE. `Job()` grabs the best available
JOB_COMPOSITOR and dispatches by state:

- **Execute_Create** (agent 0 only) — calls `Viewport::Renderer_Initialize()`
- **Execute_Render** (any agent) — consumes input, updates camera, builds scene,
  calls renderer BeginFrame/EndFrame
- **Execute_Present** (any agent) — readback framebuffer, calls `OnFrameReady`,
  runs `Diagnostics()`
- **Execute_Destroy** (agent 0 only) — calls `Viewport::Renderer_Shutdown()`

### Rendering pipeline / scaling (Compositor.cpp)

`Execute_Render` walks the scene graph (`TraverseNode`, recursive) to build
metre-space draw lists, then flattens them to render space in a single seam.

**Universal TRS.** Every node — from the synthetic `ROOT` down to the
bottom-most `PHYSICAL` node, celestial bodies included, with no exceptions —
composes a full Translation·Rotation·Scale transform and inherits its parent's
composed transform (`WORLD_FRAME` carries a `MAT4`, double, column-major; see
`Types.h`). There is no class-specific shortcut: orbital position, axial tilt,
and scale all nest the same way for stars, planets, moons, buildings, and
colliders. Synthetic root nodes (created in `Scene::Fabric_Root_Create`, not
parsed from JSON) are seeded with an identity transform by `RmcObject_Init` —
otherwise their zeroed `scale=(0,0,0)` collapses every descendant to the origin
(this was the "moon inside the Earth" bug).

**No AU.** The old `METERS_TO_AU` celestial path is gone. All geometry is
accumulated in metres, and `dMaxReach` tracks the scene's extent as a
root-anchored bounding sphere radius (`|world_pos| + body_radius` per node, so a
single-body scene still gets a sane reach).

**Per-scene render scale.** After traversal, `dRenderScale = TARGET_EXTENT /
dMaxReach` (`TARGET_EXTENT = 5.0`; falls back to `1.0` below `MIN_REACH`). It is
computed once per build and multiplied into every sphere centre, curve point,
light position, and box matrix at the flatten seam. This is the interim
fixed-precision solution: each scene picks one uniform factor so its whole extent
fits the render volume and the default camera frames it. (A superior technique is
patented and planned but not implemented; the global factor is the deliberate
stopgap.)

**Proximity-driven lazy loading (map-managed fabrics).** Deeper tiers of a
map-managed scene stream in as the camera approaches, rather than all at once.
Detection is folded into the read-only `TraverseNode` walk: for each node it
computes the distance (world metres) from the node's world position to the camera
and, when under the global `PROXIMITY_LOAD_METERS` threshold, records
`(CONTAINER*, NODE::ObjectIx())` in an `aExpand` collection. The camera's metre
position is `vEye` (render units) divided by the **previous** frame's
`m_dRenderScale` (this frame's scale isn't known until traversal finishes; the
one-frame lag is harmless for a proximity gate, and the scale defaults to `1.0`
before the first frame). Nothing is mutated during traversal. Immediately after
`TraverseNode` returns, `Execute_Render` drains `aExpand`, calling
`CONTAINER::Node_Expand(handle)` on each entry. Map-managed containers forward to
`MAPSVC::Expand`, which streams one child level (`Node_Open`) when the node's
RMAP model is ready or defers until it becomes ready; every other container
no-ops. Collection is recomputed every frame and de-duplicated downstream
(`bChildrenLoaded`), so no per-node "already requested" bookkeeping lives in the
compositor. Load-only for now — nodes are never `Node_Close`d as the camera
recedes (streaming-out is future work). Because the drain runs on the render
thread after traversal, the ready-now `Node_Open` is safe against the current
frame; a model that becomes ready later opens on the RMAP thread and shares the
same pre-existing traversal/mutation hazard documented in `Scene.md` "Known
Limitations" (no shared read-guard yet). `PROXIMITY_LOAD_METERS` is a tunable
file-scope constant in `Compositor.cpp`.

**Body magnification.** Visual radii do not use the raw scaled metre radius (the
Moon would be sub-pixel from afar). `MagnifyRadius` applies a power law:
`BODY_MAG * (radius_render ^ BODY_EXP)`, currently `BODY_MAG = 1.25`,
`BODY_EXP = 0.7`. `BODY_EXP < 1` compresses the huge range so small bodies grow
faster than large ones (Earth vs. Jupiter stay visibly different without the Sun
ballooning). `MIN_SPHERE_RADIUS = 0.0` (no clamp).

**Moon kludge.** Moons need to clear their magnified planet and read as small,
distinct bodies. `MOON_ORBIT_BOOST = 5.0` multiplies the local position of
`MOONSYSTEM` nodes (and the points of their orbit trails) so the orbit sits
farther out; `MOON_SIZE_FACTOR = 1.0` (currently no extra shrink). This is an
intentional, moon-only special case, valid while the data has at most a handful
of moons; it is not physically faithful.

**Orbit trails.** Curve (tube) geometry per orbit. `TRAIL_RADIUS_PLANET =
0.0002`, `TRAIL_RADIUS_MOON = 0.00005` (moon/debris systems use the thinner
radius). Both were cut to roughly a tenth of earlier values so trails don't
overpower the scaled bodies.

**Boxes (terrestrial/physical).** Each terrestrial/physical node emits an
oriented, grounded box (unit-cube mesh + per-box `MAT4` instance, mirroring the
textured-sphere instancing). GLB files are **not** parsed in the compositor —
physical nodes render as boxes for now; handing whole GLBs to Filament via
Halogen is deferred. Physical nodes whose resource reference begins with
`"action:"` (e.g. `action://collider`) are **skipped** — they are invisible
helpers. Skipping them also keeps a giant collider (e.g. DFW's multi-km "Sector
Floor Collider") from dominating `dMaxReach` and shrinking every real building to
sub-pixel size (the "DFW is one flat sheet" bug).

**Panels.** Each `MAP_OBJECT_PANEL` node rasterizes its RmlUi document to CPU
pixels during traversal (`UI_PANEL::Render` on this thread; cheap when unchanged)
and emits a `PANEL_DATA` (transform + pixel buffer) the renderer draws as an
unlit textured quad (see `Ui_Context.md`, `Scene.md`, `Viewport.md`). Panels are
**chrome, not scene geometry**: they do **not** contribute to `dMaxReach`, so a
panel never changes how the 3D content is framed. The production design is that a
panel's world size lives in `Bound.d3Max[0,1]` (metres) and its placement in the
node's TRS, riding `dRenderScale` exactly like a box. The current browser-internal
**test** panel deviates: because one panel is injected into every fabric
(`Scene::Panel_Inject_Test`), a single absolute metre size cannot suit both a
planetary system and a city block, so the flatten seam sizes it as a fraction of
`TARGET_EXTENT` and **billboards** it toward the camera (its `+Z` normal tracks
the eye, recomputed each frame) anchored just above the scene centre. This is
scaffolding for the future panel API; `Bound`/TRS carry through and would drive a
real per-fabric panel.

**Lighting.** The compositor produces two kinds of light. **Placed lights**
(point/spot): `TraverseNode` gathers a `LIGHT_BUILD` from every `STAR` celestial
node (a point light at the star's world position) and every explicit
`MAP_OBJECT_LIGHT` node (see `Scene.md`). A light node's colour comes from
`Properties.Light.fColor`, its intensity from `Properties.Light.fBrightness`, and
its subtype selects point or spot. A placed light's position is scaled by
`dRenderScale` at the flatten seam. A spot additionally aims down the node's local
-Z (rotated by its world frame) with a cone from `fOpeningAngle`/`fFalloffAngle`.
These are pushed with `RENDERER::SetLights`. **Scene-global** ambient and
directional ("sun") are *not* light nodes — they are scene properties authored in
the primary fabric's `"Primary"` block, read straight off the `SCENE`
(`SCENE::Ambient` / `SCENE::Directional`) and pushed with
`RENDERER::SetSceneLighting`, so a local node can never alter global illumination.

*Intensity invariance.* A point light's `1/r²` falloff means distances matter, so
scaling a light's frame must be compensated to keep illumination constant. Each
`LIGHT_BUILD` records `dWorldScale` (the average of its world frame's upper-3×3
column norms). At the seam a point light's intensity is multiplied by
`(dWorldScale · dRenderScale)²`, so a light authored at unit scale illuminates
identically no matter how the fabric is embedded/scaled or how the whole scene is
fitted to the render volume. The engine-generated **star** light opts out via
`bCompensate = false` (already tuned at render scale, `intensity 0.09` to keep
sunlit limbs from clipping to white). See `Viewport.md` "Lighting" for the ANARI
side.

**Backdrop.** After the draw lists are built, the compositor calls
`SCENE::Background_Consume` and, only when the colour changed, pushes it to
`RENDERER::SetBackground` (see `Scene.md` "Backdrop").

### AGENT::FETCH

Checks `IsFetch()`: if true, performs blocking curl download with SRI hash
verification; if false (notify-only), skips network work. Curl progress
callback checks `IsCancelled()` to abort in-flight downloads.

## Files

| File | Contents |
|------|----------|
| `Control.h` | All declarations: IJOB, JOB_FETCH, JOB_SCRUB, JOB_COMPOSITOR, POOL, POOL_QUEUE, POOL_CYCLE, CONTROL, AGENT + derived |
| `Control.cpp` | CONTROL implementation, pool factory table, metronome |
| `Pool.cpp` | POOL base class |
| `Pool_Queue.cpp` | POOL_QUEUE template + explicit instantiations |
| `Pool_Cycle.cpp` | POOL_CYCLE (perpetual compositor pool) |
| `Agent.cpp` | AGENT base class |
| `Compositor.cpp` | AGENT::COMPOSITOR (render loop, state machine) |
| `Scrub.cpp` | AGENT::SCRUB (disk cleanup) |
| `Fetch.cpp` | AGENT::FETCH (HTTP downloads) |
| `AgentTimer.cpp` | AGENT::TIMER (fires due guest timers via WASM_TIMERS) |
| `AgentC.cpp` | AGENT::C (retired placeholder; class retained, no longer pooled) |
| `IJob.cpp` | IJOB base (Cancel/IsCancelled/Complete) |
