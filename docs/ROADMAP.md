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

## Sub-projects

Each one gets its own spec → plan → build, and ends with something runnable and
measurable.

| # | Sub-project | Done when | Spec |
|---|---|---|---|
| 1 | **Runtime shell** — window, swapchain presentation, fixed-step loop, input actions, Forward path removed | deferred renders straight to a window at ≥ 60 fps, zero validation errors, no per-frame readback | [spec](specs/2026-10-02-runtime-shell-design.md) |
| 2 | **ECS world + GPU-driven scene** — EnTT registry replaces Scene/Actor/Component; persistent GPU instance/material buffers; GPU frustum + two-pass Hi-Z occlusion culling; indirect draws for the G-buffer; per-frame TLAS updates for moving instances | golden scenes rebuilt as ECS entities render identically; CPU draw-submission cost flat as instance count grows | — |
| 3 | **Level pipeline** — glTF-from-Blender loader, `extras` → components, collision generation, spawn points | a Blender blockout level loads, renders and collides | — |
| 4 | **Player** — Jolt at fixed step, physics ↔ transform sync, first-person controller, camera, footsteps | you can walk the level | — |
| 5 | **Combat** — hitscan weapon (recoil, spread, muzzle flash, impact decals), health/damage, enemy AI, death/restart, HUD | the slice is playable | — |

## Next, scheduled by need

Pulled in when the slice's scene complexity or look demands them, each as its
own sub-project: meshlets / mesh shaders with cluster culling; clustered
lighting; virtual shadow maps; ReSTIR / probe-based GI for the raster path;
DLSS SR / FSR / XeSS behind one upscaler interface.
