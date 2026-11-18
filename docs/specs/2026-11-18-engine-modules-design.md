# Engine modules: the design (step 4b, part 4b-6b)

**Plan:** `docs/plans/2026-11-18-step4b-jobs.md`, part 4b-6b; system design `docs/specs/2026-11-08-system-design.md`
4.1-4.3. **Status:** designed 2026-11-18 at `dedbda85` (every number measured on this machine that day unless it says
*estimate*), with the rulings below; built in the commits of section 9.

## Rulings on this design (2026-11-18)

- **R1. The edit loop is in scope, not a gap** (requirement 3; AGENTS.md "Seconds"). Section 2's 42 s for a component
  edit and 30 s for a registration are bugs this part fixes, since it moves every file they involve; section 2's
  "after" estimate of 18 s is not accepted as the end state. At the build:
  - each module's registrations in its own `types.cpp`, and no registration unit over 5 s alone (split a module's
    by component where one is: the units then compile in parallel);
  - each bench includes and installs only the modules it uses (`engine::install(..., only)`);
  - the game PCH measured: what it costs on a component edit, and whether it can hold less than every public header;
  - at commit 11, section 2's edits measured again (3 runs each) and the remaining critical path named with its
    numbers. If it is still over 10 s, the next step (for example `makeComponentOps<T>`'s per-type templates made one
    implementation over the reflected layout) is measured and proposed, not left unnamed.
- **R2. Commit 1's change of every `world.hash` is accepted** as the one deliberate change: hashes, snapshot columns
  and saves walk types by name, so no later move of a component can change them.
