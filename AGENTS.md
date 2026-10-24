# AGENTS.md — working on OHAO

The entry point for any agent (or person) building, testing or changing this repo.
Every command in a `console` block below is run by `python tests/docs/check_docs.py`
(`--slow` adds builds and long gates); if one stops working, this file is wrong.

OHAO is a C++20 game engine: a Vulkan 1.3 hybrid renderer (deferred raster with RT
shadows/GI, plus a path tracer), an EnTT world, reflection and canonical scene files, an
API for agents (`ohao/api`), the headless `ohao` CLI and `ohao serve`, the same API as a
JSON-RPC/MCP server on stdio (`ohao/rpc`). Windows/MSVC is the platform in daily use.

- **Code map, data flow, recipes:** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **Driving the engine from an agent:** [docs/agents/ohao-cli.md](docs/agents/ohao-cli.md)
  (the CLI, every method and error code) and [docs/agents/ohao-serve.md](docs/agents/ohao-serve.md)
  (a live session over MCP: connect, orient, edit, play, look, record and replay, logs)
- **Designs and plans:** `docs/specs/`, `docs/plans/`; **what changed:** `docs/changes/`;
  **where it is going:** [docs/ROADMAP.md](docs/ROADMAP.md)
- **Monograph** (design units with code citations): `site/`

## Build

Use the real CMake. On this machine conda's `cmake` shadows it on PATH, so name the scoop
shim. Build with `-j6` at most: heavier parallel load has crashed this machine.

```console
$ C:/Users/djmax/scoop/shims/cmake -B build -S .   # slow
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6   # slow
$ C:/Users/djmax/scoop/shims/cmake --build build --config Release --target shaders   # slow
```

- **Sources are globbed with `CONFIGURE_DEPENDS`**: a new `.cpp` or shader under a
  globbed directory is picked up by the next build, which re-runs the configure step
  itself. A new directory or target still needs a `CMakeLists.txt` edit.
- **The `shaders` target is not in `ALL`**, but every target that loads SPIR-V depends on
  it (the `ohao` CLI, the examples, the rendering tests), so building any of them builds
  the shaders. `python tools/check.py` builds it first regardless.
- Binaries land in `build/Release/`; SPIR-V in `build/Release/bin/shaders/`, named by
  flattening the path (`rt/pt_raygen.rgen` → `rt_pt_raygen.rgen.spv`).
- **New shaders are Slang** (`slangc` from the Vulkan SDK, pinned in
  `shaders/slangc.version`); a `.slang` with one `[shader(...)]` entry point gives
  `<flattened>.spv`, with several `<flattened>.<entry>.spv`, without one it is a module.
  Constants and structs shared with C++ live in `shaders/shared/layout.json`; regenerate
  their `.slang`/`.glsl`/`.hpp` with `python tools/gen_layout.py` (the check below fails
  until you do). See ARCHITECTURE.md, "add a render pass".

```console
$ python tools/gen_layout.py --check
```

## Gates

**One command builds and runs everything:**

```console
$ python tools/check.py   # slow
$ python tools/check.py --all   # slow
```

