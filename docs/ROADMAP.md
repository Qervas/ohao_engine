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
engine more general, and the slice pulls on whatever it needs.

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
| 2b | **Renderer upgrade: measured, GPU-driven, cheaper, correct** — per-pass GPU timings and a render benchmark first; RT shadows (scene lights, 1 ray a light) and RT GI (half resolution, denoised) rebuilt to cut the deferred frame from ~5.4 ms to ≤ 1.5 ms at 720p; GPU frustum + two-pass Hi-Z culling with deterministic compaction and indirect draws; meshlets with mesh shaders and a fallback; clustered lighting (the 8-light cap gone); SSAO wired in; path-traced frames in a window; readback of the frame just rendered; glTF node transforms; RT determinism and validation; Slang for new shaders; `render.stats` / `render.settings` for agents (see "2b scope") | `render_bench` gates every pass; `serve_bench`'s window readback (2 ms) and inline capture (20 ms) budgets met unchanged; CPU submission flat from 10k to 100k instances; a 100k-instance, 1,000-light scene within 8 ms at 1080p; deferred and hybrid-RT goldens exact; validation clean in raster and RT | [spec](specs/2026-10-24-renderer-upgrade-design.md) · **in progress** (build steps 1–8 of 15 done, 2026-10-25: `cornell_720p` at 0.87 ms of GPU, from 4.64) |
| 5 | **Level pipeline** — glTF-from-Blender loader, `extras` → components, collision generation, spawn points | a Blender blockout level loads, renders and collides | — |
| 6 | **Player** — Jolt at fixed step in the ECS, physics ↔ transform sync, first-person controller, camera, footsteps | you can walk the level | — |
| 7 | **Combat** — hitscan weapon (recoil, spread, muzzle flash, impact decals), health/damage, enemy AI, death/restart, HUD | the slice is playable | — |
| 8 | **OpenUSD interop** — USDA/USDC import and export for robotics and world-model pipelines (Omniverse, Isaac Sim): UsdGeom transforms and meshes, UsdLux lights, UsdPhysics where it maps, and every other OHAO component as namespaced custom attributes generated from the type registry | a scene goes OHAO → USD → OHAO with every mapped component identical, and the exported stage opens in usdview / Omniverse | — |

**Order:** 2b next, then 5–7. 8 is scheduled by need, when a robotics or
world-model pipeline asks for it; it builds on 5's import path and the asset database.
`.scene.json` stays the native authoring format; USD is the interchange format.

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

Pulled in as their own sub-projects when the slice or a user needs them: upscalers,
virtual shadow maps, ReSTIR DI and ReSTIR / probe GI (reasoned above); the editor UI (an
in-process `ohao/api` client: ImGui docking on the Vulkan backend); the asset database and
cooked formats; gameplay code with hot reload; the C ABI; network transports for the
control surface.