- **R3. The build stops after commit 5 and reports** (one agent's context holds five of these commits); the rest is a
  second build.

- **R4. One feature, one module: the engine is additive** (Frank, 2026-11-18: "1 feature 1 mod. so that it makes
  sure the system is additive"). A module is a feature, not a subsystem's whole: a feature that spans subsystems is
  its own small module depending on them (footsteps, today inside the audio schedule's `footFromClips`, depends on
  audio, animation and physics; the player controller depends on physics' characters), never code added to one of
  them. Every module has one shape: its data (components, events and its files' schema), a pure core below where it
  has one (no World: the math, the backend), its systems with their declarations, its methods.
- **R5. Additive by proof, not by intention.**
  - *Static, every `check.py`:* `check_layers.py` proves each module deletable: nothing outside it and the modules
    that declare it includes it, links it or names its install. A module's removal is then a directory and a line.
  - *Dynamic, at a step's close:* the engine built and its gates run with each leaf module removed (the dependency
    closure), so "deletable" is a build, not a claim.
  - *Additive commits:* a feature's commit touches its module's directory and the module list only; `check.py
    --changed` says which modules a change touches, so a feature that edits a second module is seen.
- **R6. The engine describes itself to agents** (the project's requirement: games built with AI, through `ohao serve`).
  `engine.modules` over the API and MCP: each module, its dependencies, its components, its event channels with their
  writers and typed readers, its systems with their access sets, phase and wave, its methods; and live, each module's
  cost (CPU a step and a sync from the waves' timings, its memory) beside `render.stats`. An agent asks the running
  engine who writes `Health` or what a module costs, instead of reading the tree. Built after 4b-6b's last commit, from
  what the World already records (the declarations, the channels, the reflection), with no second source.
- **R7. Measured first, then decided: engine modules reloaded live in development.** The game `.so` already reloads
  with its state across builds (`game.reload`, layout keys); every module's state is in components or snapshot hooks,
  so a module could reload the same way (snapshot its state, unload, load the new build, restore) and an edit to
  physics would not restart a session. Before any build: what a restart of `ohao serve` costs on the play scene
  against a game reload, and what reloading a module's state would need (Jolt's bodies rebuilt from components).
  Built only if the measurement shows it is the edit loop's largest cost.

- **Corrections from build 1** (commits 1-5, 2026-11-18):
  - *`only`* installs the core and exactly the named modules, not their DEPENDS closure: while animation, audio and vfx
    are still inside `ohao_ecs`, the closure installed them into benches that measure physics alone. At commit 10
    (every module its own) `only` closes over DEPENDS again, and an install whose dependencies are missing is refused,
    the one rule for engine modules and games (P5).
  - *Staged modules:* until a module moves, `engine::modules()` lists a stand-in for it, so `only` and `game.reload`
    have one path from commit 3.
  - *Exceptions until commit 8:* `ohao_ecs->ohao_visual` and `ohao_ecs->ohao_physics` (animation, audio and vfx still
    in `ohao_ecs` read them).
  - *The machine's floating-point flags come from Jolt's interface* (`-mavx2 -mfma -mfpmath=sse`): linking Jolt
    private moved the crowd's hash and replay checkpoints 2-6 (A/B), so Jolt is linked public. Build 2 makes the flags
    one explicit engine-wide setting every engine and game target links, Jolt private, the hashes unchanged.
  - *`spawnLight` and `lookRotation`* had no callers: deleted, not moved.
  - *R4 on the player:* it stays in physics. Its step runs inside the physics step, between the bodies' sync and Jolt's
    substeps (`physics_scene_players.cpp:462-498`); its footsteps and landings are sorted with contacts, triggers and
    joint breaks into one emission (`physics_scene_step.cpp:144-149`); one snapshot holds it with the bodies. As its own
    module it would collide with stale bodies and move `world.hash`. It is physics' feature (characters), not a
    feature on top of physics.

## 0. What must be true (the requirement)

Frank, 2026-11-18: subsystems must be modules, or the engine is "difficult to maintain and hard to impl across all
modules". Read as testable properties of the end state:

1. **One place per subsystem.** A subsystem's components, registrations, events, systems, API methods and install are
   in one directory, its module. Today each is spread over five to eight files in four to six libraries (section 1).
2. **Declared edges only.** A module reads another module only through that module's public headers, and only if it
   declares the dependency; everything else between modules is components and events in the World. Today nothing
   refuses a sideways include, and the World's core names physics, rendering, levels, animation, audio and vfx.
3. **The edit loop is seconds** (AGENTS.md, "Seconds"). An edit to one module's component rebuilds that module and
   what includes it, not the engine. Today a component edit is 41-43 s (section 2).
4. **Nothing behaves differently.** A refactor: `replay_test`'s hashes, the goldens and the benches' hashes and times
   hold. The single deliberate change is the one in commit 1 (section 9), named, with its reason.

## 1. Measured: the spread and the edges

### 1.1 Where each subsystem lives today

| Subsystem | Files it is spread over |
|---|---|
| core (World) | `ecs/world.*`, `events.*`, `system.hpp`, `snapshot*.{hpp,cpp}`, `world_hash.*`, `world_diff.*`, `scene_io.*`, `scene_types.hpp`, `json_parse.*`, `owned_value.hpp`, `asset_resolver.*`, `spawn.*`, `transform_system.*`, `spin_system.*`, `fly_controller_system.*`; its components mixed into `components.hpp`, its registrations into `scene_types.cpp` and `event_types.cpp` (RestoreEvent) |
| physics | `ohao/physics/` (the backend, its own library), `ecs/physics_scene{.hpp,_impl.hpp,.cpp,_joints,_players,_step}.cpp`, `ecs/player.*`; components `components.hpp:238-395`; registrations `scene_types.cpp:38-61, 290-342, 360-408, 519-543`; events `event_types.cpp:12-41`; 6 methods in `api/level.cpp` with params rows in `api/params.{hpp,cpp}`; install in `engine/install.cpp:8`; `game/install.cpp:44, 75-76` (the control rule); `world.cpp:82-83` and `transform_system.cpp:48` (the transform system tags bodies) |
| animation | `ohao/anim/` (8 files, its own library), `ecs/animation.*`; components `components.hpp:397-430`; registrations `scene_types.cpp:438-463`; events `event_types.cpp:49-55`; `api/animation.cpp` + params rows; `engine/install.cpp:7`; `game/install.cpp:42, 72-73` (drivers); `scene_io.cpp:282,297-299` (asset fields) |
| audio | `ohao/audio/` (two libraries: the bank, the mixer), `ecs/audio_schedule.*`; components `components.hpp:432-448`; registrations `scene_types.cpp:409-423`; events `event_types.cpp:57-105`; `api/audio.cpp` + params rows; `engine/install.cpp:8`; `scene_io.cpp:283,301` |
| vfx | `ohao/vfx/` (its own library), `ecs/vfx.*`; components `components.hpp:450-465`; registrations `scene_types.cpp:424-437`; `api/vfx.cpp` + params rows; `engine/install.cpp:9`; `scene_io.cpp:283,303`; `world_render/vfx_draw.*` |
| level | `ohao/level/` (two libraries: `ohao_level`, `ohao_level_world`); components `components.hpp:210-236`; registrations `scene_types.cpp:343-359, 512-518`; 2 methods in `api/level.cpp`; `engine/install.cpp:10`; `scene_io.cpp:281,293,826` |
| rendering's world side | `world_render/` (4 pairs); components `components.hpp:50-76, 96-199, 351-372`; registrations `scene_types.cpp:21-36, 78-250, 464-488, 506-511`; `ecs/primitive_mesh.*`, `ecs/scene_view.*`, `ecs/spawn.*` (spawnLight); `engine/install.cpp:17-21`; `game/install.cpp:43, 74` (presenters); `scene_io.cpp:281,287-290,314-328` |

`components.hpp`: 502 lines, 69 declarations (structs, enums, the alias), 36 direct includers, 56 objects depending on
it (ninja's deps, with the benches and tools), plus the two games' precompiled headers and their 10 objects.
`scene_types.cpp`: 547 lines, one function, 66 registrations (`r.type`/`r.enumeration`); `event_types.cpp`: 14.

### 1.2 Every edge between subsystems, with its verdict

`L` legitimate (declare it, through a public header), `E` should be an event or component (it is, or becomes one),
`W` wrong (generalize, move or delete). 38 edges: 20 L, 3 E, 15 W.

**From the core to the modules (all wrong: the core may know nothing above it)**

| # | Where | What it uses | Verdict and fix |
|---|---|---|---|
| 1 | `ecs/world.hpp:3` | includes `components.hpp`: every module's components reach every World user | W: `ecs/components.hpp` keeps the core's only; each module has its own |
| 2 | `ecs/world.cpp:2-4, 76-84` | installs `spin`, `fly_controller`, `transform` in the World's constructor | W: the core module's install does (section 6.1) |
| 3 | `ecs/world.cpp:82-83`, `transform_system.cpp:48` | the transform system reads `PhysicsBody`, `ColliderPart`, writes `PhysicsMoved` | W: a move tag physics registers (`ecs::tagMoves<PhysicsMoved, PhysicsBody, ColliderPart>`), section 6.1 |
| 4 | `ecs/scene_io.cpp:279-306` | `AssetTypes` names MeshSource, Environment, Level, CollisionMesh, SkinnedMesh, Animator, AudioMix, VfxSet | W: a field annotation in each module's registration, `.asset("model")`; checkAssets walks the reflected fields (struct, optional, variant alternative) and builds the same JSON paths |
| 5 | `ecs/scene_io.cpp:314-328` | W_UNSAVEABLE_MESH: MeshRenderer without MeshSource | W, **deleted**: only `instantiate.cpp:225` emplaces a MeshRenderer, always from a MeshSource; `attachModel`/`spawnModel`, which made the case, are gone (grep: no hits). The doc row `docs/agents/ohao-cli.md:495` goes |
| 6 | `ecs/scene_io.cpp:826` | save skips `LevelNode` entities | W: a type annotation `.entityNotSaved()` ("an entity holding it is its file's, never saved") |
| 7 | `ecs/snapshot.cpp:77, 94` | `BodyMotion`: the runtime column snapshots carry | W: a type annotation `.snapshotted()` ("runtime, held by snapshots, restored, never diffed"); the motion column becomes the list of such columns |
| 8 | `ecs/spawn.cpp:7-9` | `spawnLight` emplaces a Light | W: moves to `visual/spawn.hpp`; `lookRotation` stays |
| 9 | `ecs/player.cpp:16` | `playerOf` reads `PlayerController` | W: moves to the physics module (`physics/player.hpp`) |
| 10 | `ecs/event_types.cpp:3-5` | includes animation, audio and physics headers to register their payloads | W: each module registers its own |
| 11 | `ecs/scene_types.cpp:2` | registers every module's components | W: each module's `types.cpp` |
| 12 | `ecs/snapshot_column.hpp:6`, `asset_resolver.hpp:9`, `primitive_mesh.hpp:5` | include all components for EntityId / AssetRef / the prims | W: include what they use (`ecs/components.hpp`, `reflect/asset_ref.hpp`, `visual/components.hpp`) |

**Between modules**

| # | Where | What it uses | Verdict |
|---|---|---|---|
| 13 | `physics_scene.cpp:70, 230-238` | MeshSource: a collider fitted to the entity's mesh | L physics -> visual |
| 14 | `physics_scene_impl.hpp:9` | `primitive_mesh.hpp`: the prims' meshes to fit to | L physics -> visual |
| 15 | `physics_scene_players.cpp:27-36` | Camera: the player's head | L physics -> visual |
| 16 | `physics_scene_players.cpp:55-110` | SpawnPoint (today level's) | E: SpawnPoint moves to the core (level writes it, physics and the slice read it; level -> physics already, so it cannot be level's) |
| 17 | `physics_scene_players.cpp:537,548,602`, `animation.cpp:312,343`, `slice/view_model.cpp:223,243` | write RenderPose, RenderDirty | E: both are the core's "where an entity is drawn" contract, which every module writes and the extract reads (Unreal's MarkRenderStateDirty is in Engine, not Renderer) |
| 18 | `physics_scene_impl.hpp:11` | `propagateTransforms` | L physics -> core |
| 19 | `physics_scene.hpp:42` | the backend header in the public header (`backend()`, `bodyOf` return backend types) | W: `backend()` leaves the public header (no caller outside physics); `bodyOf` becomes `hasBody(e)` (its one outside caller, `api/level.cpp:501`, asks only that) |
| 20 | `animation.cpp` (MeshSource), `animation.hpp:16` | the skinned mesh's source; the anim cache | L animation -> visual; anim/ becomes the module's own |
| 21 | `audio_schedule.cpp:322-324` | FootEvent channels, ClipEvent channel | L (events) audio -> physics, audio -> animation |
| 22 | `audio_schedule.cpp:144-157` | `physicsScene(world)->raycastFrom`: occlusion | L audio -> physics |
| 23 | `audio_schedule.cpp:49, 127, 752` | PlayerController (the character, its eye height), `playerInfo` (velocity, ground surface) | L audio -> physics |
| 24 | `audio_schedule.cpp:404` | CollisionMesh: a zone fitted to the collision model | L audio -> physics |
| 25 | `audio_schedule.cpp:405, 123, 134` | MeshSource, Camera: zone fit, the listener | L audio -> visual |
| 26 | `audio_schedule.cpp:3, 449-462` | `AnimationSystem::graph` and `anim/anim_graph.hpp`, to see whether a graph's clips fire foot events | L but narrowed: `animation/animation.hpp` gains `bool graphFires(World&, path, names)`; audio stops including the graph |
| 27 | `vfx.cpp:5, 344, 424` | `playerOf`, PlayerController (first-person effects) | L vfx -> physics |
| 28 | `vfx.cpp:62-63` | MeshSource's BoxPrim (ray hits on boxes) | L vfx -> visual |
| 29 | `level_hints.hpp:18` and `level_system.cpp` | writes MeshSource, Light, Camera; Collider, RigidBody, ColliderPart, CollisionMesh; ReverbZone; VfxEmitter | L level -> visual, physics, audio, vfx (a level turns a glTF file into every module's components) |
| 30 | `level_system.cpp:257, 414-417` | writes InstantiateError (rendering's) | E: InstantiateError moves to visual ("why an entity's content could not be built"), which both level and instantiate write; else level would link the renderer |
| 31 | `render_extract.cpp:2, 127` | AnimPose (the skinning palette), SkinnedMesh | L world_render -> animation |
| 32 | `vfx_draw.cpp:6` | `VfxSystem`'s records | L world_render -> vfx |
| 33 | `instantiate.cpp:3` | `primitive_mesh.hpp` | L world_render -> visual |
| 34 | `engine/install.cpp:3-10` | names each module's install | L (the composition root), but becomes the module list |
| 35 | `game/install.cpp:5-6, 11, 42-44, 72-76` | animation's drivers, physics' control rule, world_render's presenters (what uninstallGame takes back) | W: each module reports and removes what others added to it (`contributions`, `removeContributions`), section 4 |
| 36 | `api/animation.cpp`, `api/level.cpp` (8 methods) | each method reads one module through the session | W: the methods move into their modules (section 5) |
| 37 | `api/vfx.cpp:88`, `api/audio.cpp:68, 190` | the host's frame stats and audio device, and the session's mix overrides | L: these three methods are the session's (they report the host), on the modules' public queries |
| 38 | `api/audio.cpp:11`, `tools/ohao/main.cpp` | the audio bank | L (above every module) |

The worst: edge 1 (every World user compiles every module's components) together with edge 11 (one 547-line function
holds every registration): together they are the 42 s edit loop. Edges 2-7 are the World's core naming six modules.

## 2. Measured: the edit loop

`touch`, then `cmake --build build --config Release -j16`, 3 runs each, nothing else running; the tree restored by a
normal build after (a no-op build compiles and links nothing).

| Edit | Runs (s) | Objects compiled | Programs linked | The slowest step |
|---|---|---|---|---|
| `ecs/components.hpp` (any component field) | 43.07, 40.64, 42.23 | 68 (56 objects, 2 game PCHs, the games' 10) | 22 | `scene_types.cpp` 39.6 s under the parallel load |
| `ecs/animation.cpp` (a system) | 6.34, 6.31, 6.32 | 1 (5.6 s) | 10 | the compile |
| `ecs/scene_types.cpp` (a registration) | 30.46, 30.40, 30.36 | 1 (29.7 s) | 10 | the compile |

Why `scene_types.cpp` costs 30 s: each `makeComponentOps<T>` instantiates the capture, restore, hash and diff column
templates at -O3. A by-hand probe (scratchpad copies, the same flags): an empty registration TU compiles in 1.03 s,
one holding only animation's three components (`scene_types.cpp:438-463`) in 3.74 s (twice each): about 0.9 s per
component with data, so the 30 s is the 30-odd such components of one TU, serial.

Other compile times seen in the components.hpp rebuild (under load): `physics_scene.cpp` 21.0 s, `api/params.cpp` 19.1 s,
`physics_bench.cpp` 17.2 s, `level_system.cpp` 12.3 s, `world.cpp` 11.7 s, the slice's PCH 8.9 s then `combat.cpp` 8.8 s.

**After** (*estimate*, measured at the build): an animation component edit compiles animation's `types.cpp` (about
4 s, the probe), its systems, `render_extract.cpp`, `audio_schedule.cpp`, the benches that include it and the games
(the game API is every public header, so its PCH and objects rebuild). The critical path becomes the game PCH chain
(about 9 + 9 s) or `physics_bench.cpp` (17 s), not 30 s of registrations: about 18 s from 42 s. Getting a component
edit to seconds needs two further steps this part does not take and the record names as gaps: the benches include
everything, and the game PCH holds every module's headers.

## 3. The modules and their public surface (decision 1)

**Decided:** eight modules. The plan's seven, with rendering split into `visual` (headless) and `world_render`.

| Module | Directory | Library (level) | Depends on | Public headers |
|---|---|---|---|---|
| `ecs` (the core) | `ohao/ecs/` | `ohao_ecs` (4) | - | `world.hpp`, `system.hpp`, `events.hpp`, `components.hpp` (the core's), `module.hpp` (new), `snapshot*.hpp`, `world_snapshots.hpp`, `world_hash.hpp`, `world_diff.hpp`, `scene_io.hpp`, `scene_types.hpp` (ComponentOps), `json_parse.hpp`, `owned_value.hpp`, `asset_resolver.hpp`, `spawn.hpp` (lookRotation), `transform_system.hpp`, `spin_system.hpp`, `fly_controller_system.hpp` |
| `visual` | `ohao/visual/` (new) | `ohao_visual` (5) | ecs | `components.hpp`, `primitive_mesh.hpp`, `scene_view.hpp`, `spawn.hpp` (spawnLight), `module.hpp` |
| `animation` | `ohao/animation/` (from `ohao/anim/` + `ecs/animation.*`) | `ohao_animation` (6) | ecs, visual | `animation.hpp` (AnimationSystem, AnimParams, AnimPose, drivers, `graphFires`), `components.hpp`, `events.hpp` (ClipEvent), `anim_graph.hpp`, `anim_cache.hpp`, `skeleton.hpp`, `gltf_anim.hpp`, `module.hpp` |
| `physics` | `ohao/physics/` (backend + `ecs/physics_scene*` + `ecs/player.*`) | `ohao_physics` (6) | ecs, visual | `physics_scene.hpp` (queries, players, the control rule), `components.hpp`, `events.hpp`, `player.hpp` (playerOf), `module.hpp`; private: `private/physics_scene_impl.hpp`, `private/backend/**` |
| `audio` | `ohao/audio/` | `ohao_audio` (7); below it `ohao_audio_bank` (1) and `ohao_audio_mixer` (2), in the same directory | ecs, visual, physics, animation | `audio_schedule.hpp`, `components.hpp`, `events.hpp`, `module.hpp`; the bank's `audio_bank.hpp`; the mixer's `audio_system.hpp`, `reverb_node.hpp` |
| `vfx` | `ohao/vfx/` | `ohao_vfx` (7); below it `ohao_vfx_effects` (1) in `ohao/vfx/effects/` | ecs, visual, physics | `vfx.hpp` (VfxSystem, records), `components.hpp`, `module.hpp`; the effects library's `effects/vfx_set.hpp`, `particle_math.hpp`, `smoke_math.hpp`, `tracer_math.hpp` |
| `level` | `ohao/level/` | `ohao_level` (8) | ecs, visual, physics, audio, vfx | `level_system.hpp`, `level_hints.hpp`, `components.hpp`, `module.hpp` |
| `world_render` | `ohao/world_render/` | `ohao_world_render` (8) | ecs, visual, animation, vfx | `present.hpp` (HUDs, spheres), `components.hpp` (MeshRenderer), `module.hpp`; private: `private/components.hpp` (RenderSlot, InstantiateDirty), the systems' headers |

- **Why `visual` is its own module (the plan's grouping refuted):** physics fits colliders to MeshSource, audio fits
  zones to it and hears from the Camera, level writes MeshSource, Light and Camera, animation skins a MeshSource. Were
  these components the rendering module's, all four would depend on a library that links `ohao_renderer` (Vulkan, VMA,
  NRD, OIDN, TBB), which spec 4.1 forbids ("the world does not link the renderer"). `visual` is data and CPU code
  only: what an entity looks like and where it is seen from. Taken from Unreal: components in Engine, the
  implementation in Renderer. Not taken: Bevy's bevy_render owning Camera and Mesh, which works there only because
  the GPU crate links headless.
- **Why spin and fly stay in the core module:** they depend on nothing but Transform and the input, and nothing
  depends on them. A module of their own would add a boundary with nothing on the other side. They leave the World's
  constructor, not the core.
- **RenderPose, RenderDirty, MovedLastFrame are the core's** (edge 17): three modules and the game write them, the
  extract reads them. SpawnPoint is the core's (edge 16). InstantiateError is visual's (edge 30).
- **Namespaces stay** (`ecs::` for components and systems, `anim::`, `physics::backend::`, `level::`): a component
  is the World's vocabulary, the directory says whose it is. Renaming would rewrite every game for no requirement.
- **The engine's public install functions go.** `installPhysics`, `installAnimation`, `installAudio`, `installVfx`,
  `installLevels`, `installInstantiate`, `installRenderExtract`, `installVfxDraw` become private to their modules;
  everything installs through the module list (section 4). The accessors (`physicsScene(world)`,
  `animationSystem(world)`, ...) stay public.

### 3.1 Where each line of `components.hpp` goes

| Lines | Declarations | Destination |
|---|---|---|
| 1-18 | includes, namespace, `using Key` | each new header its own; `using Key` stays in `ecs/components.hpp` |
| 20-48 | EntityId, Name, Transform, WorldTransform, Parent, Children, the change-tracking note | `ecs/components.hpp` |
| 50-56 | MeshRenderer | `world_render/components.hpp` |
| 58-60 | RenderSlot | `world_render/private/components.hpp` |
| 62-76 | LightType, Light | `visual/components.hpp` |
| 78-94 | CreationOrder, PreservedComponent(s), TransformDirty, RenderDirty, MovedLastFrame | `ecs/components.hpp` |
| 96-98 | the note, `using AssetRef` | `ecs/components.hpp` (the alias; everyone uses it) |
| 100-173 | ModelAsset, the eight prims, MeshSource, MaterialAlpha, Material | `visual/components.hpp` |
| 175-177 | InstantiateDirty | `world_render/private/components.hpp` |
| 178-181 | InstantiateError | `visual/components.hpp` |
| 183-199 | Camera, Environment | `visual/components.hpp` |
| 201-208 | Spin | `ecs/components.hpp` |
| 210-227, 232-236 | Level, LevelNode, Socket, LevelDirty | `level/components.hpp` |
| 228-230 | SpawnPoint | `ecs/components.hpp` |
| 238-349 | ColliderShape, Motion, Collider, RigidBody, ColliderPart, CollisionMesh, PhysicsBody, PhysicsMoved, JointType, Joint, JointState, PlayerController, MoveIntent | `physics/components.hpp` |
| 351-359 | RenderPose | `ecs/components.hpp` |
| 361-372 | FirstPerson, DrawnFov | `visual/components.hpp` |
| 374-395 | PlayerState, BodyMotion | `physics/components.hpp` |
| 397-430 | SkinnedMesh, AnimLayerState, Animator, JointAttach | `animation/components.hpp` |
| 432-448 | AudioMix, ReverbZone | `audio/components.hpp` |
| 450-465 | VfxSet, VfxEmitter | `vfx/components.hpp` |
| 467-500 | ActionBinding, AxisBinding, InputActions, FlyController | `ecs/components.hpp` |

`ecs/components.hpp` keeps about 150 lines: what the World itself knows (ids, names, the hierarchy, the transforms
and the drawn pose, preserved components, the input bindings, spawn points, spin and fly).

Also moved: `AnimParams`, `AnimPose`, `ClipEvent` (`ecs/animation.hpp:36-64`) to `animation/animation.hpp` and
`animation/events.hpp`; `physics_events::*` and ContactEvent, TriggerEvent, FootEvent, JointBreakEvent
(`ecs/physics_scene.hpp:100-140`) to `physics/events.hpp`; `audio_events::*` and the seven payloads plus
AudioOutcome (`ecs/audio_schedule.hpp:85-146`) to `audio/events.hpp`; RestoreEvent stays in `ecs/world.hpp`.

### 3.2 Where each registration goes

Each module's `types.cpp` holds one function, its `Module::registerTypes`.

| `scene_types.cpp` lines | Registrations | Destination |
|---|---|---|
| 6-15, 29 | `registerKeyEnum` and its call | `ecs/types.cpp` (`registerKeyEnum` stays public: `api/params.cpp:120` uses it) |
| 21-27, 31-36 | LightType, MaterialAlpha | `visual/types.cpp` |
| 38-61 | ColliderShape, Motion, JointType | `physics/types.cpp` |
| 62-77 | Name, Transform, Parent | `ecs/types.cpp` |
| 78-250 | Light, ModelAsset, the prims, MeshSource, Material, Camera, Environment | `visual/types.cpp` (ModelAsset's, Environment's AssetRef fields gain `.asset("model")`, `.asset("environment image")`) |
| 251-289 | Spin, ActionBinding, AxisBinding, InputActions, FlyController | `ecs/types.cpp` |
| 290-342 | PlayerController, MoveIntent | `physics/types.cpp` |
| 343-351, 356-359 | Level, Socket | `level/types.cpp` (Level's asset `.asset("level")`) |
| 352-355 | SpawnPoint | `ecs/types.cpp` |
| 360-408 | Collider, RigidBody, Joint, JointState | `physics/types.cpp` |
| 409-423 | AudioMix, ReverbZone | `audio/types.cpp` (`.asset("audio bank")`) |
| 424-437 | VfxSet, VfxEmitter | `vfx/types.cpp` (`.asset("effect set")`) |
| 438-463 | SkinnedMesh, AnimLayerState, Animator, JointAttach | `animation/types.cpp` (`.asset("skin")`, `.asset("animation graph")`) |
| 464-472, 481-488 | MeshHandle, MaterialHandle, MeshRenderer, RenderSlot | `world_render/types.cpp` |
| 474-480, 489-505 | Children, WorldTransform, CreationOrder, PreservedComponent(s), TransformDirty, RenderDirty, MovedLastFrame | `ecs/types.cpp` |
| 506-507 | InstantiateDirty | `world_render/types.cpp` |
| 508-511 | InstantiateError | `visual/types.cpp` |
| 512-518 | LevelNode (`.entityNotSaved()`), LevelDirty | `level/types.cpp` |
| 519-543 | ColliderPart, CollisionMesh (`.asset("collision model")` on its ModelAsset), PhysicsBody, PhysicsMoved, PlayerState, BodyMotion (`.snapshotted()`) | `physics/types.cpp` |
| 544 | `registerEventTypes(r)` | each module's events: `event_types.cpp:12-41` physics, 43-47 ecs, 49-55 animation, 57-105 audio |

`registerSceneTypes` and `registerEventTypes` are deleted; `engine::registerTypes(r)` registers every engine module's
types in module order, each under `r.setModule("<module>")`. The seven callers (`tools/ohao/main.cpp:115`,
`doctor.cpp:276,300`, `game/install.cpp:149`, four benches, `examples/ohao_game.cpp:83`) call it.

## 4. The Module interface (decision 2)

**Decided:** one interface for engine modules and games, `ecs::Module` in `ecs/module.hpp`; `game::Module`,
`game::Method`, `game::Call` and `game::decodeParams` become aliases in `game/module.hpp`, so the games' code keeps
its names.

```cpp
// ecs/module.hpp (ohao_ecs: it needs the World and reflect only)
namespace ohao::ecs {
struct Call { World& world; bool paramsFailed{false}; };
inline bool decodeParams(Call&, const reflect::TypeInfo&, const reflect::JsonNode&, reflect::DecodeContext&, void*);
using MethodFn = std::function<bool(Call&, const reflect::JsonNode&, reflect::DecodeContext&, reflect::JsonWriter&)>;
struct Method { reflect::MethodDoc doc; MethodFn fn; bool mutates{false}; };

// What a host gives the modules: the assets and the types always; anything else by type (the renderer's scene and
// textures, once a host has them). The core names no renderer type.
class Services {
public:
    AssetResolver& assets;
    const reflect::TypeRegistry& types;
    template <class T> void put(T& service);
    template <class T> [[nodiscard]] T* get() const;   // null when the host has none
};

class Module {
public:
    virtual ~Module() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Engine modules: generated from CMake's DEPENDS (section 7), one source. A game: the engine modules it uses.
    [[nodiscard]] virtual std::span<const std::string_view> dependencies() const { return {}; }
    virtual void registerTypes(reflect::TypeRegistry& types) const { (void)types; }
    // The services it needs beyond assets and types; installModules installs it at the first call that has them all.
    [[nodiscard]] virtual std::span<const TypeKey> needs() const { return {}; }
    virtual void install(World& world, const Services& services) const = 0;   // once per World
    virtual void uninstall(World& world) const { (void)world; }                // a game's; engine modules never
    [[nodiscard]] virtual std::span<const Method> methods() const { return {}; }
    // What other modules added to this one, by name (animation: drivers; physics: the control rule; world_render:
    // presenters), and their removal: what uninstallGame takes back.
    [[nodiscard]] virtual std::vector<std::string> contributions(const World& world) const { return {}; }
    virtual void removeContributions(World& world, std::span<const std::string> names) const {}
};

// Each module after its dependencies, ties to the lesser name; an unknown dependency or a cycle throws, naming them.
[[nodiscard]] std::vector<const Module*> moduleOrder(std::span<const Module* const> modules);
void registerModuleTypes(reflect::TypeRegistry& types, std::span<const Module* const> modules);
// Installs, in order, each module not yet installed whose needs `services` meets; records them in the World.
void installModules(World& world, std::span<const Module* const> modules, const Services& services);
[[nodiscard]] std::span<const Module* const> installedModules(const World& world);
[[nodiscard]] const Module& coreModule();
}
```

- `engine/install.{hpp,cpp}` becomes the list: `engine::modules()` (the eight, from each `<m>/module.hpp`'s one
  declaration), `engine::registerTypes(r)`, and `engine::install(World&, const Services&, std::span<const
  std::string_view> only = {})`: `only` names the modules a bench wants, closed over their dependencies. A window
  host calls it once with the renderer's scene and textures in `services`; a headless host calls it at start and
  again at its first capture, when world_render's `needs` (`RenderScene`, `BindlessTextureManager`) are met.
  `installRendering` is deleted.
- **Methods, decided:** a module owns the methods that need only its World: the session takes the rows of the
  World's installed modules after its own and before the game's, with `MethodDoc::module` set (the catalogue says
  whose a method is, as it does for games). No upward edge: `Method` and `Call` name only `World` and `reflect`.
  Moved: `physics.raycast`, `physics.info`, `physics.contacts`, `player.info`, `player.teleport` (physics),
  `level.info`, `level.reload` (level), `animation.info` (animation), each with its params struct, out of
  `api/params.{hpp,cpp}` into the module's `methods.cpp` and its own params registry (the slice's pattern,
  `games/slice/methods.cpp:40`). Their E_STATE branches go: a module's methods exist only when it is installed.
- **Stays in `ohao_api`:** the session's methods (scene, world, time, input, render, log, schema, engine.info,
  game.reload, `asset.info`, `asset.reload`: the session's resolver) and three that report the host: `vfx.info` (the
  GPU frame, `api/vfx.cpp:88`), `audio.info` (the device, `api/audio.cpp:68`), `audio.mix` (the session's overrides,
  `api/audio.cpp:190`). They call the modules' public queries. `api/level.cpp` keeps only the asset methods and is
  renamed `api/assets.cpp`.
- **The session's rows** are taken at its start and at `game.reload` (as the game's are now). A module with `needs`
  has no methods (world_render has none), so a module installed after the session starts never misses its rows.
- **`game/install.cpp`** no longer names animation, physics or present: `Inventory` gains, per installed module, its
  `contributions(world)`; `remove` calls `removeContributions` with what the game added. `installGame` refuses a game
  whose `dependencies()` are not installed, naming them (today the slice checks `animationSystem(world)` for null).

**State of the art.** Taken: Bevy's `Plugin` (one trait for the engine's crates and the user's, `build(&mut App)`
adds systems and types) and Unreal's `IModuleInterface` (one interface for engine and game modules) and its declared
`PublicDependencyModuleNames`. Not taken: Bevy's dynamic plugin groups and runtime discovery, Unreal's per-module
DLLs and engine hot reload (the game is already a `.so`; the engine's modules link statically), Unreal's
`PrivateDependencyModuleNames` (one kind of dependency is enough: a module's public headers name only what its
dependencies define, and CMake links PUBLIC).

**Refuted, then kept:**
- *"Engine modules should be a subset (no uninstall, no methods)."* A game needs every member, and an engine
  module's uninstall is simply never called; two interfaces would put two install paths in every host.
- *"A typed Services bag is a service locator, an anti-pattern."* The alternative names the renderer's types in
  the core (a forward declaration is still knowledge), or gives world_render a second install entry. The bag carries
  two required members and one optional kind; `needs()` makes the wait explicit and checked.
- *"vfx.info and audio.info should move too."* They report the host's device and GPU frame; moving them would push
  the session's `Host` interface below the modules for one fact each. The session is that seam already.

## 5. Directories, libraries, levels and the enforcement (decision 3)

### 5.1 A module's directory

```
ohao/<m>/CMakeLists.txt     ohao_add_module(<m> DEPENDS <modules> LINKS <libraries below>)
ohao/<m>/*.hpp              public: components.hpp, events.hpp, its API, module.hpp (one declaration: <m>Module())
ohao/<m>/*.cpp              its systems, module.cpp (the Module), types.cpp (registrations), methods.cpp
ohao/<m>/private/           private headers (and their code, where it is a unit: physics' backend)
```

- **Public headers stay at the module's root, private ones go under `private/`** (the plan said `public/`): includes
  stay today's `"<module>/<header>.hpp"`, the one marked directory is the exception, and the check is a path test.
- **The domain libraries (decided):** a library stays below the modules where code below the modules needs it, in
  the module's directory, with its own target and level; else it merges into the module.
  - `ohao/anim` (only `ohao_ecs` links it) merges into `ohao/animation`.
  - `ohao/physics/backend` (only `ohao_ecs` links it) merges into the physics module as `private/backend/`; its
    include root `ohao/physics` and the stray Vulkan include directory are deleted (`physics/CMakeLists.txt:6-10`), and
    `jolt_jobs.cpp`'s `-fno-rtti` follows its new path.
  - `ohao_vfx` (the set and the closed forms) stays below as `ohao_vfx_effects` in `ohao/vfx/effects/`: the renderer
    draws with it (`render/scene/vfx_frame.hpp`, `render/vfx/vfx_renderer.cpp`).
  - `ohao_audio_bank` and the mixer (renamed `ohao_audio_mixer`) stay below in `ohao/audio/`: the window's
    `runtime/audio_player.hpp` and `audio_bench` use the mixer, the mixer reads the bank.
  - `level/gltf_structure.*` moves to `ohao/cook/` (the cooker reads it, `cook/model_loader.cpp`; it is a source
    format reader, which `asset/` must not hold); `ohao_level_world` becomes the module `ohao_level`.
- Taken from Unreal: a plugin (our module) may hold several libraries (its modules). Not taken: per-module include
  roots (`Public/` on the include path), which would let the compiler enforce visibility but means dropping the one
  `ohao/` include root every library exports, a rewrite of every include in the engine; the scan below gives the same
  refusals with file and line.

### 5.2 The levels (`cmake/ohao_layers.cmake`)

```
0  ohao_core
1  ohao_gpu_vulkan ohao_reflect ohao_audio_bank ohao_vfx_effects
2  ohao_asset ohao_audio_mixer
3  ohao_cook
4  ohao_renderer ohao_ecs
   modules: 4 + the longest path from ecs (visual 5; animation, physics 6; audio, vfx 7; level, world_render 8)
9  ohao_engine ohao_game_api         (api.hpp becomes a source of ohao_game_api, so its includes are checked)
10 ohao_api
11 ohao_rpc ohao_runtime
12 the games
```

Modules' levels are computed from `OHAO_MODULE_DEPENDS` at configure, never typed; the libraries above them are
placed relative to the highest module level.

### 5.3 The enforcement

**Measured gap first:** the layer check runs only at configure, and `build/CMakeFiles/common.ninja:7049` re-runs CMake
only for CMake files, the shaders and the glob verification: an `#include` added to an existing `.cpp` or `.hpp` is
never checked until something else reconfigures. So the scan moves to Python, `tools/check_layers.py`, called by
configure (`execute_process`, so configure still fails) and by `tools/check.py` on every run, which the pre-commit
hook runs. One implementation of the scan; CMake stays the one source of the graph: configure writes
`build/check/modules.json` (each library's level, sources and headers; each module's DEPENDS).

The rules, each an error naming the file, the line and the rule:
1. Levels, as today (links and includes point down), and the games include only `game/api.hpp` (as today).
2. A module's source includes another module's header only if that module is in its DEPENDS (direct, not transitive):
   `ohao/animation/animation.cpp:5 includes physics/physics_scene.hpp: animation does not declare physics`.
3. A path under `<lib-dir>/private/` is included only by sources of the library that owns it.
4. DEPENDS names only modules that exist, and the declared graph has no cycle (the cycle printed in order).
5. Quoted and angle-bracket includes both (the library scan reads only quotes today, `ohao_layers.cmake:137`).

Its cost is measured at the build: the 4b-2 thread rule's scan is the reference; a check that adds more than 50 ms
is made faster before it lands.

## 6. The core, audio, world_render and the slice (decision 4)

### 6.1 `ecs::World` forgets spin, fly and transform

- The constructor keeps only what makes a World consistent: the id index's hooks, the hierarchy's hooks, the
  InputActions hooks (`world.cpp:56-73`). It installs no system and includes no system header.
- The core module (`ecs/core.cpp`, `ecs::coreModule()`) registers the core's types and installs `transform`
  (sync), `spin` and `fly_controller` (simulate) with today's declarations. Every module depends on it, so every host,
  bench and tool that installs any module installs it. A World with no module installed has no systems; every host that makes a World
  without `installEngine` and relied on the constructor's systems (found by grep at commit 11: `scene_bench`,
  `serve_bench`, the doctor and the benches' worlds are the candidates) calls `engine::install(world, services, {"ecs"})`.
- **The move tag** (edge 3): `ecs/transform_system.hpp` gains `template <class Tag, class... Holders> void
  tagMoves(World&)`: every entity the transform system recomputes that holds one of `Holders` gets `Tag`. The rules
  are a context entry; each is two function pointers (holds, tag). The physics module installs
  `tagMoves<PhysicsMoved, PhysicsBody, ColliderPart>`. The rule's reads and writes join the transform system's
  declaration through a new `World::addAccess(std::string_view system, std::vector<ComponentType> reads,
  std::vector<ComponentType> writes)` (as a signal's effect joins its writers'), so the install check sees exactly
  today's declaration. Order of tags per entity: RenderDirty, then the rules in install order (physics only): today's.
  Cost: one indirect call per recomputed entity; measured on `crowd_sync_ms` (budget 0.30) at the commit.

### 6.2 The audio schedule's includes of animation and physics

Legitimate, declared: audio DEPENDS physics (its FootEvent channels, `raycastFrom` for occlusion, `playerInfo` for the
velocity and ground surface, PlayerController for the character and eye height, CollisionMesh for a zone's fit) and
animation (the ClipEvent channel, Animator). Narrowed: `footFromClips` (`audio_schedule.cpp:449-462`) iterates the
bound graph's clips; it calls `ecs::graphFires(world, path, {"foot_l", "foot_r"})` from `animation/animation.hpp`
instead, and the schedule stops including `anim/anim_graph.hpp`. Not made data (footsteps as cues of the bank): the
surface-dependent sound and the speed scale are code today; data for them is a feature, not this refactor.

### 6.3 world_render

A module: `ohao_world_render`, DEPENDS ecs, visual, animation, vfx, LINKS `ohao_renderer`, `ohao_gpu_vulkan`. Its
`needs()` are `render::RenderScene` and `BindlessTextureManager`. Its install is today's `installRendering`
(instantiate, render_extract, vfx_draw). Public: `present.hpp` (the game's HUDs and spheres) and `components.hpp`
(MeshRenderer); RenderSlot and InstantiateDirty are private. It reports presenters as its contributions (their ids as
text). 4b-7's frame packet is then written here, once.

### 6.4 The slice

Unchanged rule: it includes `game/api.hpp` only. `game/api.hpp` lists the modules' public headers it exports (the
lines that name `ecs/animation.hpp`, `ecs/physics_scene.hpp`, `ecs/audio_schedule.hpp`, `ecs/components.hpp` become
`animation/animation.hpp`, `physics/physics_scene.hpp`, `physics/components.hpp`, `audio/audio_schedule.hpp`,
`visual/components.hpp`, `ecs/components.hpp`, ...). `SliceModule` declares `dependencies()` {physics, animation,
audio, vfx, world_render, visual} and its `install` takes `(World&, const Services&)`; the sample game the same. The
slice's code does not otherwise change (it names components unqualified inside `ohao::slice`).

## 7. CMake: `ohao_add_module`

`cmake/ohao_modules.cmake`:

```cmake
ohao_add_module(<name> DEPENDS <module>... LINKS <library>... [SOURCES <explicit list>])
```

- A static library `ohao_<name>` from its directory's globbed sources (with `private/`), PUBLIC include `ohao/`,
  linking `ohao_<dep>` for each DEPENDS (PUBLIC) and LINKS.
- Writes `${CMAKE_BINARY_DIR}/generated/modules/<name>_dependencies.hpp` (only when its text changes, so an
  unchanged configure rebuilds nothing): `inline constexpr std::string_view k<Name>Dependencies[] = {...};`, which the
  module's `dependencies()` returns. CMake's DEPENDS is the one source of the graph; C++ cannot drift from it.
- Appends to `OHAO_MODULES` and `OHAO_MODULE_DEPENDS_<name>` (global properties) for the levels and the scan.
- The core module is `ohao_add_module(ecs LINKS ohao_core ohao_asset ohao_cook ohao_reflect EnTT::EnTT ...)`.

## 8. Order inside the build: why commit 1 changes the hash

Today three things depend on the order in which types are registered: `world.hash` (`world_hash.cpp:50`,
`snapshotTypes` walks `types.types()`), the snapshot's columns (`snapshot.cpp:74`) and each saved entity's component
order (`scene_io.cpp:41`, `savedTypes`). Today's registration order interleaves the modules (Light, then Spin, then
PlayerController, then Level, then Collider...), so any grouping by module changes all three. Event hashing does not
depend on it (`events.hpp:394-409` walks in seq order) and snapshot hooks are already sorted by name
(`snapshot_hook.cpp:12`).

**Decided:** these three walk the component types in name order, once and for all. Then the order in which modules
register, or a component moving from one module to another, never changes a hash or a file again: the same principle
as the systems' order ("a function of the systems installed, never of who installed them"). The catalogue keeps
registration order (now module order: the docs read by module). Refuted: keeping an explicit order table preserves
today's hashes but stores the order as a second fact that every new component must be added to by hand.

## 9. The commits (decision 5)

Each commit: green hooks; `replay_test` 12/12 with its 6 checkpoint hashes equal to commit 1's; goldens exact;
`physics_bench`'s hashes (the crowd, boxes1k, events_1k) at 1 and 31 threads equal to commit 1's. Measured against
the 4b-6 build where it says so.

1. **The canonical orders** (the one deliberate change). World hash, snapshot columns and save walk types by name;
   `scene_types.cpp` and `event_types.cpp` regrouped into the final module order (ecs, visual, animation, physics,
   audio, vfx, level, world_render), each group under `setModule("<module>")`; `installEngine`'s and
   `installRendering`'s calls reordered to that order. Named effects: every `world.hash` value (the crowd's
   `7cb6071c97e14b81` gets a new value, recorded at 1 and 31 threads), saved entities list components alphabetically
   (checked-in scenes are not rewritten: loading is order-free; each re-saved scene's world hashes as the original
   loaded), the catalogue's order, module fields and `schemaHash`. Goldens exact. Proven: the registration order
   shuffled by hand (not committed) leaves every hash equal; the old walk with the shuffle changes them. If the
   reordered install changes a hash beyond the type order (signal handlers or EnTT pool order), the pair is found by
   bisecting the order and made order-free in this commit.
2. **The core generalized** (refactor, exact). The annotations `.asset(kind)`, `.entityNotSaved()`, `.snapshotted()`
   in `reflect/registry.hpp` and their uses in `scene_io.cpp` and `snapshot.cpp`; `tagMoves` and `World::addAccess`;
   W_UNSAVEABLE_MESH deleted. checkAssets' diagnostics identical on `tests/scenes/v1/*` and a scene naming every asset
   kind missing (by hand).
3. **The module interface and the enforcement.** `ecs/module.{hpp,cpp}`, `Services`, the core module (types only;
   the constructor still installs its systems), `engine::modules()` = {ecs} then today's direct calls for the rest,
   `engine::registerTypes`, `ohao_add_module`, `tools/check_layers.py` (from `ohao_layers.cmake`'s scan, then rules
   2-5) run by configure and `check.py`, game aliases and the new `install` signature. Proven: an include added to an
   existing file without a reconfigure (a level violation) is now refused by `check.py` (today's check misses it).
4. **visual.** `ohao/visual/`: its components, `primitive_mesh`, `scene_view`, `spawnLight`, `types.cpp`. Every user
   includes `visual/components.hpp` itself (the final shape; `ecs/components.hpp` cannot include it: rule 1).
5. **physics.** `ohao/physics/`: the backend into `private/backend/`, `physics_scene*`, `player.*`, components, events,
   `types.cpp`, `methods.cpp` (five methods), the move tag, the control rule as a contribution, `hasBody`. Benches
   install it through `engine::install(..., {"physics", ...})`. Measured: every `physics_bench` scenario and
   `crowd_sync_ms` against the 4b-6 numbers, within the spread.
6. **animation.** `ohao/anim` and `ecs/animation.*` into `ohao/animation/`; `animation.info`; drivers as
   contributions; `graphFires`. **Planted, each refused by `check.py` with file and line:** P1 `animation.cpp`
   including `physics/private/physics_scene_impl.hpp` (rule 3); P2 `animation.cpp` including
   `physics/physics_scene.hpp` without declaring physics (rule 2); P3 physics DEPENDS animation added while animation
   DEPENDS physics (rule 4, the cycle printed; configure also fails); P4 `ecs/world.cpp` including
   `physics/components.hpp` (rule 1).
7. **audio.** `ecs/audio_schedule.*` into `ohao/audio/` as the module; the mixer renamed; events, types.
8. **vfx.** `ecs/vfx.*` into `ohao/vfx/`; the effects library into `ohao/vfx/effects/`.
9. **level.** `gltf_structure` into `ohao/cook/`; `ohao_level_world` becomes `ohao_level`; `level.info`, `level.reload`.
10. **world_render.** The module with `needs`; `installRendering` deleted; hosts call `engine::install` again with the
    renderer; presenters as contributions; `game/install.cpp` generic. Planted: P5 the slice installed on a World with
    only {ecs, physics}: `installGame` refuses it, naming animation, audio, vfx, world_render and visual.
11. **The core installs its own systems.** transform, spin and fly leave `World::World()` for the core module;
    `ecs/components.hpp` is the core's 150 lines; `game/api.hpp`'s list final. Measured: the edit loop of section 2
    again (3 runs each: a field in `animation/components.hpp`, `animation/animation.cpp`, `animation/types.cpp`, a
    field in `ecs/components.hpp`), every bench against the 4b-6 numbers, TSan's `replay_test` none.

Each later commit deletes the old lines it replaces (the direct install calls, the sections of components.hpp and
scene_types.cpp); none rewrites what an earlier one wrote.

## 10. Risks

- **The install order changes in commit 1** (level moves from first to after vfx, animation before physics). If a
  signal's handler order or EnTT's pool creation order reaches the state (a `registry().clear()` empties pools newest
  first, firing physics' and instantiate's destroy handlers), the hashes change for a reason other than the type
  order. Commit 1 separates the two by bisecting.
- **Catalogue churn for agents:** methods gain a `module`, eight methods move after the session's in the catalogue and
  exist only where their module is installed. Every serving host installs all eight, so no method disappears for
  `ohao serve` or `ohao run`; `schemaHash` changes once.
- **The edit loop gain is partial** (section 2): the game PCH and the benches include every public header. Recorded
  as measured, with the next step named, not claimed as seconds.
- **The scan is a regex:** an include inside `#if 0` or a comment counts, one built from a macro is missed (as today).
- **Hot reload:** `game::Module` becomes an alias, and its `install` takes Services. A game built before this part must
  be rebuilt (no binary ABI promise, system design 4.4); both games are rebuilt by the same build.
- **The move tag** adds an indirect call per moved entity in the transform system, which the 4b-6 sync budget
  (0.30 ms, measured worst median 0.219) has room for; measured, not assumed, at commit 5.

## 11. Docs at 4b-10 (decision 6)

- **AGENTS.md:** "Systems are installed by `engine::installEngine` and `engine::installRendering`" becomes "by
  modules: `engine::install(world, services)` installs every engine module in dependency order (the renderer's
  module when the host has a renderer), then the game's"; a new rule: "a module includes only its declared
  dependencies' public headers; `private/` is its own; between modules only components and events (`check.py` holds
  it)"; Build: "a new module is `ohao_add_module(name DEPENDS ...)` in its directory"; the edit loop's numbers for a
  component edit, as measured at commit 11.
- **ARCHITECTURE.md:** the Libraries table becomes the module table of section 3 plus the libraries below and above;
  "Dependency rules" gains the module rules; "Recipe: add a component" (its module's `components.hpp` and `types.cpp`),
  "Recipe: add a system" (in its module's install; `World::World()` installs nothing), "Recipe: add an API method"
  (a module's `methods.cpp` with its params registry, or the session's when it needs the host); a new "Recipe: add a
  module".
- **docs/games/README.md:** `game::Module` is `ecs::Module`: `dependencies()`, `install(World&, const Services&)`;
  the game API's headers by module; what `uninstallGame` takes back (systems, hooks, and each module's contributions);
  plus 4b-4's list already noted in the plan.
- **docs/agents/ohao-cli.md:** the component table grouped by module, the methods' module, `W_UNSAVEABLE_MESH`'s row
  deleted.
- **The spec, 4.1 and 4.2:** the Engine services layer as modules; "hosts differ by config, never by sequence" as
  built (`engine::install` with `only`).
- **`.claude/skills/add-component`:** it names `components.hpp` and `scene_types.cpp`; it names the module's files.
