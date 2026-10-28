# Sub-project 10 — Animation: skeletons, clips, GPU skinning, the enemy's body and the player's arms

Status: written 2026-10-28, after Frank's "Continue" on the SP9 overnight run, as Day 1 of
sub-project 10 (documents only; no code). Frank deferred his rulings on content, motion scope,
the skinning path and the order to this row. They are written under "Rulings for Frank" as
**PROVISIONAL** (recommended, not yet ruled), and the design follows them. Every other judgment
call is listed there as **OPEN**, with a recommendation. The spec is binding once Frank accepts
it.

Roadmap: row 10 of `docs/ROADMAP.md`. Done when two skinned enemies run, aim, shoot, flinch and
die, validation-clean in raster and RT (shadows and GI see the deformed mesh); culling and
determinism tests cover skinned meshes; a skinning budget exists; and the viewmodel's reload
matches the weapon's.
- **Before it:** 9, Feel. It is built and waiting for Frank's hand test and pacing ruling.
  Nothing here depends on those rulings.
- **After it:** 11, Audio.

Builds on:
- **Sub-project 2a/2b** (`2026-10-03-ecs-gpu-scene-design.md`,
  `2026-10-24-renderer-upgrade-design.md`):
  - the persistent `RenderScene`: one shared vertex arena (`MeshRegistry`), one BLAS per mesh,
    and the per-frame TLAS build or refit (`RenderScene::recordTlas`);
  - the instance table (`InstanceGpu`, `prevWorld` for motion vectors);
  - GPU culling over `DrawRecord` bounds, meshlets with spheres and cones
    (`render/scene/meshlet_builder`);
  - the pass registry, `render_bench`, and `MAX_FRAMES_IN_FLIGHT` = 3.
- **Sub-project 5** (`2026-10-26-level-pipeline-design.md`): the glTF structure reader
  (`ohao/level/gltf_structure.cpp`, simdjson), which today flags skins and animations as
  unsupported; `ModelLoader::loadMesh` (one mesh at its own origin); `ModelAsset.mesh`.
