# Sub-project 12 — VFX and transparency: effects from the step, sorted forward transparency, projected decals, tracers, smoke in RT GI

Status: written 2026-10-28, after Frank's "can we move on?" once the grenade's spatial sampling
landed, as Day 1 of sub-project 12 (documents only; no code).
- **PROVISIONAL rulings.** Frank's brief set nine defaults (determinism, transparency, lighting,
  decals, tracers, smoke in GI, content, scope, order). They are written under "Rulings for Frank"
  as **PROVISIONAL** (recommended, not yet ruled), and the design follows them. Where the code
  showed a default needed a sharper choice, the ruling says which and why (ruling 1 picks the
  simulation's form from what the current GPU particles cannot do).
- **OPEN rulings.** Every other judgment call is listed there as **OPEN**, with a recommendation.
- The spec is binding once Frank accepts it.

Roadmap: row 12 of `docs/ROADMAP.md`. Done when:
- a muzzle, impact and smoke set is authored as scene data;
- a VFX budget exists and is gated;
- transparency goldens are exact.

Neighbours:
- **Before it:** 9 (Feel), 10 (Animation) and 11 (Audio). All three are built and wait for Frank's
  hand tests and rulings. Nothing here depends on those rulings.
- **After it:** 13, joints and ragdoll.

Builds on:
- **Sub-project 2b** (`2026-10-24-renderer-upgrade-design.md`):
  - the pass registry and per-pass GPU timing (`ohao/render/frame/pass_registry.hpp`), which
    `render_bench` gates;
  - clustered lighting (`LightClusters`, `ohao/render/deferred/light_clusters.{hpp,cpp}`; a
    `LightGpu` storage buffer per frame slot and a froxel list buffer, both public getters);
  - RT shadows and RT GI as compute ray queries (`shaders/rt/shadow_trace.slang`,
    `shaders/rt/gi_trace.slang`) with TLAS masks from `shaders/shared/layout.json`;
  - exact deferred goldens, `deferred_determinism_gpu_test`, `render.stats`, `render.settings`.
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): the API registry, MCP tools
  under the 24 KB `tools/list` cap, `world.snapshot`/`world.restore`, `input.record`/`input.replay`,
  lockstep captures, the window host.
- **Sub-project 6** (`2026-10-26-player-physics-design.md`): the fixed 60 Hz step, render
  interpolation (`RenderPose`, `World::setRenderAlpha`), snapshots through the World's
  `SnapshotHook`.
- **Sub-project 7** (`2026-10-27-combat-design.md`, "Presentation"): the combat event log (`shot`
  with the eye and direction, `hit` with point, normal, surface and distance, `death`), the muzzle
  flash `Light`, and the impact marks (`ohao/ecs/impact_marks.{hpp,cpp}`), whose deterministic
  placement this row keeps and whose drawing it replaces.
- **Sub-project 9** (`2026-10-27-feel-design.md`): the GPU particles in a window from combat events
  (`ohao/runtime/combat_fx.{hpp,cpp}`), the view model's drawn muzzle, and the `particles` budget
  (`render_bench play_fx_1080p`, 0.1 ms).
- **Sub-project 11** (`2026-10-28-audio-design.md`): the pattern this row copies:
  - a data file named by a saved singleton (`.audio.json` / `AudioMix`);
  - a simulate system that decides in the step and writes a log every host builds;
  - runtime state carried by the `SnapshotHook`, never saved or hashed;
  - the test grenade's `explode` event (ruling 23), the first hook for a boom.

## Thesis: what spawns is simulation; how it looks is a pure function of the step

Every effect starts from a **spawn record**, decided in the step from the step's events and saved
state. A record holds the effect, the step it was spawned in, a seed, and a pose. A muzzle flash,
an impact, a tracer, a decal, a grenade's boom and its smoke are each one record.

Records are written by a `vfx` simulate system that every host runs, headless ones included. Like
the voice schedule, they are runtime state:
- carried by snapshots;
- never saved or hashed;
- the same in a window and a headless host for the same steps.

What a record looks like at any moment is a **closed-form function** of:
- its record (effect, seed, pose);
- the effect's declared parameters (`.vfx.json`);
- its age, `(step − spawnStep − 1 + alpha) · dt`.

Each particle's position, size, colour and opacity come from counter-based random numbers,
`hash(seed, particle index, attribute)`, and the ballistic-with-drag formula. Nothing integrates
frame by frame, so there is nothing to drift, replay or snapshot beyond the records.
- **Lockstep frames and captures** draw at alpha 1, a function of the step count alone. Transparency
  goldens are therefore exact and a restore redraws the same frame.
- **A realtime window** draws the same function at the frame's alpha. That is presentation: smooth
  at 240 Hz, never seen by a golden.

The GPU evaluates the function (a compute pass expands records into particles), sorts what needs
sorting with a stable tie-break, lights it through the clustered lights, and draws it in one sorted
forward pass after the opaque frame. Decals are records too, projected into the G-buffer before
lighting. Smoke records also publish coarse density spheres that RT GI and RT shadow rays
attenuate through, so a cloud in a doorway dims the room's bounce light.

## Goal

On `ohao_game assets/scenes/blockout_play.scene.json` and in every host:
- **Muzzle:** each round has a muzzle effect (a short additive flash and a wisp of lit smoke) at the
  drawn muzzle, beside SP7's flash light.
- **Impacts:** each hit has an impact effect per surface (sparks and dust on concrete, a spark spray
  on metal, splinters on wood) and leaves a **projected decal** that wraps the surface it hit. The
  64 quad marks are gone.
- **Tracers:** a streak from muzzle to hit, timed from the step, shows where fire comes from.
  Every enemy round has one; the player's every third.
- **Grenade:** the test grenade's `explode` (audio ruling 23) has a boom (flash, debris, a lit
  smoke column) and a scorch decal.
- **Lit, soft, sorted smoke:**
  - it is lit by the same clustered lights as the room (the lamp tints it) with a cheap ambient term;
  - it fades softly where it meets geometry;
  - it sorts back to front with any other transparent surface.
- **Smoke in RT GI:** smoke left in the doorway visibly dims the bounce light inside the room, and
  sun shadows through it soften, measured as numbers.
- **Alpha materials:** glTF `MASK` materials cut out in the G-buffer and in RT. `BLEND` materials
  render in the sorted forward pass, so the Khronos alpha tests render as authored instead of
  opaque.
- **Data, not code:** all of it is authored as data. One `.vfx.json` per game (effects, decals,
  tracers, the event bindings, caps) is named by a saved singleton, and scene-placed emitters are
  saved components. No Blueprint, no visual scripting.
- **Agents** read the effects (`vfx.info`, `render.stats` `vfx`) and drive them through scene data
  and input as any edit.
- **Budgets:** the frame's VFX cost is gated at 1080p in a worst-case slice moment.
- **Goldens:** transparency, decal and particle goldens are exact.

## Scope: in 12 vs later

In 12:
- **Data:** the effect data file (`.vfx.json`) and its parser (every error at its JSON pointer,
  `ohao schema vfx-set`); the saved singleton `VfxSet { asset }` and the saved component
  `VfxEmitter` (a placed, rate- or burst-driven emitter).
- **The step:** the `vfx` simulate system. It writes spawn records from:
  - the combat log after the step's last `restart` (`shot`, `hit`, `explode`);
  - placed emitters by step count.

  The records carry deterministic caps (oldest dropped, counted) and ride the `SnapshotHook`.
- **Particles:** closed-form, evaluated on the GPU from records (`vfx.expand`). Per-particle
  lighting through the clustered lights plus ambient; soft particles by depth fade; flipbook
  textures; additive, alpha and premultiplied blending.
- **Transparency:**
  - one sorted forward pass (`transparent`) for particles, tracers and `BLEND` materials, back to
    front per object with a stable tie-break, per particle inside alpha-blended emitters;
  - `MASK` materials cut out in the G-buffer, the shadow and GI rays and the path tracer;
  - `MaterialDesc` alpha mode, cutoff and opacity, read from glTF and settable from
    `MaterialOverride`.