It finds the real CMake (refusing conda's), configures if needed, builds the `shaders`
target and then everything with at most 6 jobs, runs `ohao doctor` (below; a failing
environment stops the run there), runs every CTest test with the chosen labels, and
writes one verdict: `build/check/check.json`, also the last line it prints
(`{ok, labels, build, doctor, tests: [{name, labels, ok, seconds, passed, failed,
failures, tail}], counts}`), plus `build/check/junit.xml` and the logs beside them. A
failing test's entry carries the last 40 lines of its output. Exit 0 = all passed, 1 =
something failed, 2 = the environment is wrong (the message says how).

- **Labels** (one per test, `cmake/ohao_tests.cmake`): `quick` (CPU), `gpu`, `docs` — the
  default set — and `golden`, `bench`, `slow`, which `--all` adds. `--labels gpu,docs`
  picks your own set; `ctest --test-dir build -C Release -L quick` works without the
  script. GPU tests never run two at once; benchmarks run alone.
- **Registering a new suite:** `ohao_add_test(<name> LABEL <label> [GPU] COMMAND <exe>)`
  in its `CMakeLists.txt`, and end its `main` with `ohao::test::finish(...)` from
  `tests/common/harness.hpp` (or its `CHECK`/`RUN` macros), so the summary line is
  there. `check_tests` fails if a test executable is built but not registered.
- **The environment:** `ohao doctor` answers whether this machine can build, run and test
  OHAO, as `{"result": {ok, cpuOnly, checks: [{id, ok, level, detail, fix?}]}}`: the Vulkan
  instance, device, ray tracing and validation layer; every shader's SPIR-V present and
  newer than its source and every file it includes; the assets the checked-in scenes
  reference; glslc, slangc (at its pinned version), cmake (not conda's) and the harnesses'
  Python modules; and a 64x64
  deferred frame within 2 s (a wedged GPU after a crash fails here, not in every render
  test). Exit 1 when an error-level check fails; `--cpu-only` creates no device.

```console
$ build/Release/ohao doctor --cpu-only > /dev/null
```

Every suite exits 0 on success and nonzero on any failure; that is the pass criterion.
Every suite ends with one JSON line, `{"suite", "passed", "failed", "failures"}`. The
commands below run single suites by hand.

CPU suites (no GPU):

```console
$ build/Release/runtime_tests.exe > /dev/null
$ build/Release/log_tests.exe > /dev/null
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/api_tests.exe > /dev/null
$ build/Release/rpc_tests.exe > /dev/null
$ build/Release/systems_tests.exe > /dev/null
$ build/Release/physics_backend_tests.exe > /dev/null
$ build/Release/audio_system_tests.exe > /dev/null
$ build/Release/force_generator_tests.exe > /dev/null
$ build/Release/env_cdf_test.exe > /dev/null
$ build/Release/denoise_parse_test.exe > /dev/null
$ build/Release/sobol_test.exe > /dev/null
$ python tests/cli/check_cli.py --cpu-only > /dev/null
$ python tests/reflect/check_scene_schema.py > /dev/null
$ python tests/check/check_tests.py > /dev/null
$ python tests/golden/test_golden_diff.py > /dev/null
$ python tests/doctor/check_doctor.py > /dev/null
$ python tests/serve/check_serve.py > /dev/null
$ python tests/layout/check_layout.py > /dev/null
```

| Suite | Covers |
|---|---|
| `runtime_tests` | the runtime shell without a window: fixed-step clock, the timeline (lockstep, realtime, pause), the input map (actions, axes, the virtual device, key names), swapchain choices, capture images (the PNG writer round trips through stb_image, crop, the preview downscale, JPEG), `render.stats`' per-pass medians |
| `log_tests` | the logger: level filter, ring wrap and `dropped`, repeat collapse, channel filter, paging, concurrent writers, sinks, the 1 KB cut, the stderr format |
| `reflect_tests` | registry, schema export, JSON writer and reader, float formatting, the dependency rule, the source lints (nondeterminism, prints, literal light positions, float atomics in shaders) |
| `scene_tests` | engine registrations, ids, the scene codec, error corpus, migration, snapshots, instantiate (fake backend) |
| `api_tests` | methods, commands and inverses, queries, summaries, params structs, the path sandbox, cancellation, the time model (phases, `watch`, `E_STATE`, realtime vs lockstep), `world.hash` and `world.diff`, input (inject, state, record, replay), `render.stats` |
| `rpc_tests` | the protocol server in-process: framing, ordering, the error table, ids, cancellation, progress, ping while busy, MCP initialize/tools/logging, the queue cap; the named pipe (one local client, its DACL, a second client and a second server refused, remote clients refused against a control pipe) |
| `systems_tests` | engine-provided systems (spin, the fly controller) |
| `check_cli.py` | `ohao` CLI goldens: stdout, files and exit codes, each case run twice |
| `check_scene_schema.py` | the generated JSON Schemas in a stock validator (needs `jsonschema`) |
| `check_tests.py` | `tools/check.py` itself, against fixtures in `build/check_fixture`; every built test executable registered; `render_bench --evaluate` on fixture results (pending budgets reported, a gated miss named by pass, noisy runs never a pass) |
| `test_golden_diff.py` | the golden harness's diff report on synthetic images: statistics, regions, heatmap, crop, `--json` |
| `check_doctor.py` | `ohao doctor` on fake shader trees (missing, stale, includes, Slang entry points and imports) and on the real tree; `--gpu` adds the device checks |
| `check_layout.py` | `shaders/shared/layout.json`'s generated files current, std430 offsets asserted, bad layouts refused, the generated GLSL and Slang compiling |
| `check_serve.py` | `ohao serve` as a process: protocol goldens (`tests/serve/sessions` → `expected`, `--update`), every tool schema through `jsonschema`, stdout clean under stray prints, the sandbox and caps, `ohao serve --pipe` as a process |

GPU suites and harnesses (each creates a Vulkan device):

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/ecs_tests.exe > /dev/null   # gpu
$ build/Release/renderer_test.exe > /dev/null   # gpu
$ build/Release/instantiate_gpu_test.exe > /dev/null   # gpu
$ build/Release/scene_view_gpu_test.exe > /dev/null   # gpu
$ build/Release/restore_render_gpu_test.exe > /dev/null   # gpu
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ python tests/cli/check_cli.py > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ python tests/doctor/check_doctor.py --gpu > /dev/null   # gpu
$ OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200 > /dev/null   # gpu
$ python tests/serve/check_live.py > /dev/null   # gpu
$ python tests/serve/e2e_mcp.py > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/scene_bench.exe --check tests/bench/scene_budgets.json > /dev/null   # slow
$ build/Release/serve_bench.exe --check tests/bench/serve_budgets.json > /dev/null   # slow
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
$ python tests/docs/check_docs.py --cpu-only > /dev/null   # slow
```

- `engine_tests`: event bus, command history, assets, headless deferred and path-traced
  frames, RenderScene, per-pass GPU timing (every pass registered, timed once, in frame
  order, within the frame; a pass turned off leaves the timings). `ecs_tests`: world, hierarchy, transforms and the render extract
  against a real RenderScene. The `*_gpu_test`s: instantiate, scene view and
  snapshot/restore through the renderer; `deferred_determinism_gpu_test`: deferred frames,
  post effects included, bit-identical across fresh renderers, processes and repeated
  captures, and path-traced Cornell (offline and realtime) the same within the BVH floor
  (below). `check_live.py`: `ohao serve --window` (a hidden window, validation on) driven
  end to end: load, apply, step with watch, inject, captures (inline JPEG and PNG
  previews, a region, another size and a path-traced mode refused), log.read; a lockstep
  window capture equals a headless capture of the same world bit for bit (post off and
  on); minimized is `E_STATE`; closing stdin mid-step exits 0; validation and unrequested
  readbacks 0 through teardown; `helmet_game.scene.json` saves byte for byte and its fly
  camera moves 3 m in 60 injected steps; the window host over `--pipe` (a step, a capture,
  the client's disconnect ends it cleanly). `e2e_mcp.py`: a stdlib-only MCP client runs a
  whole session against a hidden window (load, edit, snapshot, record, a flight, step with
  watch, captures, logs, restore, replay to the same hash, a capture bit-identical to the
  first), validation clean through teardown.
- **Validation** (`OHAO_VALIDATION=1`): zero messages, teardown included. Each message
  is a record on the logger's `validation` channel, on stderr as
  `W validation: <VUID> | <text>` (`E` for errors; a run of identical ones ends with
  `... repeated N more times`). `check_validation.py` renders Cornell and the helmet,
  deferred and path traced (offline, realtime, realtime through NRD), under the layers;
  `--json FILE` gives each scene's message count, first VUID and per-VUID counts.
  `OHAO_VALIDATION=sync` adds the layer's synchronization validation (hazards between
  commands, as `SYNC-HAZARD-*` messages): slower, for chasing a missing barrier.
- **Render goldens** (`tests/golden/manifest.json`, 14 scenes: 6 examples, 8 through
  `ohao run`; three at the deferred frame's `reference` quality): `--only NAME`, `--update` after a deliberate change (look at what drifted
  first). `--json FILE` writes one object per scene (statistics, tolerance, up to 4
  regions where the image changed); a failing scene also gets
  `build/golden_diff/<name>.heat.png` (where and how much) and `<name>.crop.png`
  (expected | actual | heat of the worst region), `--heatmaps all` for every scene.
  `--selftest --json` records the run-to-run floor. They are a hash of an unchanged sampler, not an image-quality gate: anything
  that perturbs the sample sequence moves every pixel by the noise. Never widen a
  tolerance to make one pass. The deferred scenes are exact (max_abs 0): any difference
  there is a change. Path-traced scenes hold to a floor of 1 LSB on a few pixels in a
  minority of runs (tolerance 1 / 0.0005): the driver builds a mesh's BVH differently run
  to run, and a ray that hits exactly on a shared triangle edge takes whichever triangle
  the traversal reaches first.
- **Benchmarks** (`scene_bench --check`): budgets in `tests/bench/scene_budgets.json`;
  results in `build/scene_bench.json`. `serve_bench --check`: the protocol's budgets in
  `tests/bench/serve_budgets.json` (`ohao serve` as a child process on pipes, bench10k:
  round trips, transport overhead, apply, step, hash, diff, the logger's per-call costs, the
  `tools/list` size; then `ohao serve --window 1280x720 --hidden` on Cornell: a step until
  its frame is presented, a frame's readback, inline and PNG captures, the reader thread's
  CPU over 600 idle realtime steps; `--cpu-only` skips the window), results in
  `build/serve_bench.json`. `render_bench --check`: the renderer's GPU budgets in
  `tests/bench/render_budgets.json`, each tagged `gatedFrom` the 2b step that delivers it
  (gated once `currentStep` reaches it, else reported `pending` with its measurement);
  headless captures of `cornell_720p` and `helmet_game_720p`/`_1080p`, 5 warm-up and 100
  timed frames each, every pass's median and p95 from GPU timestamps, results in
  `build/render_bench.json` (the per-pass baseline every renderer change is judged by);
  `--only SCENE`, `--frames N`; `--evaluate RESULTS --check BUDGETS` judges recorded
  results without a GPU. Besides the CPU loop it times a GPU calibration kernel
  (`shaders/bench/gpu_calibrate.slang`) before and after, against
  `build/render_bench_reference.json` (`render_bench --calibrate`). `serve_bench` times a
  calibration loop before and after its blocks and records every other process that
  used CPU meanwhile; a run >25% slower than `build/serve_bench_reference.json` (written
  by `serve_bench --calibrate` on a quiet machine) or beside other processes using more
  than half a core in total is `noisy`, exit 2: it is neither a pass nor a regression,
  and `tools/check.py` reruns it, at most twice. `scene_bench` records the same, plus a
  CPU and a memory probe per round, and is also noisy when over a quarter of its rounds
  are contended (a probe over 1.5x its fastest round); it prints each op's median in
  clean and in contended rounds. This laptop slows a busy core by itself after about 3 s
  of sustained load (`scene_bench --machine-test` shows it), more with other bots' work
  running. `python tools/bench/wait_quiet.py` waits until other processes use under half a
  core for a minute (exit 1 at `--deadline`); run it before a bench whose verdict you need.
  A budget is met after five consecutive passes and changes only by Frank's ruling.
- **Docs**: `check_docs.py` runs every command in this file, ARCHITECTURE.md, the CLI
  and serve guides and the skills (`build`, `verify`, `add-component`: `.claude/skills/`, or
  `docs/agents/skills/` until they are installed there; see its README).

## Gotchas that have cost this project time

- conda's `cmake` shadows the real one; use the scoop shim (above). `tools/check.py`
  refuses conda's.
- Tests run in parallel under CTest: a test that writes a fixed path (a scratch
  directory under `build/`, a file in `%TEMP%`) races another copy of itself. Give it a
  per-process name or a `LOCKS` entry in its registration.
- **Git Bash rewrites arguments that start with `/`** into Windows paths. Write
  `ohao query --fields Transform/position`, not `/Transform/position`.
- **Line endings:** `core.autocrlf` is on. Byte-compared files (goldens, scene files,
  CLI expectations) are pinned to LF in `.gitattributes`; add a pattern there for any new
  byte-compared directory, or a fresh clone checks out CRLF and the comparison fails.
- `/FORCE:MULTIPLE` hides duplicate symbols: stb, and a second VMA inside `NRI_VK.lib`.
  Which copy wins depends on link order, so `gpu_allocator.cpp` uses NRI's VMA
  configuration; never change one without the other. A new target that crashes right
  after "resizing to WxH" has picked a mismatched VMA.
- Windows' `OPAQUE` macro (`wingdi.h`) collides with `MaterialData::AlphaMode::OPAQUE`:
  `#undef OPAQUE`.
- GLSL `flat` is a reserved qualifier; rename locals called `flat`.
- `GLM_ENABLE_EXPERIMENTAL` before any `<glm/gtx/...>` include.
- Jolt: `dynamic_cast` on Jolt types crashes (no RTTI); `SetMotionQuality` lives on
  `BodyInterface`. `PhysicsWorld(cfg)`, `stepOnce()` not `step(dt)`; bodies need a shape.
- Two `AABB` types with the same signatures: `ohao::AABB` is (min, max),
  `ohao::physics::math::AABB` is (center, halfExtents). Both compile; one is the wrong box.
- ECS writes: `Transform`, `MeshRenderer`, `Light`, `EmissiveProxyLight` and
  `GiMaterialLegacy` reach the renderer only through `registry.patch<T>()` /
  `replace<T>()` (or emplace/remove). A write through `get<T>()` is never seen.
- World systems have a phase: `addSimulate` (behaviour, once per fixed step, gets dt) or
  `addSync` (derived state, no dt). Before a `render()` call `world.sync()`, never
  `update()`: rendering must not step the simulation.
- Input is stepped per simulation step, not per frame: read `pressed`/`released` in a
  simulate system or `fixedUpdate`, never once per frame (two steps in one frame would
  lose a press). A world with an `InputActions` component rebuilds its bindings from it;
  one without keeps the bindings made in code.
- Descriptor sets written at record time need one set per frame in flight
  (`MAX_FRAMES_IN_FLIGHT` = 3), or a frame rewrites the set the previous one reads (VUID 03047).
- Readback has one form: `requestReadback()` before the `render()` you want,
  `readback()` after (that frame's fence only; a path-traced frame under OIDN or NRD is
  denoised there). No other frame copies anything, so `readback()` after an unrequested
  frame is empty. Buffers the CPU reads must be host-cached memory: uncached
  host-visible memory read one 1280x720 frame in ~20 ms.
- A deferred capture renders 1 frame by default (10 with TAA), but the hybrid's RT GI
  keeps a history: frame 1 and frame 10 of Cornell differ (max_abs 29). Pass `frames` for
  a converged image; the deferred goldens state theirs (`--frames 7`, `--frames 27`).
- RT shadows are compute with inline ray queries (`VK_KHR_ray_query`; without it the
  frame keeps CSM, which renders only when RT shadows do not). They keep a history too: the
  filter averages the frames since the last reset, so frame 1 is the noisiest. Every
  shadow-casting raster light (up to 8) owns a visibility channel (`params.w`), but a pixel
  traces only its 4 most important; `render.stats`' `shadowLightsDropped` counts the pixels
  that had more.
- The validation layer stops at 10 reports per VUID; set
  `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` to see them all. The engine names no Vulkan
  objects: find a handle's creation site by temporarily printing handles at `vkCreate*`.
- Restoring a file from a backup copy keeps the backup's older mtime, so MSBuild keeps
  the objects built from the edited version: `touch` sources after a restore.
- `build/ohao` is the CMake binary directory of `ohao/`, not the CLI; the CLI is
  `build/Release/ohao.exe`.
- After a crash the GPU can stay wedged (device lost `-4` everywhere, 30 s renders of
  Cornell). It clears on reboot; it is not your change. `ohao doctor`'s `gpu.wedged`
  check says so.
- Monograph: run its tests and `check_unit.py` from the repo root, never from `site/`
  (that directory shadows the stdlib `site` module and breaks `_ctypes`). The site build
  is the one exception: it imports `tools.monograph`, so it runs from `site/`.
  `build.py` has no `__main__`, so `python -m tools.monograph.build` does nothing.
  Removing a unit takes three edits: the `.md`, its `page()` entry in
  `site/tools/monograph/tree/<module>.py`, and its `author_rich.py` block, or the stale
  page keeps being served.

```console
$ python site/tools/check_unit.py reflection/api > /dev/null
$ python site/tests/test_monograph_structure.py > /dev/null 2>&1
$ cd site && python -c "from tools.monograph.build import build; build()" > /dev/null   # slow
```

## Conventions

- **Spec → plan → build.** A sub-project gets a design in `docs/specs/`, an
  implementation plan in `docs/plans/`, then a build in the plan's steps, each ending
  with the engine running and every gate green. If the spec turns out wrong, fix the spec
  in the same commit and say so.
- **A change record per sub-project (or build step)** in
  `docs/changes/YYYY-MM-DD-<subproject>[-stepN].md`, written for the project wiki. Shape:
  a title; `**Spec:**` (path and sections) and `**Status:**` lines; then numbered
  sections — summary, what was built, bugs found and fixed, spec corrections, results
  (test counts, benchmark medians/p95, golden max_abs), known gaps, how to run the new
  tests. Facts and numbers only, from runs, never estimates.
- **Commits:** local, one or a few per step, subject `type(scope): what` (`feat`, `fix`,
  `perf`, `docs`, `test`), body saying why. Never push, merge, rebase or reset without
  the owner's say-so.
- **Forward only.** Old code and docs are reference, not something to preserve: delete
  what is dead, and describe the tree as it is, not its history.
- **Engine code logs through `OHAO_LOG(level, channel, fmt, args...)`** (`core/log.hpp`),
  never `std::cout`/`std::cerr`/`printf`; `reflect_tests` fails on a print under `ohao/`.
  Per-item and per-frame detail is `Debug` (shown with `OHAO_LOG_LEVEL=debug`).
- **Performance is a gate.** New hot paths come with a benchmark or a budget; reflection
  never runs per frame (the ECS system sources may not include `reflect/` or `api/`;
  `reflect_tests` enforces it).
- **Tests are named for what they prove** and, where a spec numbers its tests, carry the
  number (`t21_…`). A golden is updated only after looking at what changed.
