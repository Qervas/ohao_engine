# OHAO architecture

A map of the code: the libraries and which way they depend, how one frame flows from
game state to pixels, and recipes for the changes people make most often. Each recipe
was followed once, end to end, when it was written (sub-project 3, step 10: the `Spin`
component, the spin system, `entity.duplicate` and `systems_tests` are the result).
`python tests/docs/check_docs.py` runs every `console` command here.

## Libraries

All under `ohao/`, each a static library with its own `CMakeLists.txt`. Sources are
globbed with `CONFIGURE_DEPENDS`: a new file is picked up by the next build.

| Library | Directory | Depends on | What it owns |
|---|---|---|---|
| `ohao_core` | `core/` | glm | the logger (`OHAO_LOG`, the ring, sinks), the time model (`Timeline`, `FixedStepClock`: lockstep or realtime, paused, the frame number), input (`InputMap`: actions, axes, the virtual device injection uses; the `Key` name table), the event bus, command history, `Result`, concepts, small shared types |
| `ohao_reflect` | `reflect/` | glm (fast_float privately) | the type registry, schema/catalogue export, the JSON writer, reader and layout, typed decode/encode. Knows nothing of ECS or GPU. |
| `ohao_scene` | `scene/` | assimp, tinygltf, ufbx | asset loading into CPU `Model`s, procedural meshes |
| `ohao_gpu_vulkan` | `gpu/vulkan/` | `ohao_scene`, `ohao_core`, Vulkan, VMA | device and queues, `VulkanRenderer` (the facade), frame resources, bindless textures, the allocator |
| `ohao_renderer` | `render/` | `ohao_gpu_vulkan`, `ohao_scene`, `ohao_core` (NRD, OIDN, NGX optional) | `RenderScene` (the persistent GPU scene), deferred passes, path tracers, denoisers, the render graph |
| `ohao_ecs` | `ecs/` | `ohao_core`, `ohao_renderer`, `ohao_scene`, `ohao_reflect`, EnTT (simdjson privately) | `World` (simulate and sync phases, its `InputMap` bound from `InputActions`), components, systems (spin, fly controller, transform, instantiate, render extract), the scene codec, snapshots, the world hash and diff, the scene view |
| `ohao_api` | `api/` | `ohao_ecs`, `ohao_reflect` | `Session`: the method table and its params structs, edit commands, queries, the path sandbox. GPU-free. |
| `ohao_rpc` | `rpc/` | `ohao_api`, `ohao_core` | JSON-RPC 2.0 over newline-delimited streams and the MCP adapter (`ohao serve`): reader and engine threads, cancellation, progress, the log sink. CPU-only: no Vulkan, GLFW or renderer header (`reflect_tests`). |
| `ohao_runtime` | `runtime/` | `ohao_core`, `ohao_ecs`, `ohao_gpu_vulkan`, glfw | the windowed `Application` loop (stepped by a `Timeline`), frame statistics, input actions, swapchain |
| `ohao_physics`, `ohao_audio` | `physics/`, `audio/` | `ohao_core`; Jolt / miniaudio | physics and audio, not yet driven by the ECS |
| `ohao_host` | `tools/ohao/` | `ohao_api`, `ohao_ecs`, `ohao_renderer` | `RenderHost`, the GPU side of `render.capture` |

Programs: the examples (`examples/`: `cornell_box`, `model_viewer`, `turntable`,
`env_demo`, `ohao_game`), the CLI (`tools/ohao`), `scene_bench` and `serve_bench`
(`tools/bench`; they share the bench10k builder, `bench_world.hpp`, and `serve_bench`'s
calibration and load record, `bench_env.hpp`).

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
- **The API is GPU-free.** Anything that needs a renderer is a host handler
  (`Session::setRenderHandler`), installed by a program that owns one.
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
for each step:
  World::stepInput(events)   device events since the last step, then injected ones
                             (api: Session::step; InputActions rebinds the map first)
  host / game hooks (fixedUpdate)                       write components with patch/replace
  World::simulate(fixedDt)
    spin            engine-provided: Spin -> Transform (patch)        every World
    fly_controller  engine-provided: input + FlyController -> Transform   every World
once per batch of steps and before each render:
World::sync()
  instantiate     MeshSource/MaterialOverride -> MeshRenderer,        installed by a host with
                  meshes and materials registered in the RenderScene   a renderer (addSyncBefore)
  transform       Transform + Parent -> WorldTransform; tags RenderDirty, MovedLastFrame
  render_extract  MeshRenderer/Light -> RenderScene instance slots    installed by a host with
                  and the light list                                    a renderer (addSyncAfter)
host: applySceneView (Camera, Environment -> renderer camera, HDRI)   tools/ohao/scene_host.hpp
VulkanRenderer::render()
  RenderScene::recordUpdates   staged copies of changed table rows; TLAS build or refit
  deferred passes or a path tracer, then present (window) or readback (headless)
```

- **Change tracking is by signal.** `patch`/`replace`/`emplace` fire EnTT signals that
  set `TransformDirty`, `InstantiateDirty` and `RenderDirty`; each system works only on
  what is tagged, and per-frame upload cost follows what changed (`RenderScene::stats()`).
- **Simulate systems** (today: spin) change the world, once per fixed step, with the
  fixed dt. **Sync systems** derive state from it, with no dt: rendering a frame calls
  `World::sync()` only, so it never advances the simulation, and a realtime run and a
  lockstep run of the same steps reach the same state (the same `world.hash`).
  `World::update(dt)` is one `simulate(dt)` and one `sync()`, for tests and tools.
- **The fixed `dt` is the only clock** a system may read. `world.step` in the API,
  `ohao run --steps` and `Application`'s timeline pass it.
- **Teardown order:** the `World` before the renderer. Its systems release their
  `RenderScene` slots when it is destroyed.

Scene files, snapshots and the API sit beside the frame, not in it: load/save
(`ecs/scene_io`), snapshot/restore (`ecs/snapshot`) and commands (`api/commands`)
change components through the same signals, and the next update carries them through.

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

A new **method** instead:
1. **Its params struct** in `ohao/api/params.hpp`, registered in `params.cpp` with a doc
   on every field, limits, units and `.required()` where a member must be given. Free
   JSON that another decoder checks is a `reflect::JsonRef`. This one definition is the
   validation (`decodeParams`), the catalogue's JSON Schema and the MCP tool's input
   schema.
2. **The handler** `bool name(Session::Impl&, const JsonDocument&, const JsonNode& params,
   DecodeContext&, JsonWriter& out)` in `ohao/api/session.cpp`: first
   `decodeParams(s, params, ctx, p)`; a rule on the params alone fails with
   `badParams(s)` (JSON-RPC -32602), anything else is a domain error. A path in params
   goes through `usePath` (the server's sandbox). A long loop checks
   `s.cancelRequested()` between whole steps and reports `s.control->progress`.
3. **Register it:** a `MethodDoc` in `buildMethods` and the handler in `kDispatch`.
4. Then update the method count in `t09_method_table_dispatch_and_errors`, the CLI and
   serve goldens (the catalogue and `tools/list` list methods; `schemaHash` changes), an
   example in `EXAMPLES` of `tests/serve/check_serve.py`, and the method table in the
   CLI guide. `rpc_tests` keeps `tools/list` under 24 KB.

```console
$ build/Release/api_tests.exe > /dev/null
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