- **Sub-project 6** (`2026-10-26-player-physics-design.md`): the fixed 60 Hz step, characters
  (`PlayerController`, `PlayerState.velocity`, `onGround`, `sprint`), render interpolation
  (`RenderPose` at the World's render alpha), and snapshots that replay bit for bit.
- **Sub-project 7** (`2026-10-27-combat-design.md`):
  - the combat step and its event log (`Shot`, `Hit`, `Damage`, `Death`, `Reload`, `Reloaded`,
    `RoundStart`, `Restart`, `Win`, each with `step` and `seq`);
  - `Weapon.reloadTime`, `WeaponState.reloading`;
  - the enemy AI (`EnemyMode` `Idle`, `Chase`, `Attack`, `Dead`; `MoveIntent`);
  - the round's restart through snapshots;
  - the stand-in enemies, which are boxes: a body, a head and a gun per enemy.
- **Sub-project 9** (`2026-10-27-feel-design.md`):
  - the look lead and frame timing;
  - `ViewModel` with its view-space remap, sway, bob and kick at render rate (`World::presentationDt`);
  - `kDrawViewModel` and `kTlasMaskViewModel`;
  - combat sounds through the window's one `AudioSystem`;
  - `feel_bench`.

## Thesis: a pose is a function of the step

A character's pose is computed, never accumulated. Each step advances every animator by whole
steps:
- the state each layer is in;
- how many steps it has been in that state;
- the transition being blended.

These are saved, hashed and snapshotted like `WeaponState`. The pose is then a pure function of
that state, the graph and the clips, sampled at `(steps + alpha) × 1/60 s`:
- alpha is 1 in lockstep, headless hosts and every capture;
- alpha is the timeline's alpha in a realtime window, so the motion is drawn at render rate as
  `RenderPose` interpolation is.

The pose itself is presentation. It is never saved, hashed or read by a step, so a restore, a
replay and a window-against-headless capture give the same pose bit for bit.

Gameplay keeps its own geometry:
- rounds hit the capsule and the body boxes, as in SP7;
- what the animation decides that gameplay hears comes back as **events** emitted in the step,
  in step order: a footstep, a magazine out, a magazine in, a death fall. They are as
  deterministic as combat's events.

The deformed mesh is the one mesh every consumer reads. The G-buffer, the shadow cascades, the
meshlet path, RT shadows, RT GI and the path tracer all reach a skinned character through the
same vertex arena and its own refitted BLAS. Nothing renders the bind pose, so a shadow or a GI
bounce never disagrees with what the camera sees.

## Goal

On `ohao_game assets/scenes/blockout_play.scene.json`, the two enemies are skinned humanoids
instead of boxes. They:
- stand idle, walk and run with their speed;
- aim at the player (upper body to the pitch of their aim);
- fire with a recoil on each round, and reload with the weapon's timing;
- flinch when hit;
- play a death fall and stay down.

Their handling sounds (magazine out and in) and their death fall play from animation events.

The player holds a pair of arms and a rifle in place of the box. They idle, fire, reload in
exactly the weapon's `reloadTime`, aim down the sights, and lower the gun while sprinting, on
top of SP9's sway, bob and kick.

Everything renders validation-clean in deferred and in the path tracer, with shadows, GI and
motion vectors of the deformed mesh. Culling and determinism tests cover skinned meshes, and
the skinning cost is gated.

## Scope: in 10 vs later

In 10:
- glTF skins and clips read by our own reader;
- `Skeleton`, `Clip` and `AnimGraph` assets;
- the components `SkinnedMesh`, `Animator` and `JointAttach`;
- an `animation` simulate system (states, transitions, events);
- a `pose` sync system (sampling, blending, layers, palettes, attachments);
- GPU compute skinning into each skinned instance's own vertex range in the arena;
- per-meshlet and per-draw bounds of the deformed mesh;
- an updatable BLAS per skinned instance, refitted each frame;
- previous-pose positions for motion vectors;
- the enemy's full body wired to the AI and combat;
- the player's arms and gun (idle, fire, reload, ADS, sprint);
- animation events driving handling sounds;
- `animation.info` for agents and `skinning` in `render.stats`;
- test 22 to 25, budgets, goldens, look shots and Frank's hand test.

Later:
- **Not in this project:** a face (blend shapes or morph targets), cinematics, a third-person
  player.
- **13:** ragdolls, and per-bone hitboxes (rounds stay on the capsule and boxes).
- **Later, by need:**
  - root motion (the controller owns the root: provisional ruling 2);
  - foot and aim IK (aim is a blend of authored poses here);
- **15:** clip compression and cooked clips (asset database).
- **11:** footsteps synced to the feet; physics' distance-based footsteps stay.
- **Not in 10:** retargeting between rigs (OPEN ruling 9), crowd-scale animation (instancing,
  LOD of skeletons), and animation on non-character props.

## Why (what the code does today)

- **Joint data arrives and dies.** Every `Vertex` (100 bytes, `scene/asset/model.hpp`) carries
  `boneIndices` (`ivec4`) and `boneWeights` (`vec4`). Assimp fills them (`LimitBoneWeights`, a
  file-wide bone-name map, `model_loader.cpp:456-533`), and ufbx fills them for FBX. Nothing
  reads them: "Skeletal animation removed" (`model_loader.cpp:562-565`). The G-buffer and CSM
  vertex shaders declare locations 6 and 7 and never use them.
- **Assimp bakes node transforms into whole-file loads.** That is wrong for a skin, whose
  vertices are in bind space and are placed by joints, not by the mesh node. `loadMesh` emits
  mesh-local geometry and is the path skins take.
- **The glTF reader only counts.** `ohao/level/gltf_structure.cpp` reads nodes, lights, cameras,
  extras and accessor counts. Skins and animations go to `unsupported`
  (`W_LEVEL_UNSUPPORTED`). No accessor data is decoded.
- **Geometry is per mesh, not per instance.** The G-buffer, the meshlet path and the path
  tracer's hit shaders all find vertices through `InstanceGpu.mesh` → `MeshGpu.vertexOffset` in
  one arena (`pt_closesthit.rchit` reads normals and UVs from it by binding). Two enemies on one
  mesh cannot each be deformed by writing the mesh.
- **BLAS are static.** Each is built once, `PREFER_FAST_TRACE`, without `ALLOW_UPDATE`
  (`rt_acceleration_structure.cpp:249`). `createBLASFromPositions`, a full rebuild "for
  per-frame animated mesh BLAS", has no caller.
- **Bounds are the bind pose's.** Culling transforms the mesh's load-time AABB
  (`instances.slang` `classify`), and meshlets carry load-time spheres and cones. A raised arm
  outside the bind box would be culled away.
- **Motion vectors are rigid.** `gbuffer.vert` and `meshlets.slang` project the same object
  position through `prevWorld`. A limb moving in place has no motion vector, so TAA and the
  temporal denoisers smear it.
- **There is no step count on the World.** Counters live in the timeline, the session, combat
  and physics. Clip time must come from saved animator state.
- **Death is a rigid tip.** `enemy_ai.cpp` `lieDown` rotates the saved `Transform` 90° about X
  and lifts it. It is saved and hashed, and asserted bit for bit across restarts by test 16/17.
- **Two skinned glTFs sit in the repo unused and unlicensed:** `assets/test_models/Fox.glb` and
  `BrainStem.glb`. No licence file is beside them; upstream they are Khronos samples under
  CC BY 4.0, an attribution licence. No code or scene references them.

## Assets and content

- **Rigs and clips are glTF 2.0 (`.glb`).** A file's skin gives the joints (node hierarchy, rest
  TRS, `inverseBindMatrices`). Its animations give clips (channels of translation, rotation and
  scale; samplers `LINEAR`, `STEP`, `CUBICSPLINE`). Morph-target channels are refused with a
  warning (no faces in 10).
- **Content (PROVISIONAL ruling 1):** CC0 humanoids committed to the repo, with their licence and
  upstream note beside them, as `assets/khronos/` does:
  - `assets/characters/<name>/LICENSE.md` and `README.upstream.md`;
  - Quaternius- or Kenney-style packs, chosen for a rifle-holding set: idle, walk, run, aim, fire,
    reload, hit, death.
  - Not Mixamo (its terms forbid redistributing the files). Not Somnabulate art, which stays in
    git-ignored `assets/local/`.
  - Where a needed clip is missing (an aim pose, a rifle reload), it is made in Blender by a
    script under `tools/anim/`, ours (MIT), like the levels.
- **The player's arms and rifle:** CC0 if a suitable rig exists, else generated by
  `tools/anim/make_fps_arms.py` (Blender 5.2.2, rebuilt byte for byte by a test that skips
  without Blender, as `check_level_assets.py` does). Their clips are idle, fire, reload, ADS
  in, ADS hold and sprint. Reload carries `mag_out` and `mag_in` events.
