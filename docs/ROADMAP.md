# Roadmap — OHAO, an AI-native game engine

OHAO is a general-purpose game engine with two kinds of users from the start: humans and
AI agents. Existing engines are built around a human in an editor, and give agents access
afterwards through plugins that drive that editor. OHAO turns this around: the agent
surface is the foundation, and the editor will be one more client of it.

- **Agents are first-class users,** with two explicit goals:
  - **G1:** agents build games with OHAO end to end: author scenes, run and play them,
    observe, iterate, and later write gameplay code.
  - **G2:** agents develop OHAO itself: one obvious way to build and verify, with
    machine-readable results, an accurate map of the code and a recipe for every common
    change.
- **One API for everyone.** Every authoring operation goes through `ohao/api`: typed
  commands with undo, canonical scene files with a published schema, deterministic
  headless runs. Agents reach it through the `ohao` CLI (3) and the MCP server (4); the
  future editor calls the same methods in-process.
- **The quality bar is production AAA,** IW-Engine class, not a showcase.

**The first proof is a small first-person vertical slice** (sub-projects 5–7): one level, a
character controller, a hitscan weapon, a few enemies, built with the engine's own tools
and agents. The slice proves the engine; it is not the goal. Every sub-project leaves the
engine more general, and the slice pulls on whatever it needs. With the slice playable (7),
sub-projects 9–15 take it to the bar, IW/CoD-quality feel: latency and the view first, then
animation, audio, effects, ragdolls, a game module and an asset database.

## Principles

- **Performance first.** Performance is a gate: benchmarks with checked-in budgets are
  part of done criteria, and reflection never runs on per-frame paths.
- **Structure before effects.** Structural frontier features (a GPU-driven scene,
  meshlets, clustered lighting) go in early, because retrofitting them rewrites
  everything that feeds the renderer. Effect-level features plug in when the slice needs
  them.
- **Determinism.** World state is reproducible from a scene, a seed and an input
  recording; renders are reproducible wherever the hardware allows, and the tests say
  exactly where.
- **Language per layer:** the C++20 core is kept. New engine layers, tools and the
  control surface are compiled and low-overhead (C++20; C ABI for embedding). No
  TypeScript or GC runtime on hot paths; Python only in test harnesses. New shaders are
  written in Slang from 2b. Layers meet at language-neutral boundaries: the scene format
  and its JSON Schema, the schema catalogue, the protocol, and the C ABI.
- **Scripting (proposed direction, not a commitment):** native C++ gameplay systems with
  hot reload first; a sandboxed WebAssembly tier later for agent- and mod-written code.

Foundation decisions (2026-10-02), still in force:

- **Game render path:** hybrid raster — deferred G-buffer core with optional RT
  shadows and RT GI. The path tracer stays as ground truth.
- **Game code:** ECS on EnTT, systems run in an explicit fixed order.
- **Levels:** authored in Blender, exported as glTF 2.0; node `extras` map to
  components.

## Sub-projects

Each one gets its own spec → plan → build, and ends with something runnable and
measurable.