- **Decals:** projected box decals (the `decals` pass) writing G-buffer albedo, roughness and
  normal before lighting, in a capped ring. They replace the impact marks' quads, keep their
  placement data and follow what they hit.
- **Tracers:** stretched camera-facing quads from `shot` events, in the forward pass.
- **Smoke in RT GI and shadows:** analytic density spheres from smoke records, attenuating RT GI
  and RT shadow rays (OPEN ruling 18).
- **Content:**
  - a muzzle, impact (per surface), tracer, grenade-boom, smoke and decal set in
    `assets/vfx/slice.vfx.json`;
  - textures and flipbooks generated by `tools/vfx/` (ours, MIT) or CC0, each folder with its README
    and licence notes.
- **Agents and benches:** `vfx.info`; `render.stats` `vfx`; `render.settings` `vfx` switches;
  `render_bench play_vfx_1080p`; the step's cost in `physics_bench`.
- **Tests:** tests 31–35, look shots, and Frank's hand test.
- **Removed:** the current GPU particle system (`ohao/render/particles/*`,
  `shaders/particles/*`), `runtime/combat_fx.*` and the quad drawing in `ecs/impact_marks.cpp`.
  Their jobs move here (forward only).

Later:
- **Not in 12:**
  - fluid or smoke simulation (the smoke is a parametric cloud);
  - a volumetric fog system or its rewrite;
  - destruction;
  - a VFX editor UI;
  - particle collision with the world;
  - GPU-driven emitters with feedback;
  - refraction and distortion;
  - weighted-blended OIT (OPEN ruling 10).
- **13:** ragdolls; blood effects on a falling body follow its pose then.
- **15:** compressed (BC) textures and cooked flipbooks; textures load as PNG here.
- **16:** upscaling, which will need the transparency mask this row writes (OPEN ruling 15).
- **Untouched here:** the HDRI orientation (row 16), pacing (SP9 ruling 21), ROADMAP rows 9–11's
  status.

## Why (what the code does today)

- **Particles are presentation in a window only, and not replayable.**
  - `runtime/combat_fx.cpp` emits bursts from the combat log only while a window's timeline runs
    (`collect`, `realtime`; `live_host.cpp` 205–206).
  - Headless hosts and `ohao run` emit nothing. A lockstep capture drops the queue
    (`DeferredRenderer::setParticlesDrawn(false)`, `live_host.cpp` 484–488). No golden and no
    determinism test draws a particle.
- **The GPU simulation cannot be replayed:**
  - **Seed:** `hash(uint(time·1000) + idx·7919)` (`particle_emit.comp:65`), where `time` is a
    per-frame clock (`deferred_renderer.cpp:540, 548`).
  - **Time step:** a constant 0.016 s, because `DeferredRenderer::setDeltaTime` has no caller
    (`deferred_renderer.hpp:184`).
  - **Integration:** explicit Euler every frame (`particle_update.comp:63–70`).

  Its state lives in GPU buffers with atomic counters, so it cannot be snapshotted or restored.
- **The GPU particle path has defects:**
  - **Slots run out for good.** `particle_emit.comp:58` takes slots with `atomicAdd(aliveCount)`
    and nothing ever lowers it (`particle_update.comp:56` only counts the dead;
    `particle_system.cpp:120–126` sets the counters once). After 65,536 particles over a renderer's
    life, emits do nothing.
  - **Per-emitter parameters are ignored:** `update()` pushes gravity 9.81 and drag 0.1
    (`particle_system.cpp:512–513`), and no shader reads `colorEnd` or `endSize`.
  - **Every particle is additive** (`SRC_ALPHA, ONE`, `particle_system.cpp:358–367`), so smoke and
    dust glow instead of occluding. They are unlit, untextured (a procedural circle,
    `particle_render.frag:13–23`) and hard-edged against geometry (no depth fade).
  - **Projections disagree:** particles use the unjittered projection (`deferred_renderer.cpp:579`)
    against the jittered G-buffer depth, and write no velocity.
- **Place in the frame:** particles are drawn in their own pass straight after lighting and the sky,
  before SSS, SSR, bloom and TAA (`pass_registry.hpp:39`; `deferred_renderer.cpp:519–632`). SSR's
  resolve rewrites the lit image after them.
- **There is no transparency:**
  - `render::MaterialDesc` has no alpha mode, cutoff or opacity (`render_scene_types.hpp:54–71`),
    and `assets/khronos/README.md:36–37` notes that `BLEND` and `MASK` materials render opaque.
  - The G-buffer never discards.
  - Every BLAS geometry is `VK_GEOMETRY_OPAQUE_BIT_KHR` (`rt_acceleration_structure.cpp:245, 397`),
    and every trace forces opaque (`gl_RayFlagsOpaqueEXT`; `RAY_FLAG_FORCE_OPAQUE` in
    `gi_trace.slang:125` and `shadow_trace.slang:152`). So the path tracer's alpha-testing any-hit
    shader (`pt_anyhit.rahit:52–58`) never runs.
  - AGENTS.md's gotcha about `MaterialData::AlphaMode::OPAQUE` names an enum that does not exist;
    Task 2 corrects it.
- **Impact marks are opaque quads.**
  - There are 64 runtime `RenderScene` instances: a unit quad 2 mm off the surface, 8 cm, dark
    albedo (`impact_marks.cpp:47–54, 229–242`).
  - They float off curved or cornered surfaces, z-fight at grazing angles, show no surface relief,
    and have one look for every surface.
  - They sit in the TLAS as scene geometry, so RT shadows and GI see 8 cm black cards.
  - Their placement is right and deterministic (the log read from its oldest event, attached by
    pose, restart clears). This row keeps it.
- **The grenade's boom is silent to the eye.** `explode` has a sound and no visual
  (`combat_fx.cpp:81` emits nothing for it), and the grenade in flight is not drawn.
- **Nothing in RT sees media.** No volume, density or extinction exists in `shaders/rt` or `ohao/`.
- **The light loop is inline.** The froxel lookup and the light loop live in
  `shaders/core/deferred_lighting.frag:264–314` (GLSL). A forward pass needs the same loop and
  cannot include it.

## Data: the VFX set (`.vfx.json`)

One file per game, named by the saved singleton `VfxSet { AssetRef asset; }`. The play scene names
`assets/vfx/slice.vfx.json`. Without one, a built-in default set equal to today's presets (muzzle,
spark, dust per surface) so a scene without data still shows combat effects. It is parsed by
`ohao/vfx/vfx_set.{hpp,cpp}` (simdjson, as `.audio.json` and `.animgraph.json`), every error at its
JSON pointer; `ohao schema vfx-set` exports its schema.