- **The test rig:** `tools/anim/make_test_rig.py`, pure Python with no Blender, writes
  `tests/anim/fixtures/rig.glb`, a 4-joint jointed column with:
  - known weights;
  - clips `wave` (looping), `raise` (one shot), `step` (`STEP` sampling) and `spline`
    (`CUBICSPLINE`);
  - events, and a joint swung far outside the bind box.

  Every unit and GPU test uses it, so the tests do not depend on art.
- **`Fox.glb` and `BrainStem.glb`:** given licence files, or deleted (forward only; nothing uses
  them). OPEN ruling 10.

## The data model

### Assets (no ECS, no Vulkan: a new library `ohao/anim`)

- `Skeleton`: joints in parent-before-child order, each with a name, parent, rest TRS and
  inverse bind matrix. Made from a glTF skin by the reader (below), cached per `(file, skin)`.
- `Clip`: per-joint channels (keys as authored, times in seconds), a duration, and events
  `{t, name}`. Read from a glTF animation, bound to a skeleton by joint name at load (a channel
  whose node is not a joint is dropped with a warning). Sampling binary-searches the keys.
  Rotations are normalised `nlerp` for `LINEAR` and the glTF Hermite form for `CUBICSPLINE`, in
  `float`, in a fixed order: the same inputs always give the same bits.
- `AnimGraph` (`.animgraph.json`, scene data, validated against a JSON Schema that
  `ohao schema` exports):
  - **`clips`:** name → `{file, animation, loop, rate | rateFrom}`. `rateFrom: "weapon.reloadTime"`
    scales a clip to the weapon's time.
  - **`params`:** names and types. They are written by drivers (below), never by the graph.
  - **`layers`:** each has a joint mask (names, with all descendants), a blend mode (`replace` or
    `additive`) and a weight.
  - **`states` per layer:** a clip, or a 1-D blend of clips by a parameter (idle / walk / run by
    speed, aim down / level / up by pitch). Each state names its exit transitions:
    `{to, when: param comparison | event | clip end, fadeSteps}`.
  - **`events`:** per clip, `{t, name}`, added to the clip's own (glTF has no event channel).
- Every value is checked at load:
  - a parameter no driver writes;
  - a state with no way out;
  - a mask joint that is not in the skeleton;
  - an event time outside the clip.

  Each is an error with a path, as scene errors are.

### Components (`ecs/components.hpp`, registered with docs, units and limits)

- `SkinnedMesh` (saved): `{ AssetRef file; std::string skin; }`, beside the entity's
  `ModelAsset`/`MeshSource`, whose mesh must be the skin's. Instantiate gives the instance its
  own deformable copy (below).
- `Animator` (saved):
  - `{ AssetRef graph; std::vector<AnimLayerState> layers; }`.
  - Each `AnimLayerState` is `{ std::string state; uint32_t steps; std::string from;
    uint32_t fromSteps; uint16_t fade; uint16_t faded; }`: the state, the steps spent in it, and
    the transition being blended.
  - Integers only, so clip time never drifts and a snapshot holds it exactly.
  - Agents may force a state with `component.set` (a normal saved-component edit).
- `JointAttach` (saved): `{ std::string joint; }` on a child entity. It is drawn at that joint
  of its parent's pose times its own `Transform`: the enemy's rifle in the right hand, the
  muzzle light at the rifle's tip.
- `AnimParams` (runtime, never saved): the drivers' parameter values for this step, rebuilt
  every step from saved state (below), so they need not be hashed.
- `AnimPose` (runtime): the sampled local and model-space joint matrices and the skinning
  palette of the current and the previous drawn frame. Presentation.

Three new saved types move every `world.hash` in the serve and CLI expectations (their names
are hashed). They are re-baselined once with that reason, as SP7 and SP9 did.

## The step: states, transitions, events

- **Drivers** (code, `ecs/enemy_anim.cpp` and `ecs/arms_anim.cpp`) run at the start of the
  `animation` simulate system. They write `AnimParams` from saved state only, so a restored
  world rebuilds them identically.
  - **Enemy:** `speed` (horizontal `PlayerState.velocity`), `aimPitch` (`MoveIntent.pitchDeg`),
    `mode` (`EnemyState.mode`), `alive`.
  - **Enemy, this step's combat events:** `shot`, `hit` (damage taken), `reload`, `death`.
  - **Player's arms:** `moving`, `sprinting`, `aim`, `shot`, `reload`, `alive`.