| # | Sub-project | Done when | Spec |
|---|---|---|---|
| 1 | **Runtime shell** — window, swapchain presentation, fixed-step loop, input actions, Forward path removed | deferred renders straight to a window at ≥ 60 fps, zero validation errors, no per-frame readback | [spec](specs/2026-10-02-runtime-shell-design.md) · **done** |
| 2a | **ECS world + persistent GPU scene** — EnTT registry replaces Scene/Actor/Component; render-world extraction into persistent GPU mesh/material/instance/light tables; one BLAS per mesh; per-frame TLAS refit; one bindless texture table | golden scenes rebuilt as ECS entities render identically; moving objects correct in RT; upload cost proportional to change | [spec](specs/2026-10-03-ecs-gpu-scene-design.md) · **done** |
| 3 | **Reflection, scene serialization + agent API** — type registry with docs/units/limits, schema catalogue + JSON Schema, stable entity IDs, canonical `.scene.json` (defaults omitted), structured validation errors, versioning, command layer with inverses, filtered/projected queries and scene summaries (token budget for agents), columnar snapshot/restore, headless `ohao` CLI; `AGENTS.md`, `docs/ARCHITECTURE.md`, refreshed `.claude/skills` | the example scenes ship as `.scene.json` files and the goldens render from them via `ohao run`; save → load → save is byte-identical; `scene_bench` budgets met | [spec](specs/2026-10-22-reflection-scene-serialization-design.md) · **done** (2026-10-23; skills installed the same day; every `scene_bench` budget and scaling ratio passes, `restore_empty` at ×10.1–11.4 over twelve consecutive runs after the restore follow-up) |
| 4 | **Agent control surface + agent dev ergonomics** — `ohao serve` (JSON-RPC 2.0 / MCP over stdio), live window host, lockstep/realtime time control, input as data with injection and record/replay, live frame capture, log streaming; deterministic deferred rendering; one `check` entry point (CTest labels, shaders first, JUnit/JSON results), golden diff JSON + heatmaps, `ohao doctor` | an MCP agent loads, edits, plays and captures a scene in a live engine; an agent builds and verifies OHAO from one command with machine-readable pass/fail; `serve_bench` budgets met | [spec](specs/2026-10-23-agent-control-surface-design.md) · **done** (2026-10-24; tests 1–20 pass; an MCP client loads, edits, plays, records, replays and captures `helmet_game` in a live window, bit-identical on replay. Open, owned by 2b by Frank's ruling: the window readback (2 ms) and inline capture (20 ms) budgets, which miss on the deferred frame's own GPU cost. Waiting on Frank: the repo's `.mcp.json` and the manual Claude Code run) |
| 2b | **Renderer upgrade: measured, GPU-driven, cheaper, correct** — per-pass GPU timings and a render benchmark first; RT shadows (scene lights, 1 ray a light) and RT GI (half resolution, denoised) rebuilt to cut the deferred frame from ~5.4 ms to ≤ 1.5 ms at 720p; GPU frustum + two-pass Hi-Z culling with deterministic compaction and indirect draws; meshlets with mesh shaders and a fallback; clustered lighting (the 8-light cap gone); SSAO wired in; path-traced frames in a window; readback of the frame just rendered; glTF node transforms; RT determinism and validation; Slang for new shaders; `render.stats` / `render.settings` for agents (see "2b scope") | `render_bench` gates every pass; `serve_bench`'s window readback (2 ms) and inline capture (20 ms) budgets met unchanged; CPU submission flat from 10k to 100k instances; a 100k-instance, 1,000-light scene within 8 ms at 1080p; deferred and hybrid-RT goldens exact; validation clean in raster and RT | [spec](specs/2026-10-24-renderer-upgrade-design.md) · **built** (all 15 build steps, 2026-10-25; tests 1–16 pass, goldens exact, validation clean: `cornell_720p` at 0.97 ms of GPU, from 4.64; `bench100k_1080p` 4.15 ms with 1,000 lights, budget 8; `serve_bench`'s window readback 1.30 ms, budget 2, inline capture 8.2 ms, budget 20; agents read and set the frame through `render.stats`, `render.settings` and capture timings. Open: the five consecutive clean runs of every budget, every run so far noisy from outside load, every budget within its value) |
| 5 | **Level pipeline** — glTF-from-Blender loader, `extras` → components, collision generation, spawn points | a Blender blockout level loads, renders and collides | [spec](specs/2026-10-26-level-pipeline-design.md) · [plan](plans/2026-10-26-level-pipeline.md) |
| 6 | **Player** — Jolt at fixed step in the ECS, physics ↔ transform sync, first-person controller, camera, footsteps | you can walk the level | [spec](specs/2026-10-26-player-physics-design.md) · [plan](plans/2026-10-26-player-physics.md) · **done** |
| 7 | **Combat** — hitscan weapon (recoil, spread, muzzle flash, impact decals), health/damage, enemy AI, death/restart, HUD | the slice is playable | [spec](specs/2026-10-27-combat-design.md) · [plan](plans/2026-10-27-combat.md) · **done** (2026-10-27; `blockout_play.scene.json` is a round: fire, reload, recoil and bloom, flash, marks, sounds, a HUD; two enemies that see, chase and shoot; kill both and win, die and restart to the round's start bit for bit; an agent plays the same fight over MCP through `combat.info`, `combat.events`, `combat.damage` and input, a window and a headless host reaching the same `world.hash` and HUD picture. Open: every bench run noisy from outside load) |
| 8 | **OpenUSD interop** (by need, after 15) — USDA/USDC import and export for robotics and world-model pipelines (Omniverse, Isaac Sim): UsdGeom transforms and meshes, UsdLux lights, UsdPhysics where it maps, and every other OHAO component as namespaced custom attributes generated from the type registry | a scene goes OHAO → USD → OHAO with every mapped component identical, and the exported stage opens in usdview / Omniverse | — |
| 9 | **Feel** — input-to-present latency measured and reported (`render.stats`); the camera turned at render rate by a presentation-only look lead the step consumes with the same arithmetic (the step's aim stays authoritative, the lead never hashed); present modes and late input sampling; frame pacing; the gun's own field of view and depth placement, sway, bob and kick (render side; its animations are 10's); GPU particles from combat events; audio distance curves, a voice cap and one occlusion ray; SP7's leftovers and Frank's hand test | a mouse movement reaches the screen within 15 ms (median, poll to present, 144 Hz, vsync off); the camera turns every frame; frame time p99 ≤ 1.5 × median; every simulation test and golden unchanged; Frank's hand-test list closed | [spec](specs/2026-10-27-feel-design.md) · [plan](plans/2026-10-27-feel.md) · **built, waiting for Frank** (2026-10-28; all three steps: look latency 4.83 ms median at 240 Hz, the camera turned every frame by the look lead; the gun's own lens, sway, bob and kick, never in a wall; sparks, dust and muzzle bursts in a window; a 24-voice cap, distance curves and an occlusion ray; the won screen without crosshair. Open for Frank: the hand test H1–H7 (spec "Hand-test acceptance"), and pacing, `frame_p99_over_median` 2.01 against 1.5 from the presentation engine's stalls, ruling 21. Moved to 16: the doorway's RT GI speckles (the HDRI's sun in GI rays) and the near-plane depth clip) |
| 10 | **Animation** — skeleton and animator components, glTF skins and clips, GPU skinning feeding the G-buffer, meshlets, a refitted BLAS and previous-pose motion vectors; enemy full body (locomotion, aim, fire, reload, hit reactions, death, events driving sounds); then the player's arms and gun (idle, fire, reload, ADS, sprint); clip time from the step count. Face, cinematics and a third-person player are out | two skinned enemies run, aim, shoot, flinch and die, validation clean in raster and RT (shadows and GI see the deformed mesh); culling and determinism tests cover skinned meshes; a skinning budget; the viewmodel's reload matches the weapon's | [spec](specs/2026-10-28-animation-design.md) · [plan](plans/2026-10-28-animation.md) · **built, waiting for Frank** (2026-10-28; all four steps: glTF skins and clips read by our own reader, an Animator of saved integers, GPU skinning that every renderer sees (G-buffer, meshlets, a refitted BLAS for RT shadows, GI and the path tracer, previous-pose motion vectors); two skinned CC0 enemies that move, aim, fire, reload, flinch and die, their events playing sounds; the player's arms with idle, fire, reload timed to the weapon's, ADS and sprint; validation clean in raster and RT, goldens exact, every SP10 budget met on quiet runs. Open for Frank: the hand test A1–A11 (spec "Hand-test acceptance") and the provisional rulings 1–8, 11, 13 and 16) |
| 11 | **Audio system** — a mixer with buses, ducking and snapshots; voice priority; occlusion; reverb zones from level hints; scheduling from step events; `audio.*` for agents; a local staging path for real recordings beside a committed generated or CC0 set | the same fight gives the same voice schedule; occlusion audibly changes behind cover; a mix-cost budget; Frank signs off the mix by ear | [spec](specs/2026-10-28-audio-design.md) · [plan](plans/2026-10-28-audio.md) · **built, waiting for Frank** (2026-10-28; all four steps: one `.audio.json` bank of buses with ducks, snapshots and caps, mixed by nodes of ours that start voices on the frame; a Freeverb return following the listener's reverb zone, and occlusion graded by the walls between; every sound decided in the step by a voice schedule (priority, stealing, `holdMs`, the round's snapshots, enemy footsteps from their clips) that replays with snapshots and that a window's player renders a step later; ambience beds; `audio.info` and `audio.mix` for agents and `render.stats` `audio`; every audio budget met; the blockout's outdoor ground 5 cm below the room's floor (ruling 21). Open for Frank: the hand test U1–U10 (spec "Hand-test acceptance"), the provisional rulings 1–6 and open rulings 7–20, and a quiet rerun of `scene_bench`, `physics_bench` and `render_bench`, noisy all that day) |
| 12 | **VFX and transparency** — lit and soft particles through the clustered lights, sorted forward transparency, projected decals replacing the quad marks, tracers, smoke that reads in RT GI | a muzzle, impact and smoke set authored as scene data; a VFX budget; transparency goldens exact | [spec](specs/2026-10-28-vfx-design.md) · [plan](plans/2026-10-28-vfx.md) · **Day 1 docs written** (2026-10-28; documents only: effects as spawn records from the step drawn in closed form, sorted forward transparency with glTF alpha modes, projected decals, tracers, smoke spheres in RT GI and shadows; provisional rulings 1–9 and open rulings 10–27 wait for Frank) · **Task 1 built** (2026-10-28, [change record](changes/2026-10-28-vfx-step1.md): the `.vfx.json` set, the closed form, spawn records from the step, `vfx.info`; nothing drawn changes yet, V1–V8 open) · **Task 2 built** (2026-10-28, [change record](changes/2026-10-28-vfx-step2.md): glTF alpha modes in the G-buffer, RT and the path tracer, the sorted forward `transparent` pass with lit, soft particles from the step's records in every host, the old particle system deleted, test 32 and the transparency goldens; V1–V8 still wait for Frank's hand test) · **Task 3 built** (2026-10-28, [change record](changes/2026-10-28-vfx-step3.md): projected decals from the step's records in a `decals` pass after `gbuffer.late` (the quad marks deleted), tracers in the forward pass, the slice's set and its generated textures; test 33 and the `cli_blockout_play_hud` golden moved; V1–V8 still wait for Frank's hand test) · **Task 4 built, waiting for Frank** (2026-10-29, [change record](changes/2026-10-28-vfx-step4.md): smoke proxies in the RT shadow and GI traces with no in-scatter (parent ruling 29: the filled doorway dims the floor 34.6% against the 20% bar), the grenade's boom, smoke and scorch, the `vfx` hint, `vfx.info`'s frame half, `render.stats`/`render.settings` `vfx`, the play_vfx_1080p moment gated on its decals, VFX total, frame and smoke-proxy rows; not built: the monograph's vfx units; `check.py --all` is 52 of 58, its misses standing, noisy or a quiet rerun's (listed in the change record); V1–V8 open for Frank's hand test) |
| 13 | **Joints and ragdoll** — the backend's constraints as ECS components and scene data; a death ragdoll blended from the last animated pose; hit impulses; exact replay from snapshots | a shot enemy falls as a ragdoll and replays bit for bit from a mid-fall snapshot; a budget for 16 ragdolls | — |
| 14 | **Game module and hot reload** — combat, enemy AI and the round moved out of `ohao/ecs` into a game module of native C++ systems; reload as save, unload, load, restore | an edited enemy AI is picked up mid-fight in a running window with the same `world.hash`; the engine builds without the game | — |
| 15 | **Asset database and cooking** (absorbs 5's deferred kit2k) — content hashes and a cooked cache: compressed textures with mips, meshlets, BLAS-ready buffers, clips; streaming at load; `asset.*` for agents | kit2k's budgets met (load ≤ 400 ms, collision build ≤ 150 ms, a ray ≤ 5 µs, an idle sync ≤ 10 µs); a real kit loads cooked with at least 4× less VRAM | — |
| 16 | **Render to budget** (by need) — upscaling behind one interface (FSR first), virtual shadow maps, ReSTIR DI and GI, when a real level's frame asks; the standing Cornell lighting miss and quiet bench windows; every HDRI looked up upside down (found 2026-10-28: the sky lights from below; the fix moves every HDRI-lit golden) | a real level's 1440p frame within budget at high settings | the outdoor RT GI fireflies (moved here from 9: the HDRI's sun in GI's rays) **fixed** 2026-10-28: a directional light stands for the sun and the environment lights without it ([record](changes/2026-10-28-rt-gi-fireflies.md)); the rest open |

**Order:** 5–7 built (2b 2026-10-25, 6 and 7 2026-10-27). Next (Frank, 2026-10-27, adopting the
post-SP7 suggestions): **9 Feel**, then 10 Animation, 11 Audio, 12 VFX, 13 Ragdoll, 14 Game module,
15 Asset database. Feel goes first because the slice's largest feel defect is structural, not
content: the camera turns at the step's 60 Hz and is drawn a step late, and every later visual
system (arms, ADS, recoil, camera animation) sits on that path. Animation comes before audio
because skinning changes how geometry reaches the G-buffer, BLAS, culling and motion vectors
("structure before effects"), and its events time the sounds; audio's cheap wins ride in 9. 5's
step 6, a kit of 2,000 pieces, folds into 15. 16 and 8 are scheduled by need: 16 when a real
level's frame asks for it; 8 when a robotics or world-model pipeline does, after 15, on 5's import
path and the asset database. `.scene.json` stays the native authoring format; USD is the
interchange format. 9 and 10 are built and wait for Frank's hand tests and rulings; 11's spec and
plan are written (2026-10-28, Day 1, documents only), its stack, determinism, content and order
rulings provisional until Frank rules. 12's spec and plan are written (2026-10-28, Day 1,
documents only, after 11 was built), its determinism, transparency, lighting, decal, tracer,
smoke, content, scope and order rulings provisional until Frank rules.

Why 4 before 2b: 2b is a large renderer change, and every later sub-project is built and
checked by agents. 4 gives them the live engine, the one-command check, golden diffs they
can read and deterministic deferred captures, so 2b is built and verified with those tools
rather than before them.

## 2b scope: what joins GPU culling, and what waits

The spec ([2026-10-24-renderer-upgrade-design.md](specs/2026-10-24-renderer-upgrade-design.md))
is the authority; this is its summary. The test for each candidate is the "structure before
effects" principle: does it change how geometry, lights or frames flow through the
renderer (put it in 2b, while that flow is being rebuilt anyway), or is it an effect that
plugs into a finished flow (schedule it when the slice needs it)? Frank widened 2b on
2026-10-24 into a real renderer upgrade, and the frame's GPU cost became its first goal.

**In 2b:**
- **Measurement first:** a pass registry, GPU timestamps for every pass, `render_bench`
  with per-pass budgets, and `render.stats` / `render.settings` so agents read timings and
  change quality settings over MCP.
- **RT shadows and RT GI rebuilt for cost:** shadows from the scene's lights (today a
  hard-coded light) at 1 ray a light, filtered; GI at half resolution with 2 rays,
  reprojected temporal and à-trous denoising. This is what brings the frame to ≤ 1.5 ms.
- **Correctness:** SSAO wired into lighting, path-traced frames presented and captured in
  a window, readback of the frame just rendered (the lagged ring deleted), glTF node
  transforms applied on load.
- **GPU frustum + two-pass Hi-Z occlusion culling, indirect draws** (G-buffer and
  shadows): the core of 2b, as planned.
- **Meshlets with cluster culling.** Instance culling first and cluster culling later
  would mean writing the culling pipeline, the indirect argument layout and the visibility
  buffers twice. Meshlets are built at mesh upload (one more stream per mesh in the
  persistent `RenderScene`); mesh/task shaders draw them where supported, and a
  vertex-shader path over the same meshlet data stays as the fallback.
- **Clustered lighting.** The slice's levels will have hundreds of lights; today's
  lighting pass walks the whole light list per pixel. Clusters are built on the GPU from
  the same light table 2a made persistent, in the same frame flow culling rebuilds, and
  every lit pass (deferred lighting, particles, later forward transparency) reads them.
- **Slang for new shaders.** The culling, meshlet and clustering shaders are new, so the
  Slang toolchain (compile in the `shaders` target, the same flat SPIR-V naming) lands
  with them. Existing GLSL migrates only when a pass is rewritten.
- **RT determinism and the open renderer findings:** RT offline's rare 1-LSB flips,
  `rt_realtime` run-to-run noise, VUID 03004 and the NRI push-descriptor exit that block
  validation-clean RT, the double tint on untextured materials in RT, sky/particles
  lost while SSS is active, and SSAO, which is computed every frame it is on but never
  sampled by the lighting shader (wiring it changes the look, and its noise tiling and
  decode are wrong too; see the monograph's SSAO unit). 2b rebuilds the frame flow these live in; fixing them there is
  cheaper than around it, and validation-clean RT is a precondition for trusting a
  rebuilt renderer.
- **The deferred frame's GPU cost, for 4's two open budgets** (Frank's ruling,
  2026-10-24): `serve_bench`'s `window_readback_ms` (2 ms) and `window_capture_inline_ms`
  (20 ms) stay as they are and miss because one Cornell deferred 1280x720 frame takes
  5.0–5.6 ms of GPU time (RT shadows at 8 rays and RT GI at 32 samples a pixel); a capture
  waits on its frame's fence. 2b makes the frame cheaper (RT GI samples, RT shadows),
  which may change the deferred goldens.
- **A render benchmark with budgets:** the `scene_bench` 10k/100k scenes rendered, with
  CPU submission and GPU pass times gated, as `scene_bench` gates the codec.

**Later, scheduled by need** (each reasoned in the spec's "Scope"):
- **ReSTIR DI (many-light shadowing).** 2b shadows up to 4 lights a pixel; ReSTIR DI earns
  its complexity when the slice's levels need hundreds of shadowed lights.
- **Upscaling (DLSS SR / FSR / XeSS behind one interface).** It needs a render-resolution
  vs output-resolution split, but TAA jitter and motion vectors already exist, so the
  split is a contained change. Schedule it when the slice's frame budget asks for it, with
  FSR as the vendor-neutral first backend. Not in 2b: captures are at native resolution,
  the budgets are met without one, and each backend is a new dependency.
- **Virtual shadow maps.** CSM works and is validation-clean; VSMs are a quality upgrade
  that consumes 2b's culling (page-visibility passes reuse it). After the level pipeline
  shows where shadow resolution falls short.
- **ReSTIR / probe-based GI.** 2b rebuilds the 1-bounce RT GI for cost (half resolution,
  denoised); ReSTIR GI and probes are quality upgrades that reuse 2b's clusters, meshlets
  and denoiser. When the slice's lighting needs it.

## Next, scheduled by need

Upscalers, virtual shadow maps, ReSTIR DI and ReSTIR / probe GI (reasoned above) are row 16;
the asset database and cooked formats row 15; gameplay code with hot reload row 14. Pulled in
as their own sub-projects when the slice or a user needs them: soft bodies, water and cloth;
the editor UI (an in-process `ohao/api` client: ImGui docking on the Vulkan backend); a
sandboxed WebAssembly tier for agent- and mod-written code; the C ABI; network transports for
the control surface.
