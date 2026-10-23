# Roadmap — toward the FPS vertical slice

OHAO is a game engine. The target is IW-Engine-class production quality; the
first proof is a small first-person vertical slice: one level, a character
controller, a hitscan weapon, a few enemies.

Foundation decisions (2026-10-02):

- **Game render path:** hybrid raster — deferred G-buffer core with optional RT
  shadows and RT GI. The path tracer stays as ground truth.
- **Game code:** ECS on EnTT, systems run in an explicit fixed order.
- **Levels:** authored in Blender, exported as glTF 2.0; node `extras` map to
  components.
- **Structure before effects:** structural frontier features (GPU-driven scene,
  meshlets) go in early, because retrofitting them rewrites everything that
  feeds the renderer. Effect-level features plug in when the slice needs them.

General-engine track (2026-10-22): OHAO also grows toward a general-purpose
engine: editor, project/asset database, scripting, export. Sub-project 3 lays
its foundation (reflection + serialization); the slice keeps pulling on it.

- **Core thesis, AI-native:** existing engines are built around a human in an editor
  and add agent access through plugins. In OHAO the agent surface is the foundation:
  typed commands with undo shared by the editor and agents, canonical scene files plus
  a schema, deterministic headless runs, one protocol.
- **Agents as users:** AI agents are first-class users alongside humans.
  "Agentic" has two explicit goals:
  - **G1:** agents build games with OHAO end to end;
  - **G2:** agents develop OHAO itself easily.

  Every authoring operation goes through one API (`ohao/api`). Agents reach it through
  a deterministic headless CLI (3), then through JSON-RPC/MCP (4). The future editor
  is a client of the same API. Schemas, scene files, errors and test results are
  machine-readable.
- **Scripting:** proposed direction, not a commitment. Native C++ gameplay systems
  with hot reload first; a sandboxed WebAssembly tier later for agent- and mod-written
  code.
- **Performance first:** performance is a gate. Benchmarks with checked-in budgets
  are part of done criteria. Reflection never runs on per-frame paths.
- **Language per layer:** the C++20 core is kept. New engine layers, tools and the
  control surface are compiled and low-overhead (C++20; C ABI for embedding). There
  is no TypeScript or GC runtime on hot paths. New shaders are written in Slang from
  2b. Layers meet at language-neutral boundaries: the scene format and its JSON
  Schema, the schema catalogue, the protocol, and the C ABI.

## Sub-projects

Each one gets its own spec → plan → build, and ends with something runnable and
measurable.

| # | Sub-project | Done when | Spec |
|---|---|---|---|
| 1 | **Runtime shell** — window, swapchain presentation, fixed-step loop, input actions, Forward path removed | deferred renders straight to a window at ≥ 60 fps, zero validation errors, no per-frame readback | [spec](specs/2026-10-02-runtime-shell-design.md) · **done** |
| 2a | **ECS world + persistent GPU scene** — EnTT registry replaces Scene/Actor/Component; render-world extraction into persistent GPU mesh/material/instance/light tables; one BLAS per mesh; per-frame TLAS refit; one bindless texture table | golden scenes rebuilt as ECS entities render identically; moving objects correct in RT; upload cost proportional to change | [spec](specs/2026-10-03-ecs-gpu-scene-design.md) · **done** |
| 2b | **GPU-driven culling** — indirect G-buffer/shadow draws, GPU frustum + two-pass Hi-Z occlusion culling | CPU draw-submission cost flat as instance count grows | — |
| 3 | **Reflection, scene serialization + agent API** — type registry with docs/units/limits, schema catalogue + JSON Schema, stable entity IDs, canonical `.scene.json` (defaults omitted), structured validation errors, versioning, command layer with inverses, filtered/projected queries and scene summaries (token budget for agents), columnar snapshot/restore, headless `ohao` CLI; `AGENTS.md`, `docs/ARCHITECTURE.md`, refreshed `.claude/skills` | the example scenes ship as `.scene.json` files and the goldens render from them via `ohao run`; save → load → save is byte-identical; `scene_bench` budgets met | [spec](specs/2026-10-22-reflection-scene-serialization-design.md) · **done** (2026-10-23; the skills wait in `docs/agents/skills/` for a two-command install; every `scene_bench` budget and scaling ratio passes, `restore_empty` at ×10.1–11.4 over twelve consecutive runs after the restore follow-up) |
| 4 | **Agent control surface + agent dev ergonomics** — `ohao serve` (JSON-RPC 2.0 / MCP over stdio), input injection, live frame capture, pause/step, log streaming; one `check` entry point (CTest labels, shaders built first, JUnit/JSON results), golden diff JSON + heatmaps, `ohao doctor` | an MCP agent loads, edits, plays and captures a scene in a live engine; an agent builds and verifies OHAO from one command with machine-readable pass/fail | — |
| 5 | **Level pipeline** — glTF-from-Blender loader, `extras` → components, collision generation, spawn points | a Blender blockout level loads, renders and collides | — |
| 6 | **Player** — Jolt at fixed step, physics ↔ transform sync, first-person controller, camera, footsteps | you can walk the level | — |
| 7 | **Combat** — hitscan weapon (recoil, spread, muzzle flash, impact decals), health/damage, enemy AI, death/restart, HUD | the slice is playable | — |
| 8 | **OpenUSD interop** — USDA/USDC import and export for robotics and world-model pipelines (Omniverse, Isaac Sim): UsdGeom transforms and meshes, UsdLux lights, UsdPhysics where it maps, and every other OHAO component as namespaced custom attributes generated from the type registry | a scene goes OHAO → USD → OHAO with every mapped component identical, and the exported stage opens in usdview / Omniverse | — |

Order: 4 next. 2b and 5–7 follow, as the slice and scene scale need them. 8 builds
on the level pipeline's import path and the asset database, and is scheduled when a
robotics or world-model pipeline needs it. `.scene.json` stays the native authoring
format; USD is the interchange format.

## Next, scheduled by need

Pulled in when the slice's scene complexity or look demands them, each as its
own sub-project: meshlets / mesh shaders with cluster culling; clustered
lighting; virtual shadow maps; ReSTIR / probe-based GI for the raster path;
DLSS SR / FSR / XeSS behind one upscaler interface.