```json
{
  "textures": {
    "smoke":   {"file": "vfx/smoke_8x8.png",  "cols": 8, "rows": 8},
    "flash":   {"file": "vfx/flash_4x1.png",  "cols": 4, "rows": 1},
    "spark":   {"file": "vfx/spark.png"},
    "bullet":  {"file": "vfx/decal_bullet.png", "normal": "vfx/decal_bullet_n.png"},
    "scorch":  {"file": "vfx/decal_scorch.png"}
  },
  "effects": {
    "muzzle": {"emitters": [
      {"count": 4, "life": [0.03, 0.05], "speed": [0.5, 2.0], "spread": 10, "size": [0.06, 0.12],
       "color": ["#ffd9a0", "#ff9040"], "intensity": 12, "blend": "additive", "lit": false,
       "texture": "flash", "frames": "random"},
      {"count": 3, "life": [0.4, 0.8], "speed": [0.3, 0.8], "spread": 25, "size": [0.04, 0.18],
       "color": ["#b8b4ae", "#b8b4ae"], "alpha": [0.35, 0.0], "drag": 4, "gravity": -0.3,
       "blend": "alpha", "lit": true, "soft": 0.05, "texture": "smoke", "frames": "life"}]},
    "impact_concrete": {"emitters": ["..."]},
    "impact_metal":    {"emitters": ["..."]},
    "impact_wood":     {"emitters": ["..."]},
    "impact_default":  {"emitters": ["..."]},
    "grenade_boom":    {"emitters": ["..."], "light": {"color": "#ffb060", "intensity": 40, "range": 8, "time": 0.12}},
    "smoke_cloud":     {"emitters": ["..."],
                        "proxy": {"spheres": 3, "radius": [0.6, 2.4], "density": 0.9, "rise": 0.25, "life": 9.0}}
  },
  "decals": {
    "bullet_concrete": {"texture": "bullet", "size": 0.10, "depth": 0.06, "albedo": "#2b2926", "roughness": 0.95, "normal": 0.8},
    "bullet_metal":    {"texture": "bullet", "size": 0.07, "depth": 0.04, "albedo": "#3a3a3c", "roughness": 0.40, "normal": 0.6},
    "bullet_wood":     {"texture": "bullet", "size": 0.09, "depth": 0.05, "albedo": "#2a1d12", "roughness": 0.90, "normal": 0.8},
    "bullet_default":  {"texture": "bullet", "size": 0.08, "depth": 0.05, "albedo": "#262626", "roughness": 0.95, "normal": 0.7},
    "scorch":          {"texture": "scorch", "size": 1.6, "depth": 0.6, "albedo": "#151311", "roughness": 0.98, "normal": 0.0}
  },
  "tracers": {
    "rifle": {"speed": 420, "length": 1.8, "width": 0.012, "color": "#ffd090", "intensity": 10,
              "everyPlayer": 3, "everyEnemy": 1, "minDistance": 4}
  },
  "events": {
    "shot":    {"effect": "muzzle", "tracer": "rifle"},
    "hit":     {"effect": "impact_{surface}", "decal": "bullet_{surface}", "fallback": "default"},
    "explode": {"effect": ["grenade_boom", "smoke_cloud"], "decal": "scorch"}
  },
  "caps": {"records": 256, "particles": 16384, "decals": 256, "tracers": 64, "proxies": 32}
}
```

- **An emitter** declares:
  - `count` (a burst) or `rate` (per second, for a placed emitter);
  - ranges (`life`, `speed`, `size` start and end, `spread` in degrees around the spawn direction);
  - `gravity` (m/s² along −Y, signed) and `drag` (1/s);
  - colour and alpha over life (two keys, or up to four as `[t, value]`), and `intensity` (HDR
    multiplier for additive light);
  - `blend` (`additive`, `alpha`, `premultiplied`), `lit`, `soft` (the depth-fade distance in m);
  - `texture` and `frames` (`life`, `random` or a fixed index); `sort` (`auto`, `none`, `particles`).
- **An effect** is a list of emitters, with optional:
  - `light`: a transient point light record through the clustered lights, the flash's glow;
  - `proxy`: density spheres for RT GI and shadows (smoke only).
- **Decals** declare:
  - the projected box (`size` across and `depth` along the normal);
  - albedo tint, roughness and normal strength;
  - `life` (0: until the ring reuses it; else a fade over the last 10%).
- **Tracers** declare speed, length, width, colour and HDR intensity, which shots make one
  (`everyPlayer`, `everyEnemy`: the nth round of a weapon's `shots` counter), and `minDistance` (no
  tracer for a hit nearer than this).
- **Events** bind combat events to effects. `{surface}` expands from the hit's `Collider.surface`,
  else `fallback`. The binding table is the only place an event becomes an effect; code never names
  an effect.
- **Caps** are deterministic. Over a cap, the oldest record of that kind is dropped and counted
  (`dropped` in `vfx.info`). That is never a cost spike or a random choice.

Scene-placed emitters: `VfxEmitter { std::string effect; float rate; uint32_t seed; bool active; }`
(saved). Placed on an entity, it spawns its effect at the entity's pose:
- `rate` records a second, by step count (`floor(steps · rate · dt)` crossings), from the step it
  became active;
- or once when `rate` is 0 and it becomes active.

The smoke-in-doorway test scene and the GI measurement use one.

## The step: spawn records (`ecs/vfx.{hpp,cpp}`)

`installVfx(world, assets)` (the CLI context and `LiveHost` call it after `installAudio`) adds the
`vfx` simulate system, after `combat`, `impact_marks` and the animation step, before
`audio_schedule`. Each step it:
1. **Loads the set:** reads `VfxSet` (loading the file when it changes, as the schedule loads its
   bank).
2. **Reads the combat log after the step's last `restart`.** A restart (or any restore) clears
   every record and writes nothing from the abandoned timeline.
   - `shot`: the bound effect at the muzzle. The muzzle is the weapon's `Muzzle` child's world pose
     at the step: the logical, un-swayed pose, the same in every host.
   - A tracer record from the muzzle along the round (to the hit point, or 60 m on a miss) when the
     shot's ordinal matches `everyPlayer`/`everyEnemy` and the distance is at least `minDistance`.
   - `hit`: the impact effect at the point along the normal, and a decal record.
   - `explode`: the boom and the smoke at the point, and the scorch decal projected straight down.
3. **Steps placed emitters:** each active `VfxEmitter` at its rate, by step count.
4. **Drops what is due:** records past their effect's longest life, and the oldest over a cap
   (counted).
5. **Keeps decal placement:** decal records keep `impact_marks`' placement rules unchanged.
   - A hit on a character re-casts against its body boxes and attaches to the one crossed.
   - Nothing is placed on the input-driven player.
   - The record holds the attached entity and the local pose, and follows it by pose at the step.
   - `ecs/impact_marks` keeps `observe` and `follow` and stops drawing. Its 64 slots become the
     decal ring (OPEN ruling 12 sets the size).

A **record** is plain data:

```cpp
struct VfxRecord {
    enum class Kind : uint8_t { Effect, Tracer, Decal, Light };
    Kind kind;
    uint16_t effect;          // index into the set's effects / tracers / decals
    uint32_t seed;            // hash(world seed, event seq, kind)
    uint64_t spawnStep;
    uint64_t sourceSeq;       // the combat event's seq (0: a placed emitter)
    entt::entity on;          // decals: what it is attached to (null: the world)
    glm::mat4 local;          // pose: world, or local to `on`
    glm::vec3 a, b;           // tracers: muzzle and end; effects: unused
};
```