- **The `animation` simulate system** runs after `combat` (installed by `installCombat`, next to
  `view_model`). For each `Animator` in id order and each layer in order:
  - it evaluates the current state's transitions against the parameters, the step's events and
    clip end;
  - takes the first that holds;
  - advances `steps`, `fade` and `faded` by one;
  - emits each clip event whose time falls in `((steps − 1)·dt, steps·dt]` × rate, looping
    included, into the animation event log.

  The log is a ring of 1,024 with `seq` and `step`, read by cursor, as combat's is: `{step, seq,
  entity, layer, clip, name}`. Nothing in the step reads a pose.
- **What the events drive:**
  - **Sounds**, in the window host's `CombatSounds`: `mag_out`, `mag_in`, `bolt`, `body_fall`,
    each from `assets/audio/weapons/` (generated, ours), distance-shaped and occluded for enemies
    as SP9's are.
  - **Gameplay stays where it is:** the round fires on combat's step, and the reload completes
    on `Reloaded`. The arms' reload clip is scaled so its `mag_in` lands within one step of
    `Reloaded` (`t23_reload_mag_in_on_reloaded`).
- **Restart:** the round's restore puts every `Animator` back with the snapshot. The log's
  cursor resets with combat's.

## The frame: poses, palettes, attachments

- **The `pose` sync system** runs after `view_model` and before `render_extract`. For each
  entity with `Animator`, `SkinnedMesh` and a visible instance:
  - sample every layer at `(steps + alpha)` steps, the blend partner at `(fromSteps + alpha)`
    with weight `(faded + alpha) / fade`;
  - combine layers by mask and mode;
  - compute model-space joints;
  - compute the palette `model × inverseBind` for the current frame, keeping last frame's as
    the previous palette.

  It also writes each `JointAttach` child's `RenderPose` from its joint, `prev` from last
  frame's.
  - Lockstep and captures (alpha 1, `presentationDt` 0): the step's pose exactly.
  - A placement (restart, teleport, spawn) resets the previous palette to the current one, as
    `RenderPose` does across placements: no motion vector across a jump.
- **The player's arms** are the `ViewModel` entity's skinned child. `view_model` places the
  arms' root (remap, sway, bob, and kick unless ruling 8 says otherwise); `pose` then poses the
  arms inside it. ADS:
  - blends the arms toward the ADS pose;
  - narrows the drawn field of view from the scene's 75° toward 55° at render rate (presentation);
  - halves the weapon's spread while the aim fraction is 1, a gameplay effect, saved in
    `WeaponState.aim` and stepped over `Weapon.adsTime` (OPEN ruling 11).

## The renderer: skinning, bounds, BLAS, motion vectors

### One deformable mesh per skinned instance

- When the render extract first meets a `SkinnedMesh` instance, it asks
  `RenderScene::registerSkinnedInstance(baseMesh)` for a **mesh of its own**: a new range in the
  shared vertex arena, the size of the base mesh's, filled with the bind pose on the GPU. The
  new range has:
  - the base mesh's index range and triangle material slots;
  - a copy of its meshlets;
  - its own BLAS.

  The instance's `InstanceGpu.mesh` points at it. The G-buffer (both paths), CSM, the meshlet
  task shaders and the path tracer's hit shaders then read deformed vertices with **no change to
  how they find them**. The range is released with the instance through the retire queue.
- A new instance flag `kInstanceSkinned` and an offset into a new **previous-position stream**
  (`vec3` per vertex, its own buffer, `STORAGE`) are added to `InstanceGpu` (`layout.json`).
  `gbuffer.vert` and `cull/meshlets.slang` take the previous object position from that stream
  for skinned instances, and `inPosition` for everything else. `shadow_csm.vert` needs none.

### The passes (in frame order, before culling and before the TLAS update)

1. **`skin`** (compute, `shaders/skin/skin.slang`, one thread a vertex). For each skinned
   instance:
   - it reads the base mesh's bind-pose vertex, its 4 joints and weights, and the current and
     previous palettes (a per-frame-slot palette buffer, host-written: palettes are
     `MAX_FRAMES_IN_FLIGHT`-buffered because the CPU writes them);
   - it writes the deformed position, normal and tangent into the instance's range (colour, UVs
     and the joint data copied unchanged);
   - it writes the previous position into the previous-position stream.

   A fixed operation order (weights summed in index order, no FMA contraction left to the
   compiler where it would change bits: the same rule as `meshlets.slang`'s copy of
   `gbuffer.vert`) keeps it exact run to run. One queue and a barrier at the pass's start order
   it after the previous frame's reads of the same range, so the range needs no per-slot copy.
2. **`skin.bounds`** (compute, one workgroup a meshlet). Each workgroup reduces its ≤ 64
   deformed vertices to the meshlet's bounding sphere, written into the instance's meshlet
   copy, and the instance's draws get their AABB the same way: a fixed-order reduction with no
   atomics (the float-atomics lint holds). A skinned meshlet's cone is written as "always facing"
   (cutoff −1): a deforming cluster has no stable normal cone.
3. **`rt.blas.refit`:** each skinned instance's BLAS, built `ALLOW_UPDATE | PREFER_FAST_BUILD`,
   is updated (`MODE_UPDATE`) from its range. It is rebuilt instead after `kBlasRefitsPerRebuild`
   refits or when its state's clip changes (OPEN ruling 12). The TLAS instance keeps the same
   BLAS reference, so the TLAS stays a refit (`recordTlas`'s rule is unchanged). A pose that did
   not change (a dead enemy at the end of its fall, lockstep with nothing stepped) skips all
   three passes for that instance.

`createBLASFromPositions` (unused, packed `vec3`) is deleted. The refit lives on
`RTAccelerationStructure` beside `createBLAS`.

### What each consumer sees

| Consumer | Sees |
|---|---|
| G-buffer, both geometry paths | the deformed vertices; motion vectors from the previous positions |
| CSM cascades | the deformed vertices (a shadow of the pose) |
| GPU culling | the deformed AABB and meshlet spheres; cones off |
| RT shadows and RT GI (ray queries) | the refitted BLAS |
| The path tracer (raygen and hit shaders) | the refitted BLAS, and the deformed normals and UVs through the instance's own mesh |
| The emitter and viewmodel masks | unchanged; the arms keep `kTlasMaskViewModel` and `kDrawViewModel` (no cascade, no scene ray), as SP9's gun does |

## The agent-facing surface

- **`animation.info {entity?, since?}`** returns:
  - every animator (or one), each as `{entity, name, graph, layers: [{state, clip, t, from?,
    blend?}]}`;
  - the animation events since a cursor, as `{events: [...], next}`.

  It stays under 1 KB for the play scene (an `animation_info` budget in `serve_bench`). One new
  tool against `tools/list`'s 24 KB cap (23,506 bytes used): OPEN ruling 13 if it does not fit.
- **Forcing a state** is `component.set` on `Animator` (saved), through `world.apply` like any
  edit, with its inverse.
- **`render.stats`** gains `skinning {instances, vertices, blasRefits, blasRebuilds}` for the
  frames read.
- **`ohao schema`** exports the `.animgraph.json` schema beside the scene schemas.

## Performance budgets

The display targets are 240 Hz (4.17 ms a frame, this laptop's panel) and 144 Hz (6.94 ms,
Frank's). The play scene's 1080p frame measured 2.2–2.5 ms before SP10 (`render_bench
play_fx_1080p`, `feel_bench`), so animation gets a small, fixed slice.

| Measure | Bench | Budget | Gated from |
|---|---|---|---|
| `skin` + `skin.bounds` passes, the play scene (2 enemies + arms) | `render_bench play_hud_1080p` | ≤ 0.05 ms | Task 2 |
| `rt.blas.refit`, the play scene | `render_bench play_hud_1080p` | ≤ 0.10 ms | Task 2 |
| `skin` + `skin.bounds`, 16 running enemies | `render_bench anim16_1080p` (new) | ≤ 0.25 ms | Task 1 |
| `rt.blas.refit`, 16 running enemies | `render_bench anim16_1080p` | ≤ 0.60 ms | Task 1 |
| Animation's whole GPU cost added to the play scene's frame | `render_bench play_hud_1080p` | ≤ 0.20 ms over its pre-SP10 median | Task 2 |
| `animation` simulate system, the play scene | `physics_bench anim_step_ms` | ≤ 0.01 ms | Task 2 |
| `animation` simulate system, 16 enemies | `physics_bench anim16_step_ms` | ≤ 0.05 ms | Task 1 |
| `pose` sync system, the play scene | `physics_bench pose_sync_ms` | ≤ 0.02 ms | Task 2 |
| `pose` sync system, 16 enemies | `physics_bench pose16_sync_ms` | ≤ 0.10 ms | Task 1 |
| Look latency (no regression) | `feel_bench` | `look_latency_ms` ≤ 15 ms | Task 3 |

- The `physics_bench` keys are gated by a new `animStep` key in `physics_budgets.json`.
- `look_latency_ms` keeps SP9's budget. `frame_p99_over_median` stays under SP9's open ruling 21
  and is reported, not newly gated.
- Memory is reported, not gated: per skinned instance, 100 bytes a vertex (the deformed range)
  plus 12 (previous positions) plus its BLAS.

## Determinism

- **Simulation:** animator state is integers advanced by whole steps. Transitions and events
  depend only on saved state and the step's events. Two runs, a restore mid-clip, a restart
  and a replay give the same `Animator` bytes, the same event log and the same `world.hash`.
- **Presentation:** a pose is a function of saved state and alpha; captures use alpha 1.
  - A skinned capture is bit-identical across fresh renderers, processes and repeated captures
    (deferred), and within the path tracer's BVH floor (1 LSB on a few pixels).
  - A lockstep window's capture equals a headless one bit for bit.
  - The skin pass's arithmetic is fixed-order; the bounds reductions have no atomics.
- **Death:** OPEN ruling 7 recommends the death clip replace `lieDown`'s rigid tip. The entity's
  saved `Transform` then stays upright, and test 16/17's lying-down assertions become "the
  `death` state reached, its clip ended". The restart's `world.hash` equality is unaffected.
- **Hashes:** the new saved types move `world.hash` expectations once (reason stated). The
  play-scene goldens move when the boxes become characters and the box gun becomes arms: looked
  at, explained, and re-baselined in their own commits.

## Test plan

Tests are numbered after SP9's (test 21): 22 assets and sampling, 23 the step, 24 skinning in the
renderer, 25 the slice.

- **Test 22** (`anim_tests`, a new CPU suite, the generated rig):
  - `t22_gltf_skin_read`: joints, parents, inverse binds, rest TRS equal the generator's.
  - `t22_gltf_clip_read`: every sampler kind and every channel path; times and values to the bit.
  - `t22_sample_linear_step_spline`: known values at keys and between them, `nlerp` normalised.
  - `t22_loop_and_clamp`.
  - `t22_layers_mask_additive`: an upper-body additive layer leaves the legs exact.
  - `t22_blend1d_by_param`.
  - `t22_palette_identity_at_bind`: the bind pose gives identity palettes to 1e-6.
  - `t22_graph_errors`: an unknown parameter, a state with no way out, a mask joint not in the
    skeleton, an event outside its clip; each is an error with its path.
  - `t22_assimp_joint_remap`: Assimp's file-wide bone indices remapped to the skin's joint order.
  - `t22_morph_refused`.
- **Test 23** (`anim_tests`, `combat_tests`, `api_tests`):
  - `t23_transition_on_param`, `t23_transition_on_event`, `t23_transition_on_clip_end`.
  - `t23_fade_steps`.
  - `t23_events_once_per_crossing`: looping and at clip end, none twice, none skipped at 60 Hz.
  - `t23_events_in_step_order`.
  - `t23_restore_mid_clip`: the same `Animator`, events and `world.hash` after a restore and
    the same steps.
  - `t23_restart_resets`.
  - `t23_reload_mag_in_on_reloaded`: the arms' `mag_in` within one step of `Reloaded`, for
    `reloadTime` 1.5 and 2.0.
  - `t23_enemy_states_from_ai`: idle, run with speed, aim, fire per `Shot`, flinch per `Damage`,
    death per `Death`.
  - `t23_animation_info`: the shape, under 1 KB, the cursor, `E_STATE` without animation.
- **Test 24** (GPU: `skinning_gpu_test`, a new suite; `cull_parity_gpu_test`;
  `deferred_determinism_gpu_test`):
  - `t24_skin_matches_cpu`: the read-back deformed range equals a CPU skin of the same palette
    within 1e-5 relative, positions and normals.
  - `t24_two_instances_two_poses`: two instances of one mesh in different poses render
    different shapes; neither writes the other's range.
  - `t24_motion_vectors_of_a_limb`: a limb moving in place has motion vectors equal to its
    projected displacement; the still parts have none.
  - `t24_rt_sees_the_pose`, three checks:
    - an RT shadow and an RT GI capture of the rig with its arm raised differ from the bind
      pose's where the arm's shadow falls, and nowhere a static mesh baked into the same pose
      would not;
    - the path tracer's image of the posed rig equals a static mesh baked into the same pose,
      within its floor;
    - a capture with the arm raised against the bind pose: the arm is in both the G-buffer and
      the path-traced image.
  - `t24_refit_and_rebuild`: refits counted, a rebuild after the threshold and on a clip
    change, the same image either way within the floor.
  - `t24_cull_parity_skinned` (`cull_parity_gpu_test` test 8): a rig whose bind box is off
    screen with its posed arm in view is drawn; culling on and off render the same image, both
    geometry paths and the meshlet fallback.
  - `t24_deferred_determinism_skinned`: bit for bit across fresh renderers and processes.
  - `t24_unchanged_pose_skips`: a still pose runs no skin, bounds or refit work.
- **Test 25** (`combat_tests`, `check_live.py`, `e2e_mcp.py`, `check_validation.py`, goldens):
  - `t25_play_scene_skinned`: both enemies are skinned and posed from the spawn's first frame;
    the player's arms are posed.
  - `t25_fight_states`: the done demo's fight, with each enemy's states and the events
    asserted.
  - `t25_capture_window_equals_headless`: mid-fight in lockstep, bit for bit.
  - `check_validation.py` gains the rig and the play scene in deferred, `rt_offline`,
    `rt_realtime` and NRD: zero messages.
  - Goldens:
    - new: `cli_anim_rig_deferred` (exact), `cli_anim_rig_reference` (exact) and
      `cli_anim_rig_pt` (floor), the rig mid-`wave`;
    - re-baselined after looking: `cli_blockout_play_deferred` and `cli_blockout_play_hud`.
  - `e2e_mcp.py` reads `animation_info` mid-fight and forces a state through `world_apply`.
- Every existing suite stays green; `check.py --all`.

## Hand-test acceptance (from Frank)

Empty until Frank plays `build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json
--latency`. Each item is closed by his note, a fix with its test, or a ruling that it stays.

| # | Item | Frank's note | Status |
|---|---|---|---|
| A1 | (Frank) | — | — |

## Done criteria

- Two skinned enemies on the play scene run, aim, shoot, flinch and die; their handling sounds
  and fall come from animation events.
- The player's arms idle, fire, reload in the weapon's time, aim down the sights and sprint.
- Validation-clean in raster and RT, with shadows, GI and the path tracer showing the deformed
  mesh.
- Tests 22–25 pass. Culling and determinism tests cover skinned meshes.
- The skinning, refit and animator budgets are gated.
- Every simulation test is unchanged except the rulings' stated edits (death, ADS);
  `check.py --all` green.
- The hand-test table is closed by Frank's notes.

## Build order

1. **Skeletons, clips and GPU skinning on the test rig:**
   - the glTF reader (skins, accessors, animations, the GLB binary chunk);
   - `ohao/anim`, the graph format and its schema;
   - the components, the `animation` and `pose` systems;
   - the skinned-instance mesh, the `skin`, `skin.bounds` and `rt.blas.refit` passes,
     previous-position motion vectors, culling;
   - `anim16_1080p` and the CPU benches;
   - tests 22, 23 (without the slice's drivers) and 24;
   - the rig goldens;
   - `animation.info` and `render.stats` `skinning`.

   Nothing in the play scene changes yet.
2. **The enemy's full body:** content in with licences, the enemy graph and driver, `JointAttach`
   rifle and muzzle, the death ruling applied, event sounds, the play-scene enemies replaced,
   tests 23 and 25 for enemies, the play scene's budgets, the goldens re-baselined.
3. **The player's arms and rifle:** the rig and clips, the arms graph and driver, the reload
   scaled to the weapon, ADS and sprint, composed with SP9's `ViewModel`, the look-latency
   check, test 25 complete.
4. **Close:**
   - `check.py --all` and the benches;
   - look shots in `C:\Users\djmax\ohao_refs\look_shots\sp10_anim\`;
   - the change records;
   - AGENTS.md, ARCHITECTURE.md, the guides, and a monograph `animation` unit;
   - the roadmap row and Frank's hand test.

Each step ends with the engine running, every gate green, a change record
(`docs/changes/YYYY-MM-DD-animation-stepN.md`) and look shots `stepN_*.png`.

## Risks

- **The frames in flight and one deformed range.** The range is GPU-written each frame and read
  by the same frame's passes and by the previous frame still executing. One graphics queue and a
  barrier at the `skin` pass order them. If any pass moves to the async compute queue
  (`render/async/`), the range needs per-slot copies; tested by `t24_two_instances_two_poses`
  under validation's synchronization layer (`OHAO_VALIDATION=sync`).
- **BLAS refit quality.** A long refit chain degrades trace performance on limbs that swing far.
  The rebuild threshold (ruling 12) is set from `anim16_1080p`'s numbers.
- **Assimp's joint indices.** Bones are indexed file-wide by name, not by the skin's joint
  order. A remap by joint name at load is needed and tested (`t22_assimp_joint_remap`). A
  glTF whose vertices are bound to a skin by a different node naming is refused, not guessed.
- **Exactness of compute skinning.** A compiler may fuse the weighted sum differently than a CPU
  reference. The CPU comparison has a tolerance; run-to-run exactness (same SPIR-V) is what the
  goldens hold.
- **The tools/list cap.** 1 KB of headroom (ruling 13).
- **Content fit.** A CC0 pack may lack a rifle reload or an aim set. The fallback is our own
  Blender clips (ruling 1), and their quality is Frank's call in the hand test.
- **Gameplay drift.** Only the stated rulings (death clip, ADS spread) may change a simulation
  result. Every other combat and physics expectation stays.

## Review focus

- A transition and an event on the same step, and an event exactly at a clip's end on a looping
  clip: emitted once, in order (`t23_events_once_per_crossing`).
- A restore taken mid-fade: the fade resumes from the same step, the same bits
  (`t23_restore_mid_clip` fade case).
- A skinned instance created and destroyed in the same frame, and an enemy removed mid-fight:
  its arena range, previous-position slice and BLAS are retired, not leaked or read after free
  (`t24_two_instances_two_poses` destroy case).
- The arm swung outside every bind bound: drawn, cast into the cascades, hit by RT
  (`t24_cull_parity_skinned`, `t24_rt_sees_the_pose`).
- A placement mid-run (restart, teleport): no motion vector across the jump, the previous
  palette reset (`t24_motion_vectors_of_a_limb` placement case).

## Rulings for Frank

PROVISIONAL (recommended by the post-SP7 roadmap; Frank deferred them to this row; the spec is
designed to them):
1. **Content and licence:** CC0 humanoids (Quaternius- or Kenney-style) committed to the repo
   with licence files; not Mixamo (redistribution forbidden); not Somnabulate art in the MIT
   remote. Our own Blender clips where a needed one is missing, like the levels.
2. **Motion scope:** in-place animation; the character controller owns the root; no root
   motion in 10. Foot and aim IK later. Blend trees and animator state live in components and
   scene data (`Animator`, `.animgraph.json`).
3. **The skinning path:** compute skinning (it serves RT BLAS refits, meshlets and the path
   tracer), not vertex-shader skinning. The code confirms it: the path tracer and the meshlet
   path read vertices from the arena by mesh, so only a deformed copy in the arena reaches them.
4. **Determinism:** skinned poses replay bit for bit from the step count and snapshots like
   everything else.
5. **Order:** (a) skeleton, animator, glTF skins and clips, GPU compute skinning, BLAS refit,
   previous-pose motion vectors, culling; (b) the enemy's full body wired to AI and combat,
   events driving sounds; (c) the player's arms and gun replacing the procedural gun's mesh.
6. **Out of scope:** face, cinematics, a third-person player, ragdoll (13).

OPEN (judgment calls; recommendations given):
7. **(Death)** The death clip replaces `lieDown`'s rigid 90° tip; the saved `Transform` stays
   upright; tests 16/17 change their lying-down assertions to the death state. Recommended:
   the tip is a stand-in, and a clip-driven fall is what 13's ragdoll will blend from.
8. **The kick with a fire clip.** Keep SP9's procedural kick at half amplitude on top of the
   arms' fire clip (the clip carries the hands, the kick the whole view). Recommended: tune in
   the hand test.
9. **No retargeting in 10.** The enemies and the arms are separate rigs, each with its own
   clips. Recommended: retargeting is a tool for many characters, not two.
10. **`Fox.glb` and `BrainStem.glb`:** delete (forward only; unused, without licence files).
   Recommended over adding attribution for files nothing uses.
11. **ADS as gameplay:** `aim` held steps `WeaponState.aim` to 1 over `Weapon.adsTime` (0.2 s),
    halving spread at 1; the drawn field of view narrows to 55°. Recommended: ADS that does not
    tighten the cone reads as cosmetic. It is the one simulation change besides death.
12. **BLAS refit against rebuild:** refit every frame; rebuild after 120 refits or on a clip
    change. Recommended: measure on `anim16_1080p` and move the number by the trace cost.
13. **`tools/list` headroom:** if `animation_info` does not fit under 24 KB, trim descriptions
    first, then raise the cap to 28 KB. Recommended: the cap guards an agent's context, and
    one tool's worth is worth it.
14. **Clip storage:** keys as authored, binary search, no compression in 10. Recommended:
    compression and resampling belong to 15's cooked clips.
15. **Bounds of a skinned mesh:** per meshlet and per draw from the deformed vertices on the
    GPU (exact), not from per-joint boxes on the CPU (conservative). Recommended: exactness
    keeps culling parity simple, and the cost is one small pass.
16. **The skinning budget numbers** (table above) are first proposals, to be confirmed from
    Task 1's measurements before they gate.
17. **Footsteps:** physics' distance footsteps stay; animation events do not replace them in 10.
    Recommended: 11 syncs feet to sound with a mixer to schedule them.
18. **Hitboxes:** rounds keep hitting the capsule and body boxes, which no longer draw.
    Recommended: per-bone hitboxes come with 13's ragdoll bodies.

## As built (step 1)

Task 1 of the plan (2026-10-28): skeletons, clips, graphs, the step and the pose, GPU skinning on
the generated rig. Change record: `docs/changes/2026-10-28-animation-step1.md`. Where the build
differs from the text above, this section holds.

- **The reader.** `ohao/anim/gltf_anim` reads skins and animations itself (GLB chunk or base64
  data URI; normalised integer accessors; an external `.bin` and sparse accessors refused as
  `E_ASSET_UNSUPPORTED`). `readGltfStructure` (levels) is unchanged and still refuses skins and
  animations in a level: a level is static, and the node reorder it does for levels loses the glTF
  indices a skin needs.
- **The model loader** keeps a skinned file's vertices as written: it reads, then post-processes
  without `JoinIdenticalVertices` when any mesh has bones. Assimp's join compares position, normal
  and UVs, not joints or weights, and merged the rig's box-seam vertices (72 of 96 left), each then
  carrying both weight sets. Every existing golden is unchanged (no checked-in model has bones).
- **The palette is in the mesh's bone order** (Assimp's, `Model::boneNames`), mapped to the
  skeleton's joints by name: vertex data is never rewritten.
- **The pose is sampled at `(steps − 1 + alpha)` steps** (alpha 1: the newest step's pose); the fade
  weight is `(faded − 1 + alpha) / fade`. The "Frame" section's `steps + alpha` is off by one step.
- **`AnimLayerState.fade` and `faded` are `uint32_t`**, not `uint16_t`: the reflection codec has
  no 16-bit field kind. Values stay within 0..600 (the graph's limit).
- **`installAnimation(world, assets)`** is called by the hosts after `installCombat` (the CLI
  context, `LiveHost`, and the tools and tests that build their own sessions), not by
  `installCombat`: it needs the asset resolver for graphs and models.
- **The previous position lives in the skinned copy's `boneWeights.xyz`**, flagged by the draw flag
  `kDrawSkinned`, not in a separate stream with an offset in `InstanceGpu`: `InstanceGpu` (160 bytes)
  has no free field, and nothing reads a copy's joints (the skin pass reads them from the base
  mesh). No new binding, buffer or layout change; 12 bytes a vertex saved.
- **The previous palette is the previous sync's**, as `WorldTransform::prevWorld` is. A capture
  syncs once for the view and once a frame, and `world.step` syncs, so both start without motion;
  a window frame (steps, one sync, the frame) has it. Motion vectors of a skinned limb are tested on
  that path.
- **Bounds (OPEN ruling 15, partly not as recommended):** each meshlet copy's sphere is exact, from
  its deformed vertices on the GPU (`skin.bounds`, one thread a meshlet, fixed order, cone off). The
  draws' culling box is conservative, from each bone's bind-pose box through its palette matrix on
  the CPU (the pose system), not a GPU reduction: the draw table is uploaded from the CPU, and a
  conservative box keeps culling parity exact (test 8). Cost if wrong: a skinned draw culled a
  little later than it could be.
- **The passes run before `scene.upload`** (whose TLAS build reads the refit BLASes), in the pass
  registry as `skin`, `skin.bounds`, `rt.blas.refit`. They are recorded only for instances whose
  palettes changed; a still pose costs nothing. The TLAS refits whenever a BLAS was refit.
- **Refit and rebuild (OPEN ruling 12):** refit every changed frame, rebuild every
  `kBlasRefitsPerRebuild` = 120 refits; the clip-change rebuild is not built (the renderer does not
  know clips). anim16 measured the refit at 0.136 ms for 16 instances (budget 0.60).
- **`tools/list` (OPEN ruling 13):** 24,137 bytes of 24,576 with `animation_info` (36 tools): no
  trimming and no new cap for step 1; 439 bytes are left for Tasks 2–3.
- **`animation.info`** reports each layer's state, the clip timing it and its time (`t`), the fade
  (`from`, `blend`), and the clip events after `since`, numbered from 1 as `combat.events`.
- **Not built in step 1 (later tasks):** test 23's `t23_restart_resets` (needs `Round`, Task 2), the
  death clip (ruling 7) and ADS (ruling 11), the enemy and arm rigs, `JointAttach` on a real weapon
  (the component and its pose are built and unused), `ohao schema`'s `.animgraph.json` export,
  `animation_info` in `serve_bench`. `Fox.glb` and `BrainStem.glb` stay (ruling 10, Task 2).
- **Budgets (OPEN ruling 16)** gate at the proposals: anim16 skin + bounds 0.25 ms, refit 0.60 ms;
  `anim16_step_ms` 0.05 ms, `pose16_sync_ms` 0.10 ms (`animStep` 1). The play-scene budgets wait for
  Task 2's enemies.
