# Step 2: delete and unify

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.2, 4.9 and 5 (step 2). **Status:** planned, 2026-11-09.

Every fact defined once, generated or checked where it is used; what nothing uses deleted. Nine parts, each its own
commit with `tools/check.py` green, in this order: deleting first shrinks what the unifying parts must touch, and
`AGENTS.md` goes last because the others turn its rules into code.

## 2a. Delete

- `core/event_bus.*`, `core/command.*` (no user outside `ohao/core`), `core/core.hpp` (included nowhere).
- `render/rt/render_technique.hpp`'s single-implementation interfaces (`IGITechnique`, `IReflectionTechnique`): the
  one implementation is used directly.
- The render graph's unreachable code: its allocator, render-pass and framebuffer paths (measured by what
  `render_graph.cpp` reaches from the deferred frame); `Model`'s LOD stubs; the hot-reload stubs.
- `tests/scenes/invalid/` (46 files) if nothing reads it.
- Header comments and docs that promise what does not exist (a C ABI, an editor, hot reload, a full render graph)
  or describe deleted code (`impact_marks`, the gpu→render cycle).

*Done when* each deletion builds and the gates pass; nothing deleted had a caller (grep and the linker say so).

## 2b. One `installEngine`

Today the window installs the render extract first and levels "before instantiate"; the CLI installs levels,
physics, combat, animation, audio and vfx, and its render host adds the render systems at the first capture; four
benches install their own subsets. `ecs::installEngine(World&, const EngineConfig&)` installs the simulation in its
one order; `ecs::installRendering(World&, RenderScene&, ...)` adds the render systems at fixed anchors, whenever a
renderer exists. Hosts differ by config (which parts, the audio trace), never by sequence. An install whose anchor
is missing is an error, not an append. The snapshot hooks chain explicitly (physics, then schedule, then vfx), so
order is no longer what keeps physics' hook.

*Done when* `ohao`, `ohao serve`, `LiveHost` and the render host call these two; the window's and the CLI's
system lists are equal; the gates pass (`replay_test` holds `world.hash`).

*As built (2026-11-09):* the order is a list, `ecs/system_order.hpp`: `World::addSync`/`addSimulate` place a system
at its name's position and throw on a name not listed or installed twice, so the lists are equal by construction
(the anchors and name probes are gone). The hosts differed in one pair: the window ran `instantiate` before
`player_spawn`, the CLI after; the list keeps the CLI's, which the goldens and `world.hash` expectations were
recorded with. Snapshot hooks are named and restored in `kSnapshotOrder` (physics, audio_schedule, vfx). The
benches keep installing single systems: each times one system, and with the order fixed a subset cannot run out of
order; making them install everything would change what their budgets measure.

## 2c. One method table

The API's doc rows and its dispatch are two tables (`api/session.*`). One row per method (name, params, result,
doc, mutates, handler) feeds dispatch, the CLI's method list, MCP `tools/list` and the docs table in `ohao-cli.md`.
A row without a handler, or a handler without a row, fails the build.

*Done when* the session has one table and `check_serve`/`e2e_mcp` pass unchanged.

## 2d. One `RenderSettings`

Three shapes (`api::RenderSettings`, `api::RenderSettingsParams`, `VulkanRenderer::RenderSettings`) with converters
both ways. One reflected struct in the renderer, its defaults written once; the API serializes it by reflection.

*Done when* one struct remains, `render.settings` replies are unchanged (`check_serve` expectations), and a new
setting is one field.

## 2e. Generated shader bindings

`slangc -reflection-json` (2026.20 has it) per shader at build; `tools/gen_bindings.py` turns it into
`build/generated/bindings/<shader>.hpp`: named set and binding slots, descriptor types and counts, the push-constant
struct, the flag constants. C++ uses the names. Migrated a pass family at a time (post, deferred core, RT direct
light, RT GI, path tracer, vfx), each with the gates green; the path tracer's push constants and flag bits (written
twice today, comments already disagreeing) come from its header. `gen_layout --check` runs at configure.

*Done when* no C++ file types a binding number (a grep at configure fails the build on one), and the goldens are
unchanged.

*As built (2026-11-09):* the headers are checked in (`ohao/render/shader_bindings/`, as `gen_layout` does), and
`tools/check.py`'s build stage runs `gen_bindings --check`: a stale header, a set/binding declared twice in one
shader, or a binding typed as a number in C++ fails it. Hand-written push-constant structs (with their defaults and
`glm` types) are held to the shader by `OHAO_CHECK_PUSH` (every field's name and offset, the size). What the
generated names found: `rt/restir_di.slang` declared binding 45 twice (its `gLayers` and `rt/lights.slang`'s
`emissiveFirst`; `gLayers` moved to 26); the deferred lighting layout declared and wrote bindings 3 and 4 the shader
does not have (deleted); the G-buffer pass built and wrote a descriptor set no shader read (deleted); the TAA push
carried 8 bytes the shader lacks; bloom's C++ struct names the downsample's `isFirstPass` slot `blendFactor`
(now asserted to coincide). The `rtc::layoutBinding`/`descriptorWrite` helpers are deleted.

## 2f. Scene writers

36 hand-written per-type writers repeat the registrations. Measure the generic reflection writer on `scene_bench`
bench10k against the save budget (10 ms). Within it: delete the hand writers. Over it: generate them at build from
the registrations. Either way the registration is the only definition.

*Done when* no hand writer remains and `scene_bench` is within budget.

## 2g. Layers checked at configure

`cmake/ohao_layers.cmake`: each library's layer (core, gpu, render, ecs, api, runtime, tools; game modules later)
and a configure-time check that its links point down only. `ohao_ecs` stops linking the renderer publicly where the
render systems move behind `installRendering`; where it cannot yet, the exception is listed with step 3 as its end.

*Done when* configure fails on an upward link (proven by planting one) and the listed exceptions are the only ones.

*As built (2026-11-09):* `cmake/ohao_layers.cmake` gives each library a level (core 0; gpu, reflect, level, anim,
audio bank, vfx, physics 1; asset, audio 2; renderer 3; ecs 4; level world 5; engine 6; api 7; rpc, runtime 8) and
checks every link and every `#include` against the library that owns the included header. The only exception is
`ohao_ecs -> ohao_renderer` (step 3). No other upward edge existed. Planted `#include "ecs/world.hpp"` in
`core/log.cpp`: configure failed naming the file and the edge.

## 2f result (2026-11-09)

Frank raised `save_pretty` to 11 ms. `saveScene` is now the generic reflection writer alone: the 36 per-type
writers (`ecs/scene_writers.*`, 677 lines), the layout tree they fed (`reflect/json_layout.*`) and the second save
path are deleted. Every checked-in scene (22) saves byte for byte as before (each saved by the old and the new build
and compared); bench10k `save_pretty` 10.32 ms (p95 10.40). A detour measured on the way: the old tree path with the
generic writer feeding it was 15.6 ms (it renders compact text and parses it back).