- **Runtime state:** the records, the ring cursor and the counters live in the registry's context.
  They are snapshotted with the World through the `SnapshotHook` (chained like the audio schedule's)
  and never saved or hashed. `world.hash` after a fight is the same with and without VFX.
- **The seed** is a function of the event's `seq` and kind. The same fight gives the same seeds,
  and a restore gives back the records it had.
- **Placement is logical:** the ECS system sources include no `render/`, `reflect/` or `api/`
  header. The muzzle pose, hit points and normals are the step's, so window and headless hosts
  write the same records.

## The frame: expansion, sorting, the forward pass (`render/vfx/`)

A `vfx` sync system after `render_extract` (installed where a `RenderScene` exists, like
`installImpactMarks`) uploads the live records to a per-frame-slot buffer. `RenderScene` then runs:

- **`decals`** (after `gbuffer.late`, before `lights.cluster`):
  - one instanced unit-box draw per visible decal record, depth tested against the G-buffer, back
    faces only (so a camera inside a box still draws it);
  - it reconstructs each pixel's position, projects it into the decal's box, and rejects pixels
    whose G-buffer normal faces away from the decal's normal by more than 60°;
  - it blends albedo and roughness by the texture's alpha, and the normal by the decal's normal map
    rotated into the surface's frame (OPEN ruling 16);
  - it never touches emissive or the material row's other channels.

  The `kDrawEmitter` and view-model pixels are masked out (their G-buffer metallic −1 marks, and
  the view model's flag). A decal never lands on the gun.
- **`vfx.expand`** (compute, after `lighting`): one thread per particle slot of each record.
  - **Evaluate:** age, the closed-form position, size, colour, alpha and flipbook frame
    ("Determinism").
  - **Light:** for `lit` emitters, the per-particle light at its centre:
    - the clustered lights, through the froxel at its view depth;
    - the sun, through one RT shadow ray query against `kRayMaskScene` plus the smoke spheres;
    - ambient from RT GI's half-resolution radiance at its screen position, else the environment's
      irradiance (OPEN ruling 13).
  - **Emit:** an instance (position, size, colour, frame, sort key) into a compacted list in record
    order. Tracers expand to one stretched quad, each with its head at `speed · age` along the
    round, clamped to the end.
  - **Order:** records stay in id order, and particles in index order within a record. The compaction
    is a deterministic prefix sum, as `GpuCulling`'s.
- **`vfx.sort`** (compute):
  - **Sort the objects:** transparent objects (alpha emitters, tracers, `BLEND` instances) go back
    to front by view depth of their bounding centre, ties broken by object id.
  - **Sort within an emitter:** particles are sorted only in alpha-blended emitters of up to 1,024
    particles (a bitonic sort in shared memory, key = depth then index). Additive emitters are
    order-independent and stay unsorted (OPEN ruling 11).
- **`transparent`** (after `ssr`, before `post.bloom`; it replaces the `particles` pass):
  - **Target:** the lit HDR image (`R16G16B16A16_SFLOAT`) with the G-buffer's `D32_SFLOAT` read
    only, depth tested, no depth write.
  - **Draw order:** the sorted list. Each object uses its own blend state, so additive particles,
    alpha particles, premultiplied particles, tracers and `BLEND` meshes interleave in one
    back-to-front order.
  - **Particles:** camera-facing quads with the jittered projection (the G-buffer's), texture ×
    per-particle light, soft fade `saturate((sceneDepth − particleDepth) / soft)`.
  - **`BLEND` meshes:** lit per pixel by the shared clustered-light module (the forward copy of the
    deferred loop, OPEN ruling 19), the sun's shadow by one ray query per pixel, environment
    specular from the prefiltered environment.
  - **Transparency mask:** a coverage value goes to a small mask image (`R8`) that TAA reads to
    lower history weight under transparents (OPEN ruling 15).
- **Smoke proxies:** live smoke records publish up to `caps.proxies` spheres (centre, radius and
  density as closed-form functions of age) to a small storage buffer, uploaded with the records.
  `shadow_trace.slang` and `gi_trace.slang` loop over them for each ray. For each sphere a ray
  crosses, `T *= exp(−density · chord)`. Shadow visibility and GI radiance are multiplied by `T`,
  and a GI ray ending inside a cloud adds the cloud's ambient in-scatter scaled by `1 − T`
  (OPEN ruling 18).

Pass order after this row:

`skin … gbuffer.late, decals, lights.cluster, ssao, rt.shadows(+filter), rt.gi(+denoise), lighting, sky, vfx.expand, vfx.sort, sss, ssr, transparent, post.bloom, post.taa, post.tonemap, gizmo, hud, …`

Moving the effects after SSS and SSR is deliberate (OPEN ruling 24). SSR's resolve rewrites the
lit image in place from opaque data, so anything drawn before it is reflected as if opaque, or
scrubbed. Transparent surfaces draw over the resolved frame.

## Materials: alpha modes

- **Material fields:** `render::MaterialDesc` gains `alphaMode` (`Opaque`, `Mask`, `Blend`),
  `alphaCutoff` (0.5) and `opacity` (1). The packed material row carries them in its spare floats;
  the row stays 12 floats if they fit, else 16, recorded in `layout.json`.
- **Loading:** the model loader reads glTF's `alphaMode`, `alphaCutoff` and base-colour alpha
  through Assimp (`AI_MATKEY_GLTF_ALPHAMODE`, `AI_MATKEY_GLTF_ALPHACUTOFF`).
- **Scene authoring:** `MaterialOverride` gains `alphaMode` and `opacity`. That is a saved-type
  change, so it moves `world.hash` expectations once, with that reason.
- **`Mask`:**
  - **G-buffer:** a discard below the cutoff, through a separate pipeline variant so opaque draws
    keep early depth.
  - **Culling:** its draws join the double-sided lists by their material's sidedness as today.
  - **BLAS:** the geometry is built without `OPAQUE_BIT`.
  - **Rays:** shadow and GI ray queries run `CANDIDATE_NON_OPAQUE_TRIANGLE` handling that
    alpha-tests the same texel. Their `FORCE_OPAQUE` flag stays for every geometry that is opaque
    (a per-geometry flag, not a global switch).
  - **Path tracer:** the any-hit shader finally runs for these geometries.
- **`Blend`:**
  - **Raster:** never in the G-buffer, Hi-Z or CSM lists (a draw flag, `kDrawTransparent`).
  - **Forward:** each instance is one object in the forward pass.
  - **RT masks:** its TLAS instances get a new mask bit, `kTlasMaskTransparent` (8). It is in no RT
    shadow or GI ray mask (a transparent pane casts no shadow and bounces no light in the hybrid
    frame; OPEN ruling 14) and in the path tracer's camera mask.
  - **Path tracer:** stochastic opacity in the any-hit shader (`opacity · baseColor.a` against the
    pixel's sample), the path tracer's usual treatment.

## The agent-facing surface

- **`vfx.info {since?, max?}`** returns:
  - the set's name and caps;
  - counts: records live by kind, particles live (from the last frame, `null` headless), decals,
    tracers, proxies, and dropped by kind;
  - the records after a cursor, each with `kind`, `effect`, `step`, `pos`, `source` (event seq) and
    `on` for decals.

  At most 1 KB at `max` 16, measured as `serve_bench vfx_info_bytes`. Headless hosts answer the
  step half; a window adds the frame half.
- **`render.stats` `vfx`** (a window or a capture): `particles`, `objectsSorted`, `decalsDrawn`,
  `tracers`, `proxies`, `particlesSortedMax`, and the passes' timings as every pass's.
- **`render.settings` `vfx`:** `{particles, decals, tracers, soft, smokeInGi}` booleans, all true by
  default. They are presentation switches for A/B captures, never in the World. Test 34 compares
  `smokeInGi` on and off.
- **Driving effects:** no new mutating tool.
  - A test places a `VfxEmitter` by `world.apply` (`entity.create`, `component.set`) or fires
    through `input.inject`. Every effect then comes from the step, so `input.record`/`input.replay`
    and snapshots reproduce it.
  - `combat.events` already reports the events the bindings map.
- **The 24 KB cap:** `tools/list` is at 24,452 of 24,576 bytes (SP11 step 4). `vfx_info` and the
  settings schema's growth need about 700 bytes (OPEN ruling 20).

## Components and hints

- **`VfxSet { AssetRef asset; }`:** saved singleton, the effect data.
- **`VfxEmitter { std::string effect; float rate; uint32_t seed; bool active; }`:** saved; a placed
  effect at its entity's pose.
- **`MaterialOverride`** gains `alphaMode` (`opaque`, `mask`, `blend`) and `opacity` (saved).
- **Level hint `vfx`:** a node's `extras.vfx = "smoke_cloud"` makes a `VfxEmitter` on it
  (`level_hints.hpp`, `tests/level/extras_schema.json`), as `verb` makes a `ReverbZone`.

The three saved types, `VfxSet`, `VfxEmitter` and `MaterialOverride`'s new fields, move the serve
and CLI `world.hash` expectations once (saved types hash their names even with no instances). Those
are reviewed line by line, with that reason.

## Performance budgets

The display targets are 240 Hz (4.17 ms a frame, this laptop) and 144 Hz (6.94 ms, Frank's). The
play scene's frame is 2.19 ms at 1080p (`play_hud_1080p`). VFX may take at most 0.6 ms of it in the
worst slice moment, keeping the frame under 3 ms at 240 Hz.

New `render_bench` scene `play_vfx_1080p`: the play scene from the spawn, stepped before every frame
through a scripted moment:
- a grenade's boom and its smoke in the doorway;
- both enemies and the player firing (muzzles, tracers, impacts);
- 64 decals in view.

Every frame changes. Budgets are first proposals, confirmed from Tasks 1–3's measurements (OPEN
ruling 12):

| Measure | Bench | Budget | Gated from |
|---|---|---|---|
| `vfx.expand` (≤ 8,192 particles live, lit with one shadow ray each) | `render_bench play_vfx_1080p` | ≤ 0.08 ms | Task 2 |
| `vfx.sort` (≤ 64 objects, ≤ 4 alpha emitters of ≤ 1,024) | the same | ≤ 0.04 ms | Task 2 |
| `transparent` (the forward pass: smoke filling ≈ 30% of the screen, tracers, muzzles) | the same | ≤ 0.30 ms | Task 2 |
| `decals` (64 in view, 256 live) | the same | ≤ 0.05 ms | Task 3 |
| Smoke proxies in RT (`rt.shadows` + `rt.gi` with 16 spheres, against without) | the same, `render.settings vfx.smokeInGi` off | ≤ +0.06 ms | Task 4 |
| The whole VFX share (the five above) | the same | ≤ 0.60 ms | Task 4 |
| `play_vfx_1080p` frame | the same | ≤ 2.19 + 0.60 ms | Task 4 |
| The `vfx` simulate system, the play scene mid-fight | `physics_bench vfx_step_ms` | ≤ 0.02 ms a step | Task 1 |
| The `vfx` sync system (records → buffer) | `physics_bench vfx_sync_ms` | ≤ 0.03 ms a frame | Task 2 |
| `vfx.info` result, max 16 | `serve_bench vfx_info_bytes` | ≤ 1 KB | Task 4 |
| No regression: `play_hud_1080p` frame, `cornell_720p` passes, `look_latency_ms` | as today | unchanged budgets | every task |

Caps (the set's `caps`, proposals):

| Kind | Cap |
|---|---|
| Records | 256 |
| Particles live | 16,384 (the expansion buffer; today's 65,536 slots shrink) |
| Decals | 256 |
| Tracers | 64 |
| Smoke spheres | 32 |
| Particles sorted per emitter | 1,024 |

Exceeding a cap drops the oldest record of its kind, counted; it is never a cost spike. Benches run
with `scene_bench`'s calibration and noise rules. A noisy run is neither a pass nor a fail.

## Determinism

- **Records are a function of the step.** They depend only on:
  - the saved world and the step's combat log;
  - the step count and placed emitters;
  - the set's declared values.

  Two runs of the same fight, a restore mid-fight, a round restart and an `input.replay` give the
  same records. Window and headless hosts give the same records for the same steps.
- **What is drawn is a function of the records, the camera and alpha.**
  - Every particle's attributes are closed-form in its age, `t = (step − spawnStep − 1 + alpha) · dt`,
    with counter-based randomness `pcg(hash(seed, index, attribute))`. There is no integration,
    feedback or atomic slot allocation, and no wall-clock time, frame counter or frame-in-flight
    slot.
  - Lockstep frames and every capture use alpha 1. A capture of a world at step N is the same image
    in any host, after any history of frames.
- **The GPU evaluation is exact run to run:** the same shader on the same driver, in a fixed order
  (records in id order, particles in index order, a deterministic prefix sum, a sort with an index
  tie-break, fixed blend order). This is the same guarantee the deferred goldens already rely on.
  `deferred_determinism_gpu_test` gains the VFX scenes.
- **The closed form** for position, with drag `k` and gravity `g`:
  - `x(t) = x0 + v0·(1 − e^(−kt))/k + g·(t/k − (1 − e^(−kt))/k²)`;
  - `x(t) = x0 + v0·t + g·t²/2` when `k = 0`.

  The CPU reference in `ohao/vfx/particle_math.hpp` is the same expression, used by the tests and
  `vfx.info` (positions match the GPU within 1e-4 m; they are never compared bit for bit).
- **Presentation-only extras may not reach a golden:**
  - **Muzzle effects and tracers start at the drawn muzzle in a live window,** the view model's
    `RenderPose` with sway and kick, and at the logical muzzle in lockstep and captures, where the
    view model is at rest anyway.
  - **Interpolation:** the frame's alpha in realtime.

  Neither is ever read by the step.
- **Hashes:** `VfxSet`, `VfxEmitter` and `MaterialOverride`'s fields move the `world.hash`
  expectations once (above).
- **Goldens:**
  - **New goldens.** The particle, transparency, decal and smoke goldens are exact (max_abs 0) in
    deferred and within the BVH floor in the path tracer.
  - **Existing goldens move only for named reasons:**
    - `cli_blockout_play_hud` (45 steps of fight): its marks become decals, and its muzzle, impact
      and tracer effects appear;
    - the Khronos alpha scenes (their `MASK`/`BLEND` materials stop rendering opaque);
    - any golden whose light loop moves, which is none if the deferred shader is untouched
      (OPEN ruling 19).

    Each moved golden is looked at before `--update`.

## Test plan

Tests are numbered after SP11's (test 30, the grenade's pan):
- 31: data and the step;
- 32: transparency and lit particles;
- 33: decals and tracers;
- 34: smoke in RT GI;
- 35: agents, budgets and the slice.

**31 — data and the step** (`vfx_tests`, a new CPU suite, `quick`):
- **`t31_set_parse`:** `.vfx.json` errors, each at its JSON pointer:
  - an unknown texture, effect or decal;
  - a range with min > max;
  - a `{surface}` binding without a fallback;
  - a cap of 0;
  - an unknown blend.
- **`t31_default_set`:** without `VfxSet`, the built-in set equals today's presets.
- **`t31_closed_form`:** the CPU reference against a 1 ms Euler integration of the same parameters
  stays within 1 mm over 2 s, for drag 0 and drag 4.
- **`t31_records_from_events`:** a scripted fight in the play scene.
  - Every `shot` writes one muzzle effect at the logical muzzle, and every third player round and
    every enemy round a tracer to the hit point.
  - Every `hit` writes the impact effect and decal of its surface, `metal` as metal and `default`
    as default.
  - `explode` writes the boom, the smoke and the scorch.
  - All in the event's step, with seeds from `seq`.
- **`t31_placed_emitter`:** a `VfxEmitter` at rate 10 writes exactly 10 records in 60 steps, and
  rate 0 writes one when activated.
- **`t31_caps`:** 300 records against a cap of 256 drop the oldest 44, counted, and the same ones
  every run.
- **`t31_replay`:** the same fight twice gives the same records. A restore mid-fight and an
  `input.replay` give what the recording had. A round restart clears the records and writes those
  of the round's first steps again.
- **`t31_hash_unchanged`:** `world.hash` after a fight is the same with and without `installVfx`.
- **`t31_window_equals_headless`** (`check_live.py`): a window's records (`vfx.info`) equal a
  headless session's for the same fight.

**32 — transparency and lit particles** (`vfx_gpu_test`, `gpu`, and goldens):
- **`t32_sort_order`:** three overlapping alpha panes and two alpha emitters render back to front
  from four views. Two objects at equal depth draw in id order (read back by a test-only draw ID).
- **`t32_transparency_golden`:** `assets/scenes/transparency.scene.json` in deferred, with exact
  goldens. It holds:
  - two overlapping `BLEND` panes over the Cornell box;
  - a `MASK` foliage card;
  - an alpha smoke emitter crossing a wall (soft edge) at a fixed step;
  - additive sparks.

  The same scene is path traced, within the BVH floor.
- **`t32_khronos_alpha`:** the Khronos `AlphaBlendModeTest` renders `MASK` cut and `BLEND`
  blended. The golden moves once and is looked at.
- **`t32_lit_particles`:** a grey smoke puff 0.5 m from the room's red lamp is redder than the same
  puff 5 m away, by the clustered light's `I/d²` (per-particle light read back). With `lit: false`
  it is unchanged.
- **`t32_soft`:** a puff crossing a wall fades linearly over `soft`. Measured along a scanline, no
  pixel steps by more than 1/8 of the puff's alpha.
- **`t32_mask_rays`:** an RT shadow through a `MASK` card's holes lights the floor behind them, and
  the card's opaque texels shadow it. The same holds in RT GI and the path tracer.
- **`t32_blend_no_shadow`:** a `BLEND` pane casts no RT shadow and bounces no GI in the hybrid frame.
- **`t32_determinism`:** `deferred_determinism_gpu_test` gains the transparency scene and the play
  scene at step 45 with VFX. Two renderers, a child process and repeated captures are bit
  identical, from every frame slot.
- **`t32_capture_any_history`:** a capture at step N after 0, 7 and 200 realtime frames is
  identical, because effects are a function of the step.

**33 — decals and tracers** (`vfx_gpu_test`, `combat_tests`, goldens):
- **`t33_decal_footprint`:** a capture with a decal against one without differs only inside the
  decal box's screen footprint, as the HUD and emitter tests check.
- **`t33_decal_wraps`:** a decal over the blockout's step edge darkens both faces. A decal never
  lands on a face turned more than 60° from its normal, nor on the view model.
- **`t33_decal_ring`:** 300 hits keep 256 decals, the oldest reused, the same ones every run.
- **`t33_decal_follows`:** a decal on an enemy's body box moves with the box. A restart clears every
  decal.
- **`t33_no_decal_on_player`:** a round into the player marks nothing.
- **`t33_marks_gone`:** no `impact_mark` mesh or instance exists, and the TLAS holds no decal.
- **`t33_tracer_timing`:** a tracer's head is at `speed · age` along the round, clamped at the hit,
  gone after `length / speed` past the hit. It is in the frame at step + 1 and gone by its last
  step, measured in captures.
- **`t33_decal_golden`:** the blockout after a scripted burst at the wall (concrete), the step's
  edge and a crate (wood), with exact deferred goldens.

**34 — smoke in RT GI** (`vfx_gpu_test`, measured numbers):
- **`t34_doorway_dims_gi`:** in `assets/scenes/smoke_doorway.scene.json`, a `VfxEmitter`
  `smoke_cloud` fills the blockout's doorway (outdoor sun outside, the room lit only by bounce).
  - Measure the mean of RT GI's output over a floor patch 1–2 m inside the room, 30 converged
    frames, `smokeInGi` on against off.
  - With the cloud the patch is at least 20% darker. With the cloud gone (its life past), it
    returns to within 1% of the cloudless value.
  - The numbers are printed and written to `build/vfx/t34.json`.
- **`t34_shadow_through_smoke`:** sun visibility behind a cloud of density 0.9 and chord 2 m is
  `e^(−1.8)` ± 2% (the shadow trace's transmittance read back).
- **`t34_proxy_cost`:** 32 spheres add at most 0.06 ms to `rt.shadows` + `rt.gi` at 1080p (the
  budget).

**35 — agents, budgets and the slice** (`api_tests`, `serve`, benches):
- **`vfx.info`:** headless (step half, `particles: null`), after a cursor, ≤ 1 KB at max 16, refused
  params.
- **`render.stats` `vfx` and `render.settings` `vfx`:** the switches drop their passes, and they
  never change `world.hash` or records.
- **`tools/list` ≤ 24 KB**, with every tool's schema through `jsonschema`.
- **`check_live`:** a window's lockstep capture after a fight equals a headless capture bit for bit,
  now with effects in both. That replaces SP9's "a lockstep capture draws no live particle".
- **`e2e_mcp`:** a leg that throws a grenade, steps, reads `vfx.info`, captures, restores and
  captures again to the same bytes.
- **Benches:** `render_bench play_vfx_1080p` (every row of the budget table); `physics_bench
  vfx_step_ms` and `vfx_sync_ms`; `serve_bench vfx_info_bytes`.
- **Validation:** `check_validation.py` gains the VFX scenes in raster and RT.

**Look shots** (the visual form of the listening artifacts): every task writes stills and a short
frame sequence of its effects to `C:\Users\djmax\ohao_refs\look\sp12_vfx\` (local, never committed):
- muzzle;
- impacts per surface;
- decals at grazing angles;
- tracers;
- boom and smoke;
- the doorway with and without smoke in GI.

## Hand-test acceptance (from Frank)

Empty until Frank plays `build/Release/ohao_game.exe assets/scenes/blockout_play.scene.json` at his
display's rate. Each item is closed by his note, a fix with its test, or a ruling that it stays.

| # | Item | What to try | Frank's note | Status |
|---|---|---|---|---|
| V1 | Muzzle | Fire single rounds and full auto, hip and ADS: the flash and wisp read as a gun, not a sparkler; no flash smeared by TAA | | open |
| V2 | Impacts | Shoot concrete, metal, wood and the crates: each surface's spray reads differently; dust is grey and occludes, sparks glow | | open |
| V3 | Decals | Shoot the step's edge, a corner and the floor at a grazing angle: marks wrap, never float or z-fight, and keep relief under the lamp | | open |
| V4 | Tracers | Let the enemies fire from cover: you can tell where fire comes from without being hit; your own tracers do not blind you | | open |
| V5 | Grenade | Throw G into the room and outside: the boom flashes and lights the walls briefly; the smoke rises, is lit by the lamp, and softens against walls | | open |
| V6 | Smoke in GI | Leave smoke in the doorway and look into the room: the bounce light dims, then returns as it clears | | open |
| V7 | Sorting | Walk through the smoke with tracers and muzzle flashes behind and in front of it: nothing pops, flickers or draws in the wrong order | | open |
| V8 | Cost | Empty a magazine into a wall with a boom going off: no hitch; `--latency` shows frame time within budget | | open |

## Done criteria

- **The set as data:** the slice's muzzle, impact (per surface), tracer, grenade boom, smoke and
  decal set is authored in `assets/vfx/slice.vfx.json`, named by the play scene's `VfxSet`. Its
  textures are generated (or CC0) with READMEs and licence notes.
- **Every host:** effects come from spawn records the step writes, the same in every host, through
  restores, restarts and replays (test 31). The quad marks are gone (test 33).
- **Transparency:** one sorted forward pass draws lit, soft particles, tracers and `BLEND`
  materials. `MASK` cuts in raster and RT.
- **Goldens:** transparency, particle and decal goldens are exact (tests 32, 33).
- **Smoke in GI:** smoke dims RT GI by the measured amount (test 34).
- **Budgets:** every VFX budget is gated and met on a quiet run (test 35).
- **Hand test:** V1–V8 are open for Frank.

## Build order

Four tasks, in PROVISIONAL ruling 9's order. Each ends with the engine running and every gate
green, with a change record `docs/changes/YYYY-MM-DD-vfx-stepN.md`:
1. **(a) The data and the step.**
   - The set format and parser, `VfxSet` and `VfxEmitter`, and the `vfx` simulate system (records,
     caps, snapshot hook, seeds).
   - The CPU closed form, and `vfx.info`'s step half (registered with room made in `tools/list`).
   - Test 31, and `physics_bench vfx_step_ms`.
   - Nothing drawn changes yet: the old particles keep drawing in windows.
2. **(b) Sorted forward transparency and lit, soft particles.**
   - The alpha modes (material, loader, G-buffer `MASK`, RT any-hit, `BLEND` to forward).
   - `vfx.expand`, `vfx.sort` and the `transparent` pass in place of `particles`, the clustered-light
     module for the forward path, soft particles, flipbooks, and the TAA transparency mask.
   - The muzzle, impact and smoke effects drawn from records in every host.
   - The old particle system and `combat_fx` deleted.
   - Test 32, its goldens, and the `vfx.expand`/`vfx.sort`/`transparent` budgets.
3. **(c) Decals, tracers and the content set.**
   - The `decals` pass. `impact_marks` keeps placement and stops drawing, the quads are deleted, and
     the ring is sized by OPEN ruling 12.
   - Tracers in the forward pass.
   - `tools/vfx/` textures and flipbooks, and the full `slice.vfx.json`.
   - Test 33, its goldens, and the `decals` budget.
4. **(d) Smoke in RT GI, the grenade, budgets and the close.**
   - Smoke spheres in `shadow_trace` and `gi_trace`, the grenade's boom, smoke and scorch, and the
     transient boom light.
   - `render.stats`/`render.settings` `vfx`, `vfx.info`'s frame half, and `render_bench
     play_vfx_1080p` with every budget gated.
   - Test 34, test 35, `check_live`, `e2e_mcp` and validation.
   - Docs (AGENTS, ARCHITECTURE "add an effect", the serve guide, the monograph's VFX units), the
     change record, ROADMAP row 12 "built, waiting for Frank".

## Risks

- **GPU float determinism under sorting.** A sort whose keys tie in floating point must tie-break by
  integer index, or two runs blend in different orders. Keys are `(quantized depth, object id,
  particle index)`, compared as integers.
- **The forward light loop drifting from the deferred one.** Two copies of the same arithmetic in
  GLSL and Slang. A parity test (a forward-shaded opaque test plane equals the deferred plane within
  1 LSB, RT terms off) guards it (OPEN ruling 19).
- **TAA ghosting on moving particles.** No velocity is written for transparents. The transparency
  mask lowers history weight under them. Look shots at 240 Hz judge it, and V1/V7 confirm.
- **Decal normal blending in an encoded normal target.** If the G-buffer normal encoding is not
  linear (octahedral), blending is wrong. The decal pass then replaces the normal under the decal's
  alpha mask (thresholded) rather than blending it (OPEN ruling 16).
- **`tools/list` space.** About 124 bytes are free. Trimming other tools' docs must not drop
  information agents rely on (OPEN ruling 20).
- **`MASK` in RT costs any-hit invocations.** They are only on `MASK` geometry, measured in
  `render_bench` (the Khronos scenes and the slice, which has none).
- **Moving goldens.** Every moved golden gets a named reason and a look before `--update`, never a
  widened tolerance.

## Review focus

1. **A round restart in the same step as a burst of hits:** the records of the abandoned timeline
   are gone, the restored step's records are written, and nothing from before the restart is drawn
   in the next capture (Task 1, `t31_replay`; Task 2, `t32_capture_any_history`).
2. **Two transparent objects at exactly equal depth, and a camera inside a smoke cloud:** the same
   draw order every run, and the cloud's particles behind the near plane are culled without a
   flicker (Task 2, `t32_sort_order`).
3. **A decal box crossing the view model or an emitter mesh, and a decal on a face at exactly 60°:**
   nothing on the gun or the lamp's sphere, and the threshold inclusive the same way every run
   (Task 3, `t33_decal_wraps`).
4. **The caps reached in one step** (a 300-round mag dump into a wall, a grenade in the same step):
   the oldest dropped by kind, counted, the same ones every run, no cost spike (Task 1,
   `t31_caps`; Task 4, the budget).
5. **A `MASK` texture whose alpha is exactly the cutoff:** the G-buffer, the shadow ray, the GI ray
   and the path tracer agree (Task 2, `t32_mask_rays`).

## As built (step 1)

The data and the step (plan Task 1, `docs/changes/2026-10-28-vfx-step1.md`). Nothing drawn changes:
the old `render/particles` and `combat_fx` path still draws in windows until step 2. What differs from
the design above, and why:

- **Seeds come from the vfx step and the event's index, not the event's `seq`.** A record's seed is
  `vfxHash(vfx step, 8 × index + part, 0xEF7)`, where the index counts the step's combat events after
  its last `restart`. The combat log's `seq` is not part of a snapshot or of a round restart, so a
  seed from it would differ after a restore or a restart; the vfx step counter is restored with the
  records. `sourceSeq` still holds the event's `seq` for agents and tests, never for a seed.
- **The muzzle is the logical eye, 0.5 m ahead** (`kMuzzleAhead`), not the `Muzzle` entity. The play
  scene's `Muzzle` is a `JointAttach` on the arms' `gun` joint and the enemies' rifles: the pose
  system places it, which is presentation and never read by the step. Every host writes the same
  point. Step 2 draws muzzle effects from the drawn muzzle in live windows ("Determinism").
- **A tracer ends at the same shooter's next `hit` in the step**, else 60 m along the round
  (`kTracerMiss`); the shot's ordinal is `WeaponState.shots`, `shots % every == 0`.
- **Decals attach to the `hit`'s target by its pose at the step, without the body-box re-cast.**
  Nothing is placed on the input-driven player or its subtree. The re-cast against an enemy's body
  boxes, the decal ring and `impact_marks` stopping drawing are step 3's (OPEN ruling 12). An
  explosion's scorch is posed with −Z up, so it projects straight down.
- **A restart puts back the round start's records** rather than clearing: the round's snapshot is
  restored through the vfx `SnapshotHook` inside the combat step, and the vfx step then reads only
  the events after the restart. The round start's snapshot is taken before the round's first vfx
  step, so it holds none, and the steps after a restart write the round's records again, step
  numbers and seeds included (`t31_replay`).
- **The record** is `{kind, effect, seed, seq, spawnStep, sourceSeq, source, on, pose, a, b, life}`:
  `seq` orders records for the `vfx.info` cursor, `source` is the shooter, `life` the effect's
  longest life (0: kept until its cap drops it). It expires once `(step − spawnStep) · dt > life`.
- **`World::addSimulateBefore(anchor, name, fn)`** places `vfx` before `audio_schedule` whatever
  order the hosts install in; the log channel `vfx` is new (and `log.read`'s channel list with it).
- **`render_host` installs nothing:** the CLI context installs the simulation systems for `ohao run`
  and `ohao serve` alike (`tools/ohao/main.cpp`); `render_host` adds only render-side systems.
- **`vfx.info`'s records are `{seq, step, effect|tracer|decal|light: name, at, on?}`** (`at` the world
  position, a decal's through its body's pose; `on` only for an attached decal), with `set`, `steps`,
  `caps`, `live` by kind, `dropped` by kind only when any were, `particles: null` headless, and
  `next`. 16 records measured 1,593 bytes: the 1 KB proposal cannot hold 16, so `t35_vfx_info_step`
  holds them to 2 KB, as `audio.info` (OPEN ruling 20). `serve_bench vfx_info_bytes` stays Task 4's.
- **`tools/list`:** 24,855 bytes with `vfx_info` added; eleven method descriptions trimmed of repeated
  prose (no field dropped) bring it to 24,560 of 24,576, 39 tools. 16 bytes remain: step 4's frame
  half and the settings schema need room found again (OPEN ruling 20).
- **`VfxEmitter.rate`** is in `Hz` (the registry's unit for a count per second).
- **The closed form is checked against an RK4 integration at 1 ms**, not Euler: explicit Euler's own
  error under gravity alone is g·T·dt/2, 9.8 mm at 1 ms over 2 s, above the 1 mm the test asserts. Worst
  difference 0.26 mm (drag 0), 0.004 mm (drag 4).
- **Not yet tested:** `t31_window_equals_headless` (`check_live.py`) and an `input.replay` of a fight
  through `vfx.info` come with the GPU harnesses' VFX legs in step 4; `t31_replay` covers two runs, a
  restore mid-fight and a round restart.

## Rulings for Frank

PROVISIONAL 1–9 and OPEN 10–27 await Frank, each with its recommendation.

PROVISIONAL (Frank's brief; the spec is designed to them):
1. **Determinism: closed-form particles from step-written spawn records, evaluated on the GPU.**
   The simulation goldens and replays see is the records (a function of step count, events and
   per-record seeds) plus a closed form in age. A particle has no integrated state. The GPU
   evaluates it in a fixed order.
   - **Why not keep a stateful GPU simulation:** today's GPU particles (atomic slot allocation,
     per-frame seeds, Euler at a fixed 0.016 s) cannot be snapshotted, restored or replayed without
     reading GPU buffers back. A fixed-order stateful GPU simulation would also need its buffers in
     every snapshot.
   - **Why not simulate on the CPU:** that would replay, but at a cost per particle per step and
     an upload per frame.
   - **Closed form is the cheapest of the three:** the snapshot is the records (≤ 256 × ~120 B),
     and any frame at any alpha is a pure function.
   - **The cost:** no collision or feedback between particles. Those are out of 12's scope.
   - Presentation-only extras (the drawn muzzle, interpolation alpha) never reach a golden.
2. **Transparency:** a sorted forward pass after deferred lighting, per object back to front with a
   stable id tie-break, per particle only inside alpha-blended emitters (≤ 1,024). OIT is OPEN 10.
3. **Lighting:** particles and transparent surfaces are lit by the same clustered lights, plus a
   cheap ambient term (OPEN 13). Particles are soft by depth fade.
4. **Decals:** projected box decals in the G-buffer (albedo, roughness, normal per OPEN 16) replace
   the quad marks. They keep `impact_marks`' deterministic placement and run in a capped ring.
5. **Tracers:** camera-facing stretched quads from `shot` events, the head at `speed · age` from the
   step, in the forward pass.
6. **Smoke in RT GI:** smoke contributes a coarse proxy to RT GI and shadows (method OPEN 18).
7. **Content:** muzzle, impact (per surface), smoke, tracer, grenade boom and decal sets as a
   `.vfx.json` named by scene data, plus placed emitters as components. Textures are generated by
   `tools/vfx/` (ours, MIT) or CC0, each folder with README and licence notes. Non-committable
   staging goes only under git-ignored `assets/local/`. No Somnabulate or Flat42 content.
8. **Out of scope:** fluid simulation, a volumetric fog rewrite, destruction, a VFX editor UI,
   ragdolls (13), the HDRI flip (16), pacing (SP9 ruling 21).
9. **Order:**
   - (a) data, records and agent inspect;
   - (b) sorted forward transparency with lit, soft particles and transparency goldens;
   - (c) decals, tracers and the content set;
   - (d) smoke in RT GI, the grenade, budgets, `vfx.*` and the close.

OPEN (judgment calls; recommendations given):
10. **Sorted or OIT (weighted blended).** Recommended: **sorted** for 12. The slice has few
    intersecting transparent layers; sorting is exact, deterministic and matches the path tracer's
    reference; WBOIT approximates colour, breaks premultiplied/additive mixes and costs two extra
    targets. Revisit WBOIT for foliage-heavy or glass-heavy levels (a later row), keeping the sort
    for particles.
11. **Sort granularity.** Recommended:
    - per object (emitter, tracer, `BLEND` instance) by bounding-centre depth;
    - per particle inside alpha or premultiplied emitters of ≤ 1,024 particles;
    - additive emitters unsorted (order-independent);
    - intersecting `BLEND` meshes accepted as wrong where they interpenetrate (the classic
      limitation; no per-triangle sort).
12. **Budget numbers and caps** (the tables above): first proposals, confirmed from Tasks 1–3's
    measurements before they gate. Two points within it:
    - **The VFX share:** 0.6 ms keeps the slice under 3 ms at 240 Hz.
    - **The decal ring:** 256 (up from the marks' 64), because projected decals cost per covered
      pixel, not per instance.
13. **Particle lighting resolution and ambient.** Recommended:
    - **Per particle, not per pixel**, evaluated once in `vfx.expand` at its centre: clustered lights,
      the sun through one shadow ray, ambient from RT GI's radiance at its screen position (the
      environment's irradiance without RT GI).
    - **Cheaper and stable:** no per-pixel loop over 256 lights for overdraw-heavy smoke.
    - **The cost:** a large puff is lit as a point; puffs are small in the slice.
    - **`BLEND` meshes** stay per pixel.
14. **Shadows and transparency.** Recommended:
    - particles receive the sun's shadow (the ray in OPEN 13) and point lights unshadowed;
    - `BLEND` meshes receive the sun's shadow per pixel and cast none in the hybrid frame;
    - smoke casts only through its proxy spheres (OPEN 18);
    - `MASK` casts and receives as opaque;
    - the path tracer handles all of it stochastically.
15. **TAA and transparency.** Recommended: the forward pass writes coverage to an `R8`
    transparency mask, and TAA lowers its history weight (a reactive mask) where the mask is set. No
    velocity is written for particles. A later upscaler (16) reuses the mask.
16. **Decal G-buffer channels and normals.** Recommended:
    - **Written:** albedo and roughness, blended by the decal texture's alpha; the normal, from the
      decal's normal map in the surface's tangent frame (built from the G-buffer normal and the
      decal's axis).
    - **Normal compositing:** blended if the G-buffer normal target is linear, else replaced under a
      thresholded alpha (the build checks `gbuffer_pass.cpp`'s normal format first).
    - **Never touched:** emissive, metallic and the material row's other channels.
17. **Decals and RT.** Recommended: decals are raster-only. RT GI, RT shadows and the path tracer
    do not see them. The quad marks were in the TLAS as black cards, which was wrong for GI. The
    path tracer, the offline reference, shows no bullet marks in 12; a later row can project
    decals in the closest-hit shader if Frank wants them in path-traced stills.
18. **The smoke GI method.** Recommended: **analytic density spheres**. Each smoke record publishes
    up to 3 spheres (closed-form centre, radius and density in age; ≤ 32 in all) to a storage
    buffer that `shadow_trace` and `gi_trace` loop over per ray.
    - **Transmittance:** `exp(−density · chord)`.
    - **In-scatter:** GI rays that end in a cloud add a flat ambient in-scatter by `1 − T`.
    - **Path tracer:** ignores smoke in 12.
    - **Alternatives:**
      - (a) procedural AABBs in the TLAS with an intersection test: TLAS rebuilds per frame and
        candidate handling in two shaders;
      - (b) a froxel or world density grid: a 3D image written per frame, ~0.1 ms, more memory,
        finer shape.

      Spheres are the cheapest that make "a cloud in a doorway dims bounce" true and measurable.
      A grid can replace them later behind the same proxy buffer.
19. **The forward light loop.** Recommended: a Slang module
    (`shaders/includes/lighting/clustered_lights.slang`) used by `vfx.expand` and the `BLEND`
    forward shader, while the deferred GLSL loop stays untouched so no deferred golden moves. A
    parity test pins the two (Risks). The alternative, one GLSL include for both, moves the deferred
    golden's arithmetic and breaks "new shaders are Slang".
20. **`tools/list` space for `vfx_info`.** The list is at 24,452 of 24,576 bytes. Recommended: trim
    the longest tool descriptions (`render_capture`, `world_apply`, `input_inject`) by ~800 bytes of
    repetition, with no field dropped, and keep the 24 KB cap. Alternative: raise the cap to 28 KB by
    Frank's ruling. `vfx.info` is the only new tool; effects are driven through scene data and
    input.
21. **Tracer cadence and look.** Recommended:
    - every enemy round, every third player round, none for hits nearer than 4 m;
    - 420 m/s visual speed, 1.8 m long, 12 mm wide, HDR 10 (bloom makes the glow);
    - judged in V4.
22. **Effects in every host.** Captures, `ohao run` and headless sessions draw effects, so
    `setParticlesDrawn` and SP9's "a lockstep capture draws no live particle" go.
    - **Recommended:** yes. Effects are a function of the step now, so window and headless captures
      stay bit for bit with them drawn.
    - **The cost:** `cli_blockout_play_hud` moves once.
23. **Material alpha modes in scope.** Recommended: yes, glTF `MASK` and `BLEND` in 12 (row 12's
    "sorted forward transparency" needs real transparent surfaces to sort). The Khronos alpha
    goldens move once, looked at. `MaterialOverride` gains `alphaMode` and `opacity` (a saved-type
    change, `world.hash` moves once).
24. **Pass order: effects after SSS and SSR, before bloom and TAA.** Recommended: yes. SSR's resolve
    and SSS rewrite the lit image from opaque data, so transparents belong after them. Bloom and
    TAA must see them, for glow and for the reactive mask. This moves particles later than today.
25. **The flipbook format.** Recommended:
    - a PNG atlas of `cols × rows` equal frames, sRGB colour with linear alpha;
    - frames chosen by `life`, `random` or index, no frame blending in 12;
    - loaded into the bindless table at set load;
    - BC compression and mips by atlas cell wait for 15.
    - `tools/vfx/make_flipbooks.py` generates smoke (8×8 of 64 px, curl-noise puffs), flash (4×1),
      spark and the decal textures from fixed seeds, rebuilt byte for byte.
26. **The grenade in flight drawn.** Recommended: yes, a small generated mesh (ours) drawn from the
    combat system's runtime grenades in the `vfx` sync system (presentation, like the old marks: never
    in the registry). A thrown grenade the player cannot see is a test tool, not a slice feature.
    Frank decides if the test grenade stays a test tool.
27. **The old particle system.** Recommended: delete `ohao/render/particles/*`,
    `shaders/particles/*` and `runtime/combat_fx.*` once Task 2 draws every effect from records
    (forward only). `render_bench play_fx_1080p` becomes `play_vfx_1080p`, and the 0.1 ms
    `particles` budget becomes the `transparent` budget above.
