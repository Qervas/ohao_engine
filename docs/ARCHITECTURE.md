# OHAO architecture

A map of the code: the libraries and which way they depend, how one frame flows from
game state to pixels, and recipes for the changes people make most often. Each recipe
was followed once, end to end, when it was written (sub-project 3, step 10: the `Spin`
component, the spin system, `entity.duplicate` and `systems_tests`; sub-project 4, step
11: an API method, a render pass and its shader, a golden scene).
`python tests/docs/check_docs.py` runs every `console` command here.

## Libraries

All under `ohao/`, each a static library with its own `CMakeLists.txt`. Sources are
globbed with `CONFIGURE_DEPENDS`: a new file is picked up by the next build.

| Library | Directory | Depends on | What it owns |
|---|---|---|---|
| `ohao_core` | `core/` | glm | the logger (`OHAO_LOG`, the ring, sinks), the time model (`Timeline`, `FixedStepClock`: lockstep or realtime, paused, the frame number), input (`InputMap`: actions, axes, the virtual device injection uses; the `Key` name table), the event bus, command history, `Result`, concepts, small shared types |
| `ohao_reflect` | `reflect/` | glm (fast_float privately) | the type registry, schema/catalogue export, the JSON writer, reader and layout, typed decode/encode. Knows nothing of ECS or GPU. |
| `ohao_scene` | `scene/` | assimp, ufbx | asset loading into CPU `Model`s, procedural meshes |
| `ohao_gpu_vulkan` | `gpu/vulkan/` | `ohao_scene`, `ohao_core`, Vulkan, VMA | device and queues, `VulkanRenderer` (the facade), frame resources, bindless textures, the allocator |
| `ohao_renderer` | `render/` | `ohao_gpu_vulkan`, `ohao_scene`, `ohao_core` (NRD, OIDN, NGX optional) | `RenderScene` (the persistent GPU scene), deferred passes, path tracers, denoisers, the render graph |
| `ohao_ecs` | `ecs/` | `ohao_core`, `ohao_renderer`, `ohao_scene`, `ohao_reflect`, `ohao_physics`, EnTT (simdjson privately) | `World` (simulate and sync phases, its `InputMap` bound from `InputActions`), components, systems (spin, fly controller, transform, instantiate, physics, render extract), the scene codec, snapshots, the world hash and diff, the scene view. The physics system (`physics_scene`, installed by `installPhysics`) builds Jolt bodies from `Collider`/`RigidBody`; `physicsScene(world)` reaches it. |
| `ohao_level` | `level/` | `ohao_core`, glm (simdjson privately) | `readGltfStructure`: a glTF file's node tree, `extras`, lights, cameras and mesh counts, read from its JSON |
| `ohao_level_world` | `level/` | `ohao_level`, `ohao_ecs`, `ohao_reflect` | levels in the world: hints (`level_hints`), the `level` sync system (`installLevels`), and what `level.info`/`level.reload` read (`levelSummary`, `levelDiagnostics`, `reloadLevel`) |
| `ohao_api` | `api/` | `ohao_ecs`, `ohao_level_world`, `ohao_reflect` | `Session`: the method table and its params structs, edit commands, queries, the path sandbox; `level.*` and `physics.*` through the hooks the World's installers leave (`level/level_system.hpp`, `ecs::physicsScene`). GPU-free. |
| `ohao_rpc` | `rpc/` | `ohao_api`, `ohao_core` | JSON-RPC 2.0 over newline-delimited streams (stdio, or a local named pipe: `PipeTransport`) and the MCP adapter (`ohao serve`): reader and engine threads, the bounded queue, cancellation, progress, the log sink. CPU-only: no Vulkan, GLFW or renderer header (`reflect_tests`). |
| `ohao_runtime` | `runtime/` | `ohao_api`, `ohao_core`, `ohao_ecs`, `ohao_gpu_vulkan`, glfw | `LiveHost`, the window host (window, renderer, World and Session, one loop for `ohao_game` and `ohao serve --window`); `captureFrame`, the one capture procedure both hosts run; capture images (crop, scale, a fast PNG writer, JPEG); `applySceneView`; frame statistics |
| `ohao_physics`, `ohao_audio` | `physics/`, `audio/` | `ohao_core`; Jolt / miniaudio | physics (the `IPhysicsBackend` the ECS's physics system builds bodies through; nothing steps them yet) and audio (not yet driven by the ECS) |
| `ohao_host` | `tools/ohao/` | `ohao_runtime`, `ohao_api`, `ohao_ecs`, `ohao_renderer` | `RenderHost`, the headless `api::Host` behind `render.capture` |

Programs: the examples (`examples/`: `cornell_box`, `model_viewer`, `turntable`,
`env_demo`, `ohao_game`), the CLI (`tools/ohao`), `scene_bench`, `serve_bench`,
`render_bench` and `level_bench` (`tools/bench`; they share the bench10k builder,
`bench_world.hpp`, and the calibration and load record, `bench_env.hpp`).

Dependency rules:

- **Down only, with one known knot.** `ohao_gpu_vulkan` includes `render/` headers
  (`VulkanRenderer` owns the deferred renderer and the path tracers) but does not link
  `ohao_renderer`; executables link both and the linker resolves the cycle. Do not add
  new edges from `gpu/` into `render/`.
- **The renderer never sees EnTT.** `render/` and `gpu/` know nothing of `ecs/`; the
  render extract and instantiate are the only bridges, and they write `RenderScene`.
- **Reflection stays off the frame.** `render/`, `gpu/` and the per-frame ECS system
  sources (`transform_system`, `render_extract`, `instantiate`, `spin_system`) include
  nothing from `reflect/` or `api/`. `reflect_tests` (t13) enforces it.
- **The API is GPU-free.** Anything that needs a renderer goes through the session's
  `api::Host` (`Session::setHost`): the headless `RenderHost` or the window's `LiveHost`.
- **The engine prints only through the logger.** `OHAO_LOG(level, channel, fmt, args...)`
  (`core/log.hpp`): levels `Trace` … `Error`, channels `render`, `gpu`, `validation`,
  `ecs`, `asset`, `physics`, `audio`, `api`, `rpc`, `runtime`, a `std::format` string.
  Below the threshold a call costs one load and a compare; records go to a 4,096-record
  ring and to stderr as `<L> <channel>: <message>`. Nothing under `ohao/` writes to
  stdout, `std::cerr` or `printf`; `reflect_tests` (`sp4_t02`) enforces it.
  `OHAO_LOG_LEVEL=debug` shows the detail lines.

## One frame

The same `World` runs headless (CLI, tests, examples) and windowed (`ohao_game`). Its
systems come in two phases, each a fixed list run in order, never discovered or sorted
(`World::simulateNames()`, `syncNames()`):

```
the timeline (core/timeline.hpp) decides the steps: lockstep, on request (world.step);
realtime, whole fixed steps from wall time (paused: none)
for each step (api::Session::step: world.step, input.replay, a window's realtime steps):
  World::stepInput(events)   device events since the last step, then injected ones
                             (InputActions rebinds the map first)
  World::simulate(fixedDt)
    spin            engine-provided: Spin -> Transform (patch)        every World
    fly_controller  engine-provided: input + FlyController -> Transform   every World
    physics         propagateTransforms (dirty subtrees; prevWorld kept   installed with the physics
                    at the frame's start), PhysicsScene::sync (bodies     scene (installPhysics);
                    for changed colliders, moved entities move bodies,    nothing for a world
                    an edited dynamic body teleports at rest, a moved     without a collider
                    kinematic one by MoveKinematic), the players          or a player
                    (PlayerController: input -> CharacterVirtual ->
                    Transform, PlayerState), Jolt in
                    substeps <= 1/60 s, awake dynamic bodies ->
                    Transform (patch) and BodyMotion, the step's contact/trigger events
                    and the players' footsteps and landings (with the
                    ground's Collider.surface) (physicsEvents(world), the
                    log physics.events reads)
    combat          first the enemy AI (every Enemy in id order: sight   installed by installCombat
                    ray, Idle/Chase/Attack/Dead, MoveIntent for the next
                    step, WeaponTrigger for this one); then
                    every Weapon in id order: trigger (a player's input
                    fire/reload, else WeaponTrigger), cooldown, reload,  (after installPhysics);
                    bloom and recoil recovery, at most one round: a ray  nothing for a world
                    from the eye (PhysicsScene::raycastFrom: not the     without a Weapon
                    shooter, not triggers) -> Health of what it hit or
                    its ancestor; recoil -> PhysicsScene::turnPlayer;
                    WeaponState written; shot/hit/damage/death/reload
                    events (combatEvents(world), a log of 1,024); last
                    the round (a world with Round and the RoundHook       RoundHook: api::Session
                    api::Session installs): round_start and a snapshot   (ecs::installRoundHook)
                    at its first step's end; the player dead for
                    restartDelay, or `restart` pressed -> the snapshot
                    restored at this step's end (restart); every Enemy
                    dead -> win, then waits for `restart`; within each
                    weapon, a round lights its Muzzle light for
                    Muzzle.time (WeaponState.flash), then intensity 0
    impact_marks    the step's hits -> the World's mark pool (64, the   installed by installCombat
                    oldest reused), each measured against the step's
                    pose of what it hit, so every host, its renderer
                    made early or late, holds the same marks; a restart
                    clears them; never in the registry
  a game's fixedUpdate hook (LiveHost), reading the step's input        writes with patch/replace
after the frame's steps (LiveHost with LiveConfig::footsteps, ohao_game):
  runtime::Footsteps  the log's footsteps and landings since the last frame -> AudioSystem,
                      <surface>_<n>.wav (assets/local/audio/footsteps, then assets/audio/footsteps)
  runtime::CombatSounds  the combat log's events since the last frame -> fire, enemy_fire,
                      impact_<surface>, hurt, death, reload, empty (assets/audio/weapons)
once per batch of steps and before each render:
World::sync()
  instantiate     MeshSource/MaterialOverride -> MeshRenderer,        installed by a host with
                  meshes and materials registered in the RenderScene   a renderer (addSyncBefore)
  player_spawn    new or changed PlayerControllers -> characters, at  installed by installPhysics
                  the player spawn (so a play scene's first frame is
                  the spawn's view)
  transform       Transform + Parent -> WorldTransform; tags RenderDirty, MovedLastFrame
  physics         dirty colliders -> Jolt bodies; moved entities move     installed by installPhysics
                  their bodies (no step: that is the simulate system);
                  after a restore, the snapshot's simulation back in
                  place (Jolt's state, characters, players)
  interpolate     World::renderAlpha < 1: what the last step moved    installed by installPhysics
                  (players, heads, dynamic bodies) and its subtree ->
                  RenderPose between its last two step poses; at 1
                  none (Transform, the hash, saved files untouched)
  render_extract  MeshRenderer/Light -> RenderScene instance slots    installed by a host with
                  (at RenderPose when there is one)
                  and the light list; setLights gives each point,      a renderer (addSyncAfter)
                  spot and area light an emitter instance (its
                  sphere or rectangle, seen by camera and mirror rays;
                  a spot's shines toward the camera by its cone:
                  setViewerPosition)
  impact_marks    the mark pool -> up to 64 quads (RenderScene         installed by a host with
                  instances, never in the registry), each following    a renderer
                  the entity it hit                                    (installImpactMarks)
host: applySceneView (the first active Camera, at its RenderPose if  runtime/scene_view_apply.hpp
      any; Environment -> renderer camera, HDRI)
VulkanRenderer::render()
  RenderScene::recordUpdates   staged copies of changed table rows; TLAS build or refit;
                               the draw table (one DrawRecord an instance's submesh) when
                               the instances change
  GpuCulling                   the draw table to indirect lists on the GPU: early (last
                               frame's visible), Hi-Z of that depth, late (newly visible);
                               the G-buffer and CSM draw only those. Draws of 4,096
                               triangles or more go on meshlet lists: a task shader tests
                               their meshlets (cone, frustum, Hi-Z) and mesh shaders draw
                               the rest (without VK_EXT_mesh_shader: a compute pass and
                               per-meshlet draws, the same image). The meshlets are built
                               at registerMesh (render/scene/meshlet_builder)
  LightClusters                the raster lights (uncapped) into the frame slot's light
                               buffer when they changed; lights.cluster lists, for each of
                               16x9x24 froxels out to the camera's far plane, up to 256
                               lights that reach it (directional and area lights reach
                               every froxel); lighting sums a pixel's froxel's list
                               (render/deferred/light_clusters)
  deferred passes or the path tracer, then present (window: the final image or the tracer's
  output blitted into the swapchain image, present / pt.present); any frame, headless or
  windowed, copies to the CPU only when asked (requestReadback, then readback(): that
  frame's fence, never the queue); an inline capture of a deferred frame asks for the
  preview alone (requestPreviewReadback: capture.preview box-filters the final image into
  the staging buffer). resetAccumulation() waits for nothing: the next frame starts
  without history.
  every pass between two GPU timestamps (render/frame/gpu_pass_timer.hpp), named in the
  pass registry; a frame slot's times are read when the slot comes round, after its fence:
  render.stats, render_bench, never a stall
```

The deferred frame, pass by pass, under its registry names (`render/frame/pass_registry.hpp`,
the order `render.stats`, a capture's `timings` and `render_budgets.json` use). Brackets
name the `render.settings` member that turns a pass off or reshapes it; a pass that is off
is not registered in the frame's graph and costs nothing:

```
scene.upload        table rows, BLAS builds, TLAS build/refit, the draw table
csm                 shadow cascades, only when rt.shadows does not run   [csm.cascades, csm.size]
cull.instances      early lists: last frame's visible draws in the frustum   [occlusionCulling]
gbuffer.early       G-buffer cleared, early lists drawn (instances; meshlets [geometry])
hiz.build           max-depth pyramid of the early depth
cull.late           every draw vs frustum and pyramid: the newly visible
gbuffer.late        late lists drawn
lights.cluster      froxel light lists (16x9x24, up to 256 each)
ssao                half-res horizon AO, blurred   [ssao, or post "ssao"]
rt.shadows          a ray to each of a pixel's most important lights   [rtShadows.enabled, .maxLights]
rt.shadows.filter   5x5 filter + temporal (realtime quality)
rt.gi               one-bounce GI, half res 2 rays by default   [rtGI.enabled, .resolution, .rays]
rt.gi.denoise       reprojection, 2 a-trous, upsample (realtime quality)
lighting            froxel list + IBL + shadows + GI into the HDR image (+ env specular, its weight)
sky, particles      into the HDR image
sss                 subsurface blur on classified tiles   [sss]
ssr                 half-res trace; resolve replaces env specular in place   [ssr]
post.bloom, post.taa, post.tonemap   HDR to RGBA8   [capture post; exposure]  (bloom's input clamped at luminance 4)
gizmo               transform handles
hud                 the HudList over the final image (only when the list has items:      [capture hud]
                    a window always; a capture with hud: true)
capture.preview     only for an inline capture: box filter into staging
present | readback  window: blit to the swapchain (+ requested copy); headless: copy to staging
```

`quality: reference` swaps the realtime shadow and GI passes for their converging forms
(8 shadow rays to every light, 32 GI rays at full resolution, averaged over the frames).
A path-traced frame is `scene.upload`, `pt.render`, then `pt.present` or `pt.readback`.

The window host, `runtime::LiveHost` (`ohao_game`; `ohao serve --window`), runs this per
frame: poll the window; run the queued protocol requests (`rpc::Server::poll`, waiting up
to 16 ms while nothing steps); the timeline's steps (`Session::step` each, then the
`fixedUpdate` hook); the `update` hook; the combat sounds and the HUD list (`ecs::combatHud`
at the window's size); render and present in its live mode (`--mode`:
deferred, `rt_realtime` or `rt_offline`; realtime: every frame; lockstep or paused: only
when `Session::changeCount()` moved or the window resized, and an `rt_offline` window
until `--spp` samples have accumulated); then send the replies those requests produced
(`flush`), after the frame they caused. While nothing steps the window's own input is
dropped, and stepping resumes with a focus loss. A capture in lockstep is
`runtime::captureFrame`, the procedure the headless `RenderHost` runs, in any mode, so both
give the same pixels for the same world; it checks the call's cancel flag between frames.

- **Change tracking is by signal.** `patch`/`replace`/`emplace` fire EnTT signals that
  set `TransformDirty`, `InstantiateDirty` and `RenderDirty`; each system works only on
  what is tagged, and per-frame upload cost follows what changed (`RenderScene::stats()`).
- **Simulate systems** (today: spin) change the world, once per fixed step, with the
  fixed dt. **Sync systems** derive state from it, with no dt: rendering a frame calls
  `World::sync()` only, so it never advances the simulation, and a realtime run and a
  lockstep run of the same steps reach the same state (the same `world.hash`).
  `World::update(dt)` is one `simulate(dt)` and one `sync()`, for tests and tools.
- **The fixed `dt` is the only clock** a system may read. `world.step` in the API,
  `ohao run --steps` and `LiveHost`'s timeline pass it.
- **Teardown order:** the `World` before the renderer. Its systems release their
  `RenderScene` slots when it is destroyed.

Scene files, snapshots and the API sit beside the frame, not in it: load/save
(`ecs/scene_io`), snapshot/restore (`ecs/snapshot`) and commands (`api/commands`)
change components through the same signals, and the next update carries them through. A
system with state outside components that a replay needs puts a `SnapshotHook`
(`ecs/snapshot_hook.hpp`) in the registry's context; the physics scene does, so a snapshot
carries the simulation and a restore replays bit for bit.

## Recipe: add a component

Followed for `Spin`. Files, in order:

1. **The struct** in `ohao/ecs/components.hpp`: a plain aggregate. Default member
   initializers are the defaults, and files omit fields equal to them.
2. **Register it** in `registerSceneTypes` (`ohao/ecs/scene_types.cpp`):
   `r.type<T>("T").component().host(makeComponentOps<T>())`, a `.doc()` on the type and
   on every field, a `.unit()` from `knownUnits()` or `.unitless()`, and `.min()`/
   `.max()`/`.uiRange()` where values are bounded. `.singleton()` for one per world,
   `.runtime()` for derived state that is never saved. A unit not in the vocabulary goes
   into `kUnits` in `ohao/reflect/registry.cpp` (that changes both catalogue goldens).
3. **Its writer** (saved components): declare `writeFields(const T&, const T*, FieldSink&)`
   in `ohao/ecs/scene_writers.hpp` and define it in `scene_writers.cpp`, fields in
   registration order, each written only when it differs from `omit`
   (`if (!omit || !same(...))`). `makeComponentOps` finds it by overload.
4. **Tests** in `tests/scene/scene_tests.cpp`:
   - `complete<T>` in `t01_every_component_member_is_registered` (every member
     registered or skipped);
   - the name in `t01_authoring_and_runtime_split`;
   - random values in `buildRandomWorld`, so the round-trip, snapshot and
     writer-vs-generic tests (t02, t03, t10, t21) cover it.
5. **A corpus case:** `tests/scenes/invalid/NN_<name>.scene.json` with the mistakes an
   agent would make, and `NN_<name>.errors.json` listing the exact `code` and `path` of
   each (plus `suggestion` where one is expected).
6. **Goldens**, after reading the diff: `scene_tests --update-golden` (engine catalogue
   and scene schema), `reflect_tests --update-golden` if a unit was added, then
   `python tests/cli/check_cli.py --update` (the `schemaHash` in two expected outputs)
   and check that the hash is the only change.
7. **Docs:** the component table in `docs/agents/ohao-cli.md`.

Changing an existing saved component later: bump `.version(n)` in its registration, add
an `.upgrade()` from the previous version, and add a file written by the old version to
`tests/scenes/v1/` with its expected values in `t08_v1_corpus_loads_with_expected_values`.

```console
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/reflect_tests.exe > /dev/null
```

## Recipe: add a system

Followed for the spin system.

- **Engine-provided gameplay system** (no GPU; every world runs it):
  1. `ohao/ecs/<name>_system.hpp/.cpp` with a free function
     `void update<Name>(entt::registry&, float dt)`. Iterate a view; write components
     through `patch`/`replace` so the sync systems see the change. Read time only from
     `dt` (it runs once per fixed step); do nothing when `dt` is 0.
  2. Install it in `World::World()` (`ohao/ecs/world.cpp`) with `addSimulate`.
  3. Add its files to the t13 list in `tests/reflect/reflect_tests.cpp` (no
     `reflect/`/`api/` includes).
  4. Tests in `tests/systems/systems_tests.cpp`: the formula bit for bit, still entities
     untouched, determinism, and a pass through the API (`world.step`, snapshot/restore).
- **Host system** (needs a renderer; a sync system, no dt): an `install<Name>(World&, …)`
  that adds it with `addSyncBefore("transform", …)` or `addSyncAfter("transform", …)`, guarded against a
  second install (see `installInstantiate`). It must pick up entities that existed
  before it was installed.

```console
$ build/Release/systems_tests.exe > /dev/null
```

## Recipe: add an API command

Followed for `entity.duplicate`. A command is one op in a `world.apply` batch; a new
*method* is the other case below.

1. **The function** in `ohao/api/commands.cpp`:
   `bool entity<Name>(Batch& b, const JsonNode& cmd)`.
   - First `checkKeys` with every member it accepts.
   - Then read and check everything (`target`, `readRef`, `optBool`, `optString`),
     reporting with `ctx.error(code, node, message, expected, got)`. Return false before
     changing anything.
   - Then apply. Nothing after this point may fail.
   - With `b.wantInverse`, write the inverse commands (`invBegin`…`endObject`).
   - If it creates an entity, set `b.created[b.index]` so `"$k"` and `results[k]` work.
2. **Register it:** its name in `kOps` and its function in `kCommandFns`, at the same
   index (a `static_assert` checks the counts).
3. **Tests** in `tests/api/api_tests.cpp`:
   - a case in `t09_each_command_applies_and_returns_its_inverse` (it applies, has its
     effect, and its inverse restores the save byte for byte);
   - a focused test for its errors and edges.
4. **Docs:** the command table in `docs/agents/ohao-cli.md`; a CLI case in
   `tests/cli/cases.json` if the CLI exposes something new.

```console
$ build/Release/api_tests.exe > /dev/null
```

## Recipe: add an API method

Followed for `world.hash` and `world.diff` (sub-project 4, step 7) and the four `input.*`
methods (step 8). One definition makes a method everywhere at once: the native JSON-RPC
method, the MCP tool (`.` becomes `_`), the catalogue entry and its JSON Schema, the CLI's
`run --script` line.

1. **Its params struct** in `ohao/api/params.hpp`, registered in `params.cpp` with a doc
   on every field, limits, units and `.required()` where a member must be given;
   `.maxItems(n)` on an array a client could make unbounded. Free JSON that another
   decoder checks is a `reflect::JsonRef`. This one definition is the validation
   (`decodeParams`), the catalogue's JSON Schema and the MCP tool's input schema.
2. **The handler** `bool name(Session::Impl&, const JsonDocument&, const JsonNode& params,
   DecodeContext&, JsonWriter& out)` in `ohao/api/session.cpp` (or a file of its own under
   `ohao/api/`, declared in `internal.hpp`): first `decodeParams(s, params, ctx, p)`; a
   rule on the params alone fails with `badParams(s)` (JSON-RPC -32602), anything else is
   a domain error (`ctx.error(code, node, message, expected)`, then `return false`). A path
   in params goes through `usePath` (the server's sandbox). A long loop checks
   `s.cancelRequested()` between whole steps and reports `s.control->progress`. Anything
   that needs a GPU goes through `s.render` (the session's `api::Host`), never the
   renderer: the API stays GPU-free.
3. **Register it:** a `MethodDoc` in `buildMethods` (name, params struct, a compact result
   shape, one doc line: the MCP description agents read) and the handler in `kDispatch`,
   with `mutates = true` if it changes the world (a window host in lockstep redraws only
   after such calls).
4. **Tests and goldens:** its behaviour in `tests/api/api_tests.cpp`; the method count in
   `t09_method_table_dispatch_and_errors`; an example in `EXAMPLES` of
   `tests/serve/check_serve.py` (its schema through `jsonschema`, the server accepting it);
   then the goldens, after reading the diff: `python tests/cli/check_cli.py --update` and
   `python tests/serve/check_serve.py --update` (`schemaHash`, `tools/list`). `rpc_tests`
   keeps `tools/list` under 24 KB, so keep the doc line to one sentence.
5. **Docs:** the method table in `docs/agents/ohao-cli.md`, and `docs/agents/ohao-serve.md`
   if it changes how a session goes.

```console
$ build/Release/api_tests.exe > /dev/null
$ build/Release/rpc_tests.exe > /dev/null
```

## Recipe: add a render pass (and its shader)

Followed once in sub-project 4, step 11, with a throwaway compute pass (the mean
luminance of each 8x8 tile of the lit HDR image): it built, ran validation-clean through
a resize and teardown, left every golden exact, and was then removed, since nothing read
its output. Deferred passes live in `ohao/render/deferred/`; `SSRPass` is the compute
model to copy (Slang, on the `rtc` helpers of `ohao/render/rt/rt_compute.hpp`), `SkyPass`
the fullscreen raster one.

1. **The shader** under `shaders/<area>/<name>.<stage>` (`.comp`, `.vert`, `.frag`,
   `.rgen`, … — the glob in `shaders/CMakeLists.txt`). The next build picks it up and
   compiles it with `glslc --target-env=vulkan1.3 -I shaders -I shaders/includes`; the
   SPIR-V is named by flattening the path: `postprocess/bloom_threshold.frag` →
   `postprocess_bloom_threshold.frag.spv`. Include shared code by its path under `shaders/`
   (`#include "includes/common/color.glsl"`); every `.spv` is rebuilt when any file under
   `shaders/includes/` changes, and `ohao doctor`'s `shaders.fresh` follows includes
   transitively. Fullscreen raster passes reuse `postprocess/fullscreen.vert` (no vertex
   input, `vkCmdDraw(cmd, 3, 1, 0, 0)`). **New shaders are Slang** (`.slang`; GLSL only
   for a pass not being rewritten): an entry point is a function under a
   `[shader("compute")]` attribute line (further `[...]` attribute lines may follow), and
   it keeps its name in the SPIR-V (pass it as `pName`). `slangc -target spirv -profile
   spirv_1_6 -matrix-layout-column-major -I shaders` builds one SPIR-V per entry:
   `bench/gpu_calibrate.slang` (one entry) → `bench_gpu_calibrate.slang.spv`, a file with
   several entries → `<flattened>.<entry>.spv`; a `.slang` without an entry is a module
   (`import shared.layout;`). Constants and structs C++ shares with a shader go in
   `shaders/shared/layout.json`, then `python tools/gen_layout.py` writes
   `shaders/shared/layout.slang`, `layout.glsl` and `ohao/gpu/shared_layout.hpp` (std430,
   every offset asserted); `check_layout` fails while they are out of date.
2. **The class** `<Name>Pass : RenderPassBase` (`render_pass_base.hpp`): `initialize`
   (set `m_device`/`m_physicalDevice`, then create everything; return false on any
   failure), `cleanup`, `execute(cmd, frameIndex)`, `onResize`, `getName`. Use the base
   helpers (`createSampler`, `safeDestroy`; `createRenderTarget`, `transitionImage` for
   GLSL-era passes) or, for a Slang compute pass, `rtc::createImage`, `rtc::transition` and
   `rtc::createComputePipeline(device, spvName, entry, setLayout, pushBytes, …)` (it loads
   the SPIR-V by its flattened name). Push constants stay within 128 bytes, the size every
   device guarantees. The destructor calls `cleanup()`.
3. **Descriptors written at record time need one set per frame in flight**: a
   `std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>`, a pool sized ×
   `MAX_FRAMES_IN_FLIGHT`, and `execute` writing only `m_descriptorSets[frameIndex]`
   (`if (frameIndex >= MAX_FRAMES_IN_FLIGHT) return;`). A single set rewritten every frame
   races the frame still reading it (VUID 03047).
4. **Barriers are the pass's own** unless it joins the render graph: transition its output
   before writing (`UNDEFINED` → `GENERAL` for a storage image it rewrites whole) and to
   `SHADER_READ_ONLY_OPTIMAL` after, with the stages of its readers. Inputs keep the
   layouts their producers leave: the lit HDR image in `SHADER_READ_ONLY_OPTIMAL`, the
   G-buffer depth in `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. (Passes that run before lighting
   go through `RenderGraph` instead: `addComputePass`, `readComputeTexture`, imports
   refreshed in `importGraphTextures`.)
5. **Wire it into `DeferredRenderer`** (`deferred_renderer.{hpp,cpp}`): a
   `std::unique_ptr<…Pass>` member; create it in `initialize` (non-fatal: log a `Warn`
   and `reset()` on failure, as SSR does); record it in `render()` at its place in the
   frame (after lighting: sky, particles, SSS, SSR, post, gizmo) inside
   `timerBegin("name") … timerEnd()`; resize it in `onResize`, and hand it again any view
   of another pass that a resize recreates. A pass left at its creation size is invisible
   to validation and wrong on screen. A graph pass is timed under its `addPass` name.
   Either way the name goes in the **pass registry** (`ohao/render/frame/pass_registry.hpp`,
   `kPasses`, at its place in frame order, lowercase and dotted: `rt.gi.denoise`): the
   timer (`GpuPassTimer`, `render/frame/gpu_pass_timer.hpp`) times only registered names,
   logs an unknown one once as an error, and `engine_tests`' `sp2b_t01` fails. The pass's
   time then shows in `render.stats` and `render_bench`; give it a budget in
   `tests/bench/render_budgets.json` (spec 2b, "How budgets are gated").
6. **Check it:** build (the shader comes with any target that renders); run validation
   with a resize; run the goldens — a pass that changes pixels changes the deferred goldens
   on purpose, so look at the diff (`build/golden_diff/`), then `--update`; and
   `deferred_determinism_gpu_test` (deferred frames must stay bit-identical run to run).
   A post effect that agents switch on also needs its name in `PostEffect`
   (`ohao/api/params.{hpp,cpp}`) and `kEffects` (`session.cpp`), and its toggles in
   `runtime/frame_capture.cpp` and `runtime/live_host.cpp`.

```console
$ python tests/validation/check_validation.py > /dev/null
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null
```

## Recipe: register a pass and give it a budget

Followed for every pass sub-project 2b added (`cull.*`, `hiz.build`, `lights.cluster`,
`capture.preview`, `pt.present`, …). A pass is not done when it renders: it is done when
it is measured under its name and gated by a budget.

1. **The registry entry** in `kPasses` (`ohao/render/frame/pass_registry.hpp`): name, kind
   (`Transfer`, `Raster`, `Compute`, `RayTracing`) and one line saying what it does and
   what it costs at the defaults, at its place in frame order. The name is lowercase and
   dotted, with a family prefix when it has siblings (`rt.gi`, `rt.gi.denoise`), so a
   budget can cover a family with a pattern (`"cull.*"`).
2. **Time it**: `timerBegin("name") … timerEnd()` around its commands (`DeferredRenderer`),
   or its `addPass` name if it is a graph pass. `engine_tests`' `sp2b_t01` fails on a pass
   recorded out of registry order or timed twice; an unregistered name is logged once as
   an error and not timed.
3. **If a setting turns it off** (`render.settings`), the switch belongs in
   `VulkanRenderer::RenderSettings`, `setRenderSettings` and `renderSettings` (read back
   from the subsystem, never cached), its reflected twin in `ohao/api/params.{hpp,cpp}`
   (`RenderSettings`, with limits), and the two conversions in `runtime/frame_capture.cpp`.
   Turn it off by not registering it in the frame's graph, so it costs nothing and leaves
   the timings; `sp2b_t14_switches_turn_their_passes_off` is the test to extend.
4. **The budget** in `tests/bench/render_budgets.json`, one entry per scene it runs in:
   `{"scene": "cornell_720p", "name": "lights.cluster", "passes": ["lights.cluster"],
   "budgetMs": 0.03, "gatedFrom": 13}`. A budget's time is the sum of its passes' medians
   over 100 frames (`"x.*"` is every pass named `x.<something>`). `gatedFrom` is the build
   step that delivers the pass: `render_bench --check` gates budgets with `gatedFrom` ≤
   `currentStep` and reports the rest as `pending` with their measurement. Measure first
   (`render_bench --only SCENE`), set the budget from the spec's frame budget, never from
   the measurement alone. A pass budget may be moved between passes of the same scene by
   the step that measures them (record it in `"moved"`); a frame, capture or total budget
   rises only by Frank's ruling.
5. **Check it**: the names below must all match registered passes, and `render_bench
   --check` must pass five consecutive clean runs (a run flagged `noisy` is no verdict;
   `python tools/bench/wait_quiet.py` first).

```console
$ python -c "import json, re, fnmatch; names = re.findall(r'\{\"([a-z.]+)\", PassKind', open('ohao/render/frame/pass_registry.hpp').read()); pats = [p for b in json.load(open('tests/bench/render_budgets.json'))['budgets'] for p in b.get('passes', [])]; print(len(names), 'passes registered; budget names matching none:', [p for p in pats if not fnmatch.filter(names, p)])"
29 passes registered; budget names matching none: []
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
```

## Recipe: add a level

Followed for the blockout (`assets/levels/blockout_room.glb`, sub-project 5).

1. **Author it in Blender** (meters; glTF's +Y up): one object per node, hints as Object
   Properties → Custom Properties, which the exporter writes as the node's `extras`:
   `col` (`box`, `sphere`, `capsule`, `convex`, `mesh`, `none`; mesh nodes default to
   `mesh`), `body` (`static`, `dynamic`, `kinematic`), `mass`, `fric`, `rest`, `vis: 0`
   (collision only), `spawn` (a tag: `player`, `enemy`, …) and `socket` on empties, any
   saved component by name (`{"Spin": {...}}`). Unreal names work too: `UCX_<node>[_NN]`
   hulls, `SOCKET_<name>` empties. Checked-in levels are generated by a script in
   `tools/level/` (ours, MIT; `blockout_room.py` runs headless in Blender 5.2.2 and
   rebuilds the file byte for byte); third-party art is staged under the git-ignored
   `assets/local/` and never committed.
2. **Check the hints:** `tests/level/check_level_assets.py` validates every checked-in
   level's `extras` against `tests/level/extras_schema.json` (`ohao schema level-extras`);
   pin the numbers later tests assert in its contract, as the blockout does.
3. **Look at it without a scene:** `ohao level <file>` prints `level.info`: node, mesh
   and triangle counts, colliders by shape, bodies by motion, spawns in node order with
   their facing, and the warnings (`W_LEVEL_EXTRAS` for an unknown lower-case key,
   `W_LEVEL_NO_PLAYER_SPAWN`, a dynamic mesh made convex). An unknown capitalised key or a
   bad value is `E_LEVEL_EXTRAS` (exit 2): the node loads without it.
4. **Place it:** a scene entity with `Level {asset: {path}, lightScale, collision}`; one
   line places the whole file, and saving writes only that line.
5. **Check that it collides:** `physics.raycast` down from above each spawn must hit the
   ground at the spawn's height (the blockout's spawns: within 1 mm,
   `level_physics_tests`); `physics.info` counts the bodies.
6. **A golden** of the scene, as below (`cli_blockout_room_deferred`): deferred, exact.

```console
$ build/Release/ohao level assets/levels/blockout_room.glb > /dev/null
$ python tests/level/check_level_assets.py > /dev/null
```

## Recipe: add a golden scene

Followed for `cli_helmet_game_deferred` (sub-project 4, step 11).

1. **The scene** is a checked-in `.scene.json` (`assets/scenes/`); a golden renders it
   through `ohao run`, so everything about the render is on the command line: `--mode`,
   `--frames` (deferred: from a reset, the last kept; with TAA give enough frames to
   converge), `--post`, `--exposure`, `--quality` (deferred: `realtime` or `reference`),
   `--spp`/`--denoise` for the path tracer.
2. **The entry** in `tests/golden/manifest.json`: `name`, `command` (`{out}` is the
   image the harness compares), `golden` (`tests/golden/<name>.png`), the tolerance and a
   `_note` saying what the image is and why its tolerance is what it is. **Deferred
   scenes are exact** (`max_abs_diff` 0, `max_diff_frac` 0): deferred rendering is
   deterministic. Path-traced ones get the noise floor `--selftest --json` measures, never
   more.
3. **Generate, then look:** `--only <name> --update` writes the golden; open the PNG and
   check it shows what the note says before keeping it. Then run `--only <name>` twice
   more: an exact scene must pass both.
4. The scene count in `AGENTS.md` ("Render goldens").

```console
$ python tests/golden/render_golden.py tests/golden/manifest.json --only cli_helmet_game_deferred > /dev/null   # gpu
```

## Recipe: add a test suite

Followed for `systems_tests`.

1. `tests/<name>/<name>_tests.cpp`:
   - `#include "common/harness.hpp"` for `CHECK`/`RUN`;
   - one `static void` function per test, named for what it proves;
   - `main` runs them and ends with `return ohao::test::finish("<name>_tests");`, which
     prints `Results: P/T passed` and the one-line JSON summary
     `{"suite","passed","failed","failures":[…]}` and returns nonzero on any failure.
2. `tests/<name>/CMakeLists.txt`:
   - `add_executable`, then `target_link_libraries` with the narrowest library that
     covers it, plus `ohao_test_harness`;
   - `OHAO_SOURCE_DIR` as a compile definition if it reads repo files;
   - `/FORCE:MULTIPLE /IGNORE:4088` on MSVC;
   - `ohao_add_test(<name>_tests LABEL quick COMMAND <name>_tests)` (`LABEL gpu GPU` if it
     creates a device; `LOCKS <name>` for any fixed scratch path it shares with another
     test);
   - `add_dependencies(<target> shaders)` if it renders.
3. The root `CMakeLists.txt`: an `option(BUILD_<NAME>_TESTS … ON)` and
   `add_subdirectory(tests/<name>)` next to the others.
4. List it in the gates of `AGENTS.md` and in the `verify` skill. `python tools/check.py`
   runs it from then on; `check_tests` fails if the executable is built but not
   registered.

```console
$ build/Release/systems_tests.exe | tail -n 1
{"suite":"systems_tests","passed":9,"failed":0,"failures":[]}
```
