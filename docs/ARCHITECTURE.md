# OHAO architecture

A map of the code: the libraries and which way they depend, how one frame flows from
game state to pixels, and recipes for the changes people make most often. Each recipe
was followed once, end to end, when it was written (sub-project 3, step 10: the `Spin`
component, the spin system and `entity.duplicate`; sub-project 4, step
11: an API method, a render pass and its shader, a golden scene).

## Libraries

All under `ohao/`, each a static library with its own `CMakeLists.txt`, and the games under `games/`. Sources are
globbed with `CONFIGURE_DEPENDS`: a new file is picked up by the next build.

| Library | Directory | Depends on | What it owns |
|---|---|---|---|
| `ohao_core` | `core/` | glm | the logger (`OHAO_LOG`, the ring, sinks), the time model (`Timeline`, `FixedStepClock`: lockstep or realtime, paused, the frame number), input (`InputMap`: actions, axes, the virtual device injection uses; the `Key` name table), the render handles a component holds (`render_handles.hpp`), `Result`, concepts, small shared types |
| `ohao_reflect` | `reflect/` | glm (fast_float privately) | the type registry, schema/catalogue export, the JSON writer, reader and layout, typed decode/encode. Knows nothing of ECS or GPU. |
| `ohao_asset` | `asset/` | assimp, ufbx, stb_image (`external/stb_image.cpp`, its one implementation) | asset loading into CPU `Model`s, procedural meshes. No Vulkan: a raster pipeline reads `Vertex` through `render/scene/vertex_input.hpp` |
| `ohao_gpu_vulkan` | `gpu/vulkan/` | `ohao_core`, Vulkan, VMA | the Vulkan layer under the renderer: the swapchain and present waiting, bindless textures, the allocator, `vk_utils` |
| `ohao_renderer` | `render/` | `ohao_gpu_vulkan`, `ohao_asset`, `ohao_core` (NRD, OIDN, NGX optional) | `VulkanRenderer` (`render/renderer/`: the facade, device and queues, frame dispatch, environment upload), `RenderScene` (the persistent GPU scene), deferred passes, the path tracer, denoisers, the render graph |
| `ohao_ecs` | `ecs/` | `ohao_core`, `ohao_asset`, `ohao_reflect`, `ohao_physics`, EnTT (simdjson privately); never the renderer (one layer: the layer check holds it) | `World` (simulate and sync phases, its `InputMap` bound from `InputActions`), components (`MeshRenderer` holds `core/render_handles.hpp`'s ids), systems (spin, fly controller, transform, physics), the scene codec, snapshots, the world hash and diff, the scene view. The physics system (`physics_scene`, installed by `installPhysics`) builds Jolt bodies from `Collider`/`RigidBody`; `physicsScene(world)` reaches it. |
| `ohao_anim` | `anim/` | `ohao_core`, glm (simdjson privately) | skeletal animation without ECS or Vulkan: glTF skins and clips (`gltf_anim`), binding clips to skeletons by joint name, sampling, `.animgraph.json` graphs (parse, bind, validate with JSON pointers), one layer's step (`stepLayer`) and a pose (`evaluatePose`), the skinning palette |
| `ohao_level` | `level/` | `ohao_core`, glm (simdjson privately) | `readGltfStructure`: a glTF file's node tree, `extras`, lights, cameras and mesh counts, read from its JSON |
| `ohao_world_render` | `world_render/` | `ohao_ecs`, `ohao_renderer` | the world into the renderer: instantiate (MeshSources into meshes, `installInstantiate`), the render extract, the effects' draw (`vfx_draw`), in-memory models (`model_spawn`), framing, what a game presents (`present.hpp`: its HUDs, drawn by the hosts, and its presented spheres, by `vfx_draw`). `engine::installRendering` installs it. |
| `ohao_level_world` | `level/` | `ohao_level`, `ohao_ecs`, `ohao_reflect` | levels in the world: hints (`level_hints`), the `level` sync system (`installLevels`), and what `level.info`/`level.reload` read (`levelSummary`, `levelDiagnostics`, `reloadLevel`) |
| `ohao_game_api` | `game/` | `ohao_reflect` | the game API (step 3i): `game::Module` (`registerTypes`, `install`, `methods`) and a game's method row (`game::Method`, `game::Call`, `decodeParams`). A host plays one module or none. |
| `ohao_api` | `api/` | `ohao_ecs`, `ohao_level_world`, `ohao_reflect`, `ohao_game_api` | `Session`: the method table (the engine's rows, then the game's) and its params structs, edit commands, queries, the path sandbox; `level.*` and `physics.*` through the hooks the World's installers leave (`level/level_system.hpp`, `ecs::physicsScene`). GPU-free. |
| `ohao_rpc` | `rpc/` | `ohao_api`, `ohao_core` | JSON-RPC 2.0 over newline-delimited streams (stdio, or a local named pipe: `PipeTransport`) and the MCP adapter (`ohao serve`): reader and engine threads, the bounded queue, cancellation, progress, the log sink. CPU-only: no Vulkan, GLFW or renderer header. |
| `ohao_runtime` | `runtime/` | `ohao_api`, `ohao_core`, `ohao_ecs`, `ohao_world_render`, `ohao_renderer`, glfw | `LiveHost`, the window host (window, renderer, World and Session, one loop for `ohao_game` and `ohao serve --window`); `captureFrame`, the one capture procedure both hosts run; capture images (crop, scale, a fast PNG writer, JPEG); `applySceneView`; frame statistics |
| `ohao_physics`, `ohao_audio` | `physics/`, `audio/` | `ohao_core`; Jolt / miniaudio | physics (`physics/backend`: the `IPhysicsBackend` over Jolt that the ECS's physics scene builds, steps and joins bodies through) and audio (the mixer, the bank and the reverb; what plays is the ECS's voice schedule, played by a window's `runtime::AudioPlayer`) |
| `ohao_host` | `tools/ohao/` | `ohao_runtime`, `ohao_api`, `ohao_ecs`, `ohao_renderer` | `RenderHost`, the headless `api::Host` behind `render.capture` |

Games (`games/`, each `ohao_add_game(<name>)`, cmake/ohao_games.cmake; above every engine library:
`cmake/ohao_layers.cmake` fails configure when an engine library links or includes one, or when a game includes any
engine header but `game/api.hpp`, the game API's one header, which is also its precompiled header). The catalogue
(`ohao schema`, `schema.get`) names each type, enum, method and channel a game owns (`module`). `ohao_sample`
(`games/sample`): the smallest game, targets that spin until shot and then fall, `sample.targets`, `sample.hit`;
it is `--game build/Release/libohao_sample.so` with `assets/scenes/sample_targets.scene.json`. `ohao_slice`
(`games/slice`, namespace `ohao::slice`), the game the engine is built with: combat,
the enemy AI, grenades and rounds, the view model, the arms and enemy drivers, its HUD, its 9 components and 7 event
payloads (`types.cpp`), and `combat.info` and `combat.damage` (`methods.cpp`, params in its own registry);
`slice::module()` is its `game::Module`, and `ohao_game_module` its entry. Its objects are compiled once
(position independent) and linked two ways: `ohao_slice`, a module (`libohao_slice.so`) that `ohao` and `ohao_game`
load at start (`game/loader.hpp`: read into a memfd, `dlopen` RTLD_NOW and RTLD_LOCAL) and that links no engine
library, its references to the engine resolving against the host executable, which exports its symbols
(`ENABLE_EXPORTS`), so the process holds one engine; and `ohao_slice_static`, for the benches that measure its
internals. A loaded module is never unloaded: EnTT keeps a pool for each component type, with no removal, and a
module's pools run its code. `game/install.hpp` is a game's life on a World: `installGame` records what an install
added (systems, snapshot hooks, drivers, presenters, the physics control rule) and `uninstallGame` removes it, then
the module's `uninstall` its own; `reloadGame` (`game.reload`) swaps in a newer build of the game, the world kept: the
new build reads the components, channels and hook states in place, so their layouts must not change
(`core/layout_key.hpp`: TypeInfo::layout, SnapshotHook::layout), or the reload is refused and the old build kept. Type identities are names, not addresses (`core/type_key.hpp`, EnTT's `type_hash`), so
the executable, the module and a later build of it agree on every type.

Programs: the examples (`examples/`: `cornell_box`, `model_viewer`, `turntable`,
`env_demo`, `ohao_game`), the CLI (`tools/ohao`), `scene_bench`, `serve_bench`,
`render_bench`, `level_bench`, `physics_bench` and `feel_bench` (`tools/bench`; they share the bench10k builder,
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
  nothing from `reflect/` or `api/`. No test enforces it since the cut (2026-11-08): keep it by hand.
- **The API is GPU-free.** Anything that needs a renderer goes through the session's
  `api::Host` (`Session::setHost`): the headless `RenderHost` or the window's `LiveHost`.
- **The engine prints only through the logger.** `OHAO_LOG(level, channel, fmt, args...)`
  (`core/log.hpp`): levels `Trace` … `Error`, channels `render`, `gpu`, `validation`,
  `ecs`, `asset`, `physics`, `audio`, `api`, `rpc`, `runtime`, a `std::format` string.
  Below the threshold a call costs one load and a compare; records go to a 4,096-record
  ring and to stderr as `<L> <channel>: <message>`. Nothing under `ohao/` writes to
  stdout, `std::cerr` or `printf` (`ohao serve`'s stdout is the protocol).
  `OHAO_LOG_LEVEL=debug` shows the detail lines.

## One frame

The same `World` runs headless (CLI, tests, examples) and windowed (`ohao_game`). Its
systems come in two phases. Each phase's order is computed (`World::addSimulate`/`addSync`, step 3h) from what its
systems declare (`ecs::SystemOrder`: `after`, `before`, `readsCues`) and, in simulate, the event handles they hold:
a channel's writers before its typed readers, a channel with a cue before every system that reads cues. Ties go to
the lesser name (Kahn's algorithm), so the order is a function of the systems installed, not of the host; a cycle
throws at install, naming it. `OHAO_LOG_LEVEL=debug` logs each phase's order. Below, the play scene's:

```
the timeline (core/timeline.hpp) decides the steps: lockstep, on request (world.step);
realtime, whole fixed steps from wall time (paused: none)
for each step (api::Session::step: world.step, input.replay, a window's realtime steps):
  World::stepInput(events)   device events since the last step, then injected ones
                             (InputActions rebinds the map first)
  World::simulate(fixedDt)
    fly_controller  engine-provided: input + FlyController -> Transform   every World (before physics)
    spin            engine-provided: Spin -> Transform (patch)        every World (before physics)
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
                    ground's Collider.surface) into the World's event log
                    (physics.*)
    combat          first the enemy AI (every Enemy in id order: sight   the slice's (games/slice)
                    ray, Idle/Chase/Attack/Dead, MoveIntent for the next
                    step, WeaponTrigger for this one); then
                    every Weapon in id order: trigger (a player's input
                    fire/reload, else WeaponTrigger), cooldown, reload,  (after installPhysics);
                    bloom and recoil recovery, at most one round: a ray  nothing for a world
                    from the eye (PhysicsScene::raycastFrom: not the     without a Weapon
                    shooter, not triggers) -> Health of what it hit or
                    its ancestor; recoil -> PhysicsScene::turnPlayer;
                    WeaponState written; shot/hit/damage/death/reload
                    events (slice.*, each with its cue); last
                    the round (a world with Round and the                 WorldSnapshots: api::Session
                    WorldSnapshots api::Session installs): round_start   (ecs::installWorldSnapshots)
                    and a snapshot
                    at its first step's end; the player dead for
                    restartDelay, or `restart` pressed -> the snapshot
                    restored at this step's end (restart; both by
                    World::defer, between steps); every Enemy
                    dead -> win, then waits for `restart`; within each
                    weapon, a round lights its Muzzle light for
                    Muzzle.time (WeaponState.flash), then intensity 0
    animation       every Animator in id order: its graph's driver        installed by installAnimation
                    (enemy, arms: the slice's) writes AnimParams from     (installEngine)
                    saved state and the step's slice events;
                    each layer takes its first holding transition (a
                    parameter, an event, its clip's end), advances its
                    integer steps and fade, and emits the clip events it
                    crossed (anim.clip_event)
    audio_schedule  the step's physics, combat and animation events      installed by installAudio
                    -> audio.* events (play with a voice, variant,       (after its typed readers'
                    occlusion and outcome; stop for a stolen voice; bus  writers and every cue
                    ducks; the round's snapshot; the listener's zone and channel's: readsCues)
                    bed; occlusion re-casts; stop_world on a restart or
                    restore), by the bank's priorities, caps and holdMs;
                    its state rides the World's snapshot and world.hash
    vfx             the step's cues the vfx set binds (tracers, impacts,  installed by installVfx
                    decals) and VfxEmitters -> effect records; its state  (after animation; readsCues)
                    rides the World's snapshot and world.hash
    then the step's events become history (ecs/events.hpp): each system reads its channels'
    events for the step (EventReader::read); one emitted between steps (an API call, a body
    removed in a sync) is the next step's, carried by a snapshot and hashed until it runs;
    events.read reads the history, the last 8,192, by seq
    then what the step deferred runs (World::defer: a round's snapshot, its restart): between
    steps, so nothing snapshots or restores inside a step
  a game's fixedUpdate hook (LiveHost), reading the step's input        writes with patch/replace
after the frame's steps (LiveHost):
  runtime::AudioPlayer  (LiveConfig::sound, ohao_game without --mute) the audio.* events
                      since its cursor -> the AudioSystem, one step after their step on a
                      clock anchored to the device (late: at once, counted); the bank's files
                      decoded when the bank loads (assets/local/audio, then assets/audio);
                      a bus's level is the schedule's snapshot and ducks unless a
                      session's audio.mix overrides it (listening only: device-side,
                      never the world, the schedule or a snapshot)
once per batch of steps and before each render:
World::sync()
  level           Level components -> the entities they expand         installed by installLevels
                                                                       (before transform)
  instantiate     MeshSource/Material -> MeshRenderer,        installed by a host with
                  meshes and materials registered in the RenderScene   a renderer (after level)
  player_spawn    new or changed PlayerControllers -> characters, at  installed by installPhysics
                  the player spawn (so a play scene's first frame is   (after level, before
                  the spawn's view)                                    transform)
  transform       Transform + Parent -> WorldTransform; tags RenderDirty, MovedLastFrame
  physics         dirty colliders -> Jolt bodies; moved entities move     installed by installPhysics
                  their bodies (no step: that is the simulate system);    (after transform)
                  after a restore, the snapshot's simulation back in
                  place (Jolt's state, characters, players)
  interpolate     World::renderAlpha < 1: what the last step moved    installed by installPhysics
                  (players, heads, dynamic bodies) and its subtree ->  (after physics)
                  RenderPose between its last two step poses; at 1
                  none (Transform, the hash, saved files untouched);
                  then the look lead: the input-driven player and its
                  subtree turned by World::pendingLook through
                  applyLook (the newest step's turn plus the lead,
                  never interpolated; nothing in lockstep)
  view_model      a ViewModel's subtree (the first-person gun) ->      the slice's (games/slice)
                  RenderPose in the drawn view: viewModelRemap (within (after interpolate, before pose)
                  depthScale of the eye, on screen as through fovDeg),
                  sway, bob and kick at World::presentationDt (0: the
                  rest pose); drawn as kDrawViewModel (no CSM cascade,
                  TLAS mask 4: camera rays only); aiming, in realtime
                  frames only, a DrawnFov on the camera (75 -> 55)
  pose            every Animator: layers sampled at (steps - 1 +        installed by installAnimation
                  alpha), blended by mask and mode -> AnimPose (runtime: (after interpolate)
                  model matrices, the palette and last sync's palette,
                  a posed box); JointAttach children -> RenderPose at
                  their joint, subtree included
  render_extract  MeshRenderer/Light -> RenderScene instance slots    installed by a host with
                  (at RenderPose when there is one)                    a renderer (after transform,
                                                                       interpolate, pose, instantiate)
                  and the light list; a SkinnedMesh entity first gets
                  its own mesh (registerSkinnedInstance), then each
                  frame its palette; setLights gives each point,
                  spot and area light an emitter instance (its
                  sphere or rectangle, seen by camera and mirror rays;
                  a spot's shines toward the camera by its cone:
                  setViewerPosition)
  vfx_draw        the effect records -> the frame's particles,         installed by a host with
                  tracers and decals                                   a renderer (after render_extract)
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
  a window's presents: the mode asked for (immediate, mailbox, fifo; Swapchain), each tagged
  with its frame number (VK_KHR_present_id) and confirmed by a waiter thread
  (vkWaitForPresentKHR, gpu/vulkan/present_waiter.hpp); a realtime window host stamps each
  frame (poll, view, submit, present) and the look movements it shows into its FrameLog
  (runtime/frame_log.hpp): render.stats' latency and pacing, ohao_game --latency, feel_bench;
  before polling it waits until at most LiveConfig::framesAhead frames are unpresented
  (presentToAwait, VulkanRenderer::waitForPresentOf; 1 with a look-led player), and before
  sync it sets World::setPendingLook to the look no step has consumed yet
```

The deferred frame, pass by pass, under its registry names (`render/frame/pass_registry.hpp`,
the order `render.stats`, a capture's `timings` and `render_budgets.json` use). Brackets
name the `render.settings` member that turns a pass off or reshapes it; a pass that is off
is not registered in the frame's graph and costs nothing:

```
skin                only instances whose palette changed: deform their own mesh copy,
                    last frame's position into the copy's boneWeights.xyz (kDrawSkinned)
skin.bounds         their meshlets' spheres from the deformed vertices
rt.blas.refit       their BLASes refit to the pose (rebuilt every 120 refits)
scene.upload        table rows, BLAS builds, TLAS build/refit, the draw table
csm                 shadow cascades, only when RT direct light does not run   [csm.cascades, csm.size]
cull.instances      early lists: last frame's visible draws in the frustum   [occlusionCulling]
gbuffer.early       G-buffer cleared, early lists drawn (instances; meshlets [geometry])
hiz.build           max-depth pyramid of the early depth
cull.late           every draw vs frustum and pyramid: the newly visible
gbuffer.late        late lists drawn
lights.cluster      froxel light lists (16x9x24, up to 256 each)
emissive.lights     the emissive triangles as lights, their power's mips (with the path tracer)
di.sample           ReSTIR DI, half res: candidates by RIS, last frame's reservoir, a visibility ray   [rtDirect.*]
di.trace            ReSTIR DI, half res: neighbours' reservoirs, the final shadow ray
di.shade            every pixel: two nearby samples through its own material, diffuse + specular
di.denoise          temporal accumulation, 3 a-trous steps stopped by luminance (realtime quality)
rt.gi               GI, half res 2 rays by default, every bounce through its radiance cache (resolved here)   [rtGI.enabled, .resolution, .rays]
rt.gi.denoise       reprojection, 4 a-trous (reflection 2), upsample (realtime quality)
lighting            RT direct light (or the froxel list + CSM) + IBL + GI into the HDR image (+ env specular, its weight)
vfx.expand          the effect records' particles in closed form, lit at their centres (sub-project 12)
vfx.sort            each alpha batch's particles back to front (integer keys)
sss                 subsurface blur on classified tiles   [sss]
transparent         blended meshes and effects back to front over the lit image, soft against
                    depth, each with its blend state; coverage into the mask TAA reads
post.bloom, post.taa, post.tonemap   HDR to RGBA8   [capture post; ev100, tonemap]  (bloom by energy: 0.04 of the frame)
gizmo               transform handles
hud                 the HudList over the final image (only when the list has items:      [capture hud]
                    a window always; a capture with hud: true)
capture.preview     only for an inline capture: box filter into staging
present | readback  window: blit to the swapchain (+ requested copy); headless: copy to staging
```

`quality: reference` swaps the realtime shadow and GI passes for their converging forms
(8 shadow rays to every light, 32 GI rays at full resolution, averaged over the frames).
A path-traced frame is the skin passes, `scene.upload`, the tracer's stages (`emissive.lights`,
which turns every emissive triangle into a light with its power, `pt.cache.update` and
`pt.cache.resolve` in the realtime profile, `pt.trace`, `pt.denoise`), then `pt.present` or
`pt.readback`.

The window host, `runtime::LiveHost` (`ohao_game`; `ohao serve --window`), runs this per
frame: poll the window; run the queued protocol requests (`rpc::Server::poll`, waiting up
to 16 ms while nothing steps); the timeline's steps (`Session::step` each, then the
`fixedUpdate` hook); the `update` hook; the game's sounds and its HUDs (`ecs::drawHuds`
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
3. **Nothing else for the file form:** the scene save is the generic reflection writer, so the
   registration is the component's only definition in a file.
4. **Check it by hand:** `ohao schema -o build/schema` shows the type with its docs and units; a
   scene that uses it saves, reloads and keeps its `world.hash`. `replay_test` holds gameplay replays with it in
   the hash.
5. **Docs:** the component table in `docs/agents/ohao-cli.md`.

Changing an existing saved component later: bump `.version(n)` in its registration and add an
`.upgrade()` from the previous version; load a file the old version wrote (`tests/scenes/v1/`) to
see it upgrade.

## Recipe: add a system

Followed for the spin system.

- **Engine-provided gameplay system** (no GPU; every world runs it):
  1. `ohao/ecs/<name>_system.hpp/.cpp` with a free function
     `void update<Name>(entt::registry&, float dt)`. Iterate a view; write components
     through `patch`/`replace` so the sync systems see the change. Read time only from
     `dt` (it runs once per fixed step); do nothing when `dt` is 0.
  2. Install it in `World::World()` (`ohao/ecs/world.cpp`) with `addSimulate(name, fn, {.after, .before})`,
     naming the systems whose results it reads and those that read its own (its events order themselves).
  3. Its files include nothing from `reflect/` or `api/`. What it tells other systems or agents is an event:
     a payload struct registered in `ecs/event_types.cpp` (`.event()`, entities by id with `.entity()`), a
     writer taken at install (`world.events().writer<T>("<system>.<what>", "<system>")`), and readers taking
     theirs the same way; never a log of its own.
  4. Check it by hand through the API (`world.step`, then the entities' values; still entities
     untouched); `replay_test` holds its determinism if the play scene runs it.
- **Host system** (needs a renderer; a sync system, no dt): an `install<Name>(World&, …)`
  that adds it with `addSync(name, fn, {.after = {…}, .before = {…}})`, guarded against a
  second install (see `installInstantiate`). It must pick up entities that existed
  before it was installed.

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
3. **Check it by hand:** apply it through `world.apply`, then apply its inverse; the save must be
   the same bytes as before.
4. **Docs:** the command table in `docs/agents/ohao-cli.md`.

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
3. **Register it:** one row in `methodTable` (`ohao/api/session.cpp`): name, params struct, a compact
   result shape, one doc line (the MCP description agents read; keep it one sentence), the handler, and
   `mutates = true` if it changes the world (a window host in lockstep redraws only after such calls).
4. **Docs:** the method table in `docs/agents/ohao-cli.md`, and `docs/agents/ohao-serve.md`
   if it changes how a session goes.

## Recipe: add a render pass (and its shader)

Followed once in sub-project 4, step 11, with a throwaway compute pass (the mean
luminance of each 8x8 tile of the lit HDR image): it built, ran validation-clean through
a resize and teardown, left every golden exact, and was then removed, since nothing read
its output. Deferred passes live in `ohao/render/deferred/`; `SSSPass` is the compute
model to copy (Slang, on the `rtc` helpers of `ohao/render/rt/rt_compute.hpp`), the post
stack's display pass (`postprocess/tonemap.slang`) the fullscreen raster one.

1. **The shader**, Slang, under `shaders/<area>/<name>.slang` (the glob in
   `shaders/CMakeLists.txt`; every shader is Slang, there is no GLSL). The next build picks
   it up and compiles each entry point with `slangc -target spirv -profile spirv_1_6
   -matrix-layout-column-major -I shaders`: an entry point is a function under a
   `[shader("stage")]` attribute line (further `[...]` attribute lines may follow), and it
   keeps its name in the SPIR-V (pass it as `pName`). One entry gives `<flattened path>.spv`
   (`bench/gpu_calibrate.slang` → `bench_gpu_calibrate.slang.spv`), several
   `<flattened path>.<entry>.spv` (`core/gbuffer.slang` → `core_gbuffer.slang.opaque.spv`); a
   `.slang` without an entry is a module (`import includes.scene.scene_data;`). A SPIR-V is
   rebuilt when its file or one it imports changes (slangc's `-depfile`), and `ohao doctor`'s
   `shaders.fresh` follows imports transitively. The same compile saves the entry's reflection
   (`build/shader_reflection/`), from which the build writes `ohao/render/shader_bindings/<flattened
   path>.hpp`: the pass names its bindings and push constants from it. Fullscreen raster passes reuse `postprocess/fullscreen.slang` (no vertex
   input, `vkCmdDraw(cmd, 3, 1, 0, 0)`); a vertex shader reading firstInstance or firstVertex
   uses `SV_VulkanInstanceID`/`SV_VulkanVertexID` (HLSL's `SV_InstanceID` leaves the base out).
   Constants and structs C++ shares with a shader go in `shaders/shared/layout.json`, then
   `python tools/gen_layout.py` writes `shaders/shared/layout.slang` and
   `ohao/gpu/shared_layout.hpp` (std430, every offset asserted); `python tools/gen_layout.py --check`
   fails while they are out of date.
2. **The class** `<Name>Pass : RenderPassBase` (`render_pass_base.hpp`): `initialize`
   (set `m_device`/`m_physicalDevice`, then create everything; return false on any
   failure), `cleanup`, `execute(cmd, frameIndex)`, `onResize`, `getName`. Use the base
   helpers (`createSampler`, `safeDestroy`; `createRenderTarget`, `transitionImage` for
   raster passes) or, for a Slang compute pass, `rtc::createImage`, `rtc::transition` and
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
   and `reset()` on failure, as SSS does); record it in `render()` at its place in the
   frame (after lighting: sky, vfx.expand, vfx.sort, SSS, transparent, post, gizmo) inside
   `timerBegin("name") … timerEnd()`; resize it in `onResize`, and hand it again any view
   of another pass that a resize recreates. A pass left at its creation size is invisible
   to validation and wrong on screen. A graph pass is timed under its `addPass` name.
   Either way the name goes in the **pass registry** (`ohao/render/frame/pass_registry.hpp`,
   `kPasses`, at its place in frame order, lowercase and dotted: `rt.gi.denoise`): the
   timer (`GpuPassTimer`, `render/frame/gpu_pass_timer.hpp`) times only registered names,
   logs an unknown one once as an error. The pass's time then shows in `render.stats` and
   `render_bench`; a budget for it goes in `tests/bench/render_budgets.json`, judged by hand.
6. **Check it:** build (the shader comes with any target that renders); run validation
   with a resize; run the goldens — a pass that changes pixels changes the deferred goldens
   on purpose, so look at the diff (`build/golden_diff/`), then `--update`. The deferred
   goldens are exact, so they also hold the frame bit-identical run to run.
   A post effect that agents switch on also needs its name in `PostEffect`
   (`ohao/api/params.{hpp,cpp}`) and `kEffects` (`session.cpp`), and its toggles in
   `runtime/frame_capture.cpp` and `runtime/live_host.cpp`.

```
python tests/validation/check_validation.py
python tests/golden/render_golden.py tests/golden/manifest.json
```

## Recipe: register a pass and give it a budget

Followed for every pass sub-project 2b added (`cull.*`, `hiz.build`, `lights.cluster`,
`capture.preview`, `pt.present`, …). A pass is not done when it renders: it is done when
it is measured under its name.

1. **The registry entry** in `kPasses` (`ohao/render/frame/pass_registry.hpp`): name, kind
   (`Transfer`, `Raster`, `Compute`, `RayTracing`) and one line saying what it does and
   what it costs at the defaults, at its place in frame order. The name is lowercase and
   dotted, with a family prefix when it has siblings (`rt.gi`, `rt.gi.denoise`), so a
   budget can cover a family with a pattern (`"cull.*"`).
2. **Time it**: `timerBegin("name") … timerEnd()` around its commands (`DeferredRenderer`),
   or its `addPass` name if it is a graph pass. Record passes in registry order and time each
   once; an unregistered name is logged once as an error and not timed.
3. **If a setting turns it off** (`render.settings`), the switch belongs in
   `VulkanRenderer::RenderSettings`, `setRenderSettings` and `renderSettings` (read back
   from the subsystem, never cached), its reflected twin in `ohao/api/params.{hpp,cpp}`
   (`RenderSettings`, with limits), and the two conversions in `runtime/frame_capture.cpp`.
   Turn it off by not registering it in the frame's graph, so it costs nothing and leaves
   the timings; check by hand that a capture with it off lists no time for it.
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
   --check` must pass five consecutive clean runs, by hand (a run flagged `noisy` is no verdict;
   `python tools/bench/wait_quiet.py` first).

```
$ python -c "import json, re, fnmatch; names = re.findall(r'\{\"([a-z.]+)\", PassKind', open('ohao/render/frame/pass_registry.hpp').read()); pats = [p for b in json.load(open('tests/bench/render_budgets.json'))['budgets'] for p in b.get('passes', [])]; print(len(names), 'passes registered; budget names matching none:', [p for p in pats if not fnmatch.filter(names, p)])"
43 passes registered; budget names matching none: []
$ build/Release/render_bench --check tests/bench/render_budgets.json > /dev/null
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
2. **Check the hints:** `ohao level <file>` (below) reports unknown or bad `extras`; the
   schema is `ohao schema level-extras -o FILE`.
3. **Look at it without a scene:** `ohao level <file>` prints `level.info`: node, mesh
   and triangle counts, colliders by shape, bodies by motion, spawns in node order with
   their facing, and the warnings (`W_LEVEL_EXTRAS` for an unknown lower-case key,
   `W_LEVEL_NO_PLAYER_SPAWN`, a dynamic mesh made convex). An unknown capitalised key or a
   bad value is `E_LEVEL_EXTRAS` (exit 2): the node loads without it.
4. **Place it:** a scene entity with `Level {asset: {path}, lightScale, collision}`; one
   line places the whole file, and saving writes only that line.
5. **Check that it collides:** `physics.raycast` down from above each spawn must hit the
   ground at the spawn's height (the blockout's spawns: within 1 mm); `physics.info` counts
   the bodies.

```
build/Release/ohao level assets/levels/blockout_room.glb
```

## Recipe: add an animated character

Followed for the play scene's enemies and the player's arms (sub-project 10, steps 2 and 3;
`docs/specs/2026-10-28-animation-design.md`).

1. **The file:** one glTF binary holding a skinned mesh, its skin and its clips, in meters,
   facing −Z. Committed art is CC0 or ours and is rebuilt by a script in `tools/anim/` that
   writes the same bytes twice (`make_enemy.py` from a staged CC0 pack, pure Python;
   `make_fps_arms.py` in Blender 5.2.2), with `LICENSE.md` and `README.upstream.md` beside it
   (`assets/characters/<name>/`). An additive clip is authored as rest × delta: re-reference a
   pack clip to its own first frame before using it additively.
2. **The graph** (`assets/anim/<name>.animgraph.json`): `skeleton {file, skin}`, a `driver`,
   `clips` (`file`, `animation`, `loop`, `rate` or `rateFrom {param, at}`, `events [{t,
   name}]`), `params` (`float` or `bool`), and `layers` (`replace` or `additive`, a joint
   `mask`, `states`: a clip or a `blend1d` over a parameter, and `transitions` on a
   parameter, an event or `clipEnd`, with a `fade` in seconds; the file is `"version": 2`). Every error names its
   JSON pointer; a one-shot state with no way out is one.
3. **The driver** (`<name>_anim.cpp`, the game's, registered by its install: the slice's in `games/slice/module.cpp`): writes the
   graph's parameters and events from **saved** state only, and reads the step's combat
   events after its last `restart` (`driveEnemyAnim`, `driveArmsAnim`). A parameter no
   driver writes reads 0.
4. **The entity:** `MeshSource {ModelAsset {asset, mesh}}`, `SkinnedMesh {file, skin}` and
   `Animator {graph}`; things it holds are children with `JointAttach {joint}` and their own
   `Transform` in the joint's frame (the rifle in `Index1.R`, the muzzle at the barrel's
   tip). A first-person rig goes under a `ViewModel` root; author what should show at least
   0.25 m from the eye and within 30° of the eye line.
5. **Check it:** `animation.info` (the states and clip times) and `events.read {names: ["anim.clip_event"]}`;
   force a state with
   `component.set` on `Animator`; step and read it back. `replay_test` holds a restore
   replaying the same Animators (`world.hash`) if the play scene uses it; `check_validation.py`
   renders the skinned rig.
6. **Budgets:** `physics_bench` times the `animation` step and the `pose` sync
   (`anim_step_ms`, `pose_sync_ms` for the play scene), `render_bench` the `skin`,
   `skin.bounds` and `rt.blas.refit` passes (`play_anim_1080p`).

```
build/Release/ohao run assets/scenes/anim_rig.scene.json --script docs/agents/examples/animation.jsonl
```

## Recipe: add a sound

Followed for the slice's ambience beds (sub-project 11, step 3;
`docs/specs/2026-10-28-audio-design.md`). What plays is decided in the step; the device only
renders it, so a sound is tested on the schedule, without a device.

1. **The file:** a WAV under `assets/audio/<group>/`, ours or CC0, made by a script in
   `tools/audio/` that writes the same bytes twice (`make_weapons.py`, `make_footsteps.py`,
   `make_ambience.py`). Variants are numbered files, named in the bank as `{n}`. Sounds staged
   for local listening go to git-ignored `assets/local/audio/`, which is read first and never
   committed.
2. **The bank** (`assets/audio/slice.audio.json`, the `AudioMix` asset): a `sounds` entry with
   `files`, `bus`, `priority` (higher is kept when a cap is reached), `shape` (its distance
   curve, from `shapes`), `holdMs` (how long it holds its voice), and optionally `volume`,
   `send` (to the reverb return) and `occlude`. Every error names its JSON pointer; `ohao
   schema audio-bank [-o FILE]` writes the schema. Files are decoded when the bank loads, never on a play.
3. **The event:** the schedule (`ecs/audio_schedule.cpp`) names the sound for a step's event:
   the bank's `events` for a game's channels (`{"on": "slice.hit", "play": "impact_{tag}"}`), `animSoundNames` for `anim.clip_event`,
   `footstepSound` for `physics.footstep` and `physics.land` by surface. A clip event of a new name
   needs a line there; a new surface needs only its `footstep_<surface>` sound.
4. **Check it on the schedule:** in a session, `events.read {names: ["audio.*"]}` lists the step's
   commands (the play, its voice, variant, occlusion and outcome); `audio.mix` solos its bus to listen in a window
   (`ohao serve --window --sound`).
5. **Budgets:** `audio_bench` (`schedule_step_ms`, `submit_frame_ms`, `bank_load_ms`);
   `audio_bench --listen DIR` renders the fight to a WAV with its schedule.

```
build/Release/ohao schema audio-bank -o build/audio_bank.schema.json
```

## Recipe: add an effect

Sub-project 12 (`docs/specs/2026-10-28-vfx-design.md`). An effect is data: an entry in the set, a
texture if it needs one, and a binding that says when it plays. Native code changes only for a new
kind of record.

1. **The texture:** a PNG under `assets/vfx/`, written by `tools/vfx/make_flipbooks.py` (fixed seeds,
   the same bytes twice) or CC0 with `LICENSE.md` and `README.upstream.md`. Describe it in
   `assets/vfx/README.md`.
2. **The effect:** an entry in `assets/vfx/slice.vfx.json`'s `effects`: `emitters` (`count`, `life`,
   `speed`, `size`, colour and alpha curves, `blend`, `lit`, `texture`, `frames`), and optionally a
   `light` (a flash that fades over its `time`) and a `proxy` (smoke spheres for RT direct light and GI).
   Every error names its JSON pointer; `ohao schema vfx-set` writes the schema.
3. **The binding:** the set's `events` name it by channel: `slice.shot` (the muzzle, `ahead` of the eye),
   `slice.hit` (per surface through `{tag}`, with a `fallback`), `slice.explode` (the boom, smoke and scorch). A level node takes an emitter with the `vfx`
   hint (`extras.vfx = "smoke_cloud"`); a scene places one with a `VfxEmitter`.
4. **Check it on the step:** `vfx.info` lists the records a fight writes in a session; capture a
   frame to see it.
5. **Budgets** (by hand): `render_bench --only play_vfx_1080p --check tests/bench/render_budgets.json`
   (`vfx.expand`, `vfx.sort`, `transparent`, `decals`).

```
build/Release/ohao schema vfx-set -o build/vfx_set.schema.json
```

## Recipe: add a golden scene

Followed for `cli_helmet_game_deferred` (sub-project 4, step 11).

1. **The scene** is a checked-in `.scene.json` (`assets/scenes/`); the harness loads every
   scene into one `ohao serve` and renders it with `render.capture`, so everything about the
   render is in its parameters: `mode`, `frames` (deferred: from a reset, the last kept; with
   TAA give enough frames to converge), `post`, `ev100`, `tonemap`, `quality` (deferred:
   `realtime` or `reference`), `spp`/`denoise` for the path tracer, and `width`/`height`
   640x360, the size it is compared at.
2. **The entry** in `tests/golden/manifest.json`: `name`, `scene`, `capture` (those
   parameters), `golden` (`tests/golden/<name>.png`), the tolerance and a
   `_note` saying what the image is and why its tolerance is what it is. **Deferred
   scenes are exact** (`max_abs_diff` 0, `max_diff_frac` 0): deferred rendering is
   deterministic. Path-traced ones get the noise floor `--selftest --json` measures, never
   more.
3. **Generate, then look:** `--only <name> --update` writes the golden; open the PNG and
   check it shows what the note says before keeping it. Then run `--only <name>` twice
   more: an exact scene must pass both.
4. Only if it serves requirement 3 (AGENTS.md, "Gates"): the goldens are a handful, and a new one
   replaces an old one unless a bug got through that none of them shows.

```
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_cornell_box_deferred
```

## Recipe: add a gate

A gate exists only for one of AGENTS.md's five requirements, or for a real bug that got through.
Before it lands it is proven: plant a bug in what it covers and watch it fail.

1. A Python harness under `tests/<area>/` (standard library, numpy or Pillow): it drives `ohao run` or
   `ohao serve`, prints `ok`/`FAIL` lines and ends with one JSON line `{"suite", "passed", "failed",
   "failures"}`, exiting nonzero on any failure. A C++ suite uses `tests/common/harness.hpp`.
2. Register it in `ohao_register_harness_tests` (`cmake/ohao_tests.cmake`): `ohao_add_python_test(<name>
   LABEL quick|gpu|golden|slow [GPU] TIMEOUT <s> SCRIPT <path>)`, a `TIMEOUT` always, `GPU` if it
   creates a device, `LOCKS` for a fixed scratch path.
3. A rule in `tools/check_changed.py` for the files it reaches, and its row in AGENTS.md's gates.

```
ctest --test-dir build -C Release -R replay_test
```
