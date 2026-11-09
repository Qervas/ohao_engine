# AGENTS.md — working on OHAO

The entry point for any agent (or person) building, testing or changing this repo.

OHAO is a C++20 game engine: a Vulkan 1.3 hybrid renderer (deferred raster with ray-traced
direct light (ReSTIR DI) and GI, plus a path tracer), an EnTT world, reflection and canonical scene files, an
API for agents (`ohao/api`), the headless `ohao` CLI and `ohao serve`, the same API as a
JSON-RPC/MCP server on stdio (`ohao/rpc`). Fedora 45 (KDE, GCC 16, Ninja) on an RTX 4060 laptop is the
platform in daily use since 2026-11-08 (Omarchy/Arch before, from 2026-11-05); Windows/MSVC builds were not
tried since 2026-11-05.

- **How work is done:** Musk's algorithm, in order (Frank, 2026-11-08): question the requirement, delete the
  part or process, simplify, speed up, automate. Nothing is added (a test, a feature, a doc section) without
  the requirement it serves.
- **Code map, data flow, recipes:** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **Driving the engine from an agent:** [docs/agents/ohao-cli.md](docs/agents/ohao-cli.md)
  (the CLI, every method and error code) and [docs/agents/ohao-serve.md](docs/agents/ohao-serve.md)
  (a live session over MCP: connect, orient, edit, play, look, record and replay, logs)
- **Designs and plans:** `docs/specs/`, `docs/plans/`; **what changed:** `docs/changes/`;
  **where it is going:** [docs/ROADMAP.md](docs/ROADMAP.md)
- **Monograph** (design units with code citations): `site/`, frozen during the frontier renderer work.

## Build

Fedora's packages (RPM Fusion's `akmod-nvidia` for the driver):

```
sudo dnf install gcc-c++ cmake ninja-build vulkan-headers vulkan-loader-devel vulkan-validation-layers spirv-tools \
  tbb-devel wayland-devel wayland-protocols-devel libxkbcommon-devel libX11-devel libXrandr-devel \
  libXinerama-devel libXcursor-devel libXi-devel libXext-devel
```

Two pinned slangc, each unpacked from Slang's own release into `~/.local/opt/slang-<version>`: 2026.20 for SPIR-V
(`shaders/slangc.version`, `SLANGC`) and 2025.11 for the CPU twins' C++ (`shaders/slangc_cpp.version`,
`SLANGC_CPP`; 2026.20's C++ emitter miscompiles them, "Gotchas"). Python (numpy, Pillow, jsonschema) is a uv
environment in the repo, `.venv`; name its interpreter at configure, or CTest runs the harnesses with the
system Python, which has none of them. The Ninja Multi-Config generator keeps the `build/Release/` layout every
harness reads.

```
cmake -B build -S . -G "Ninja Multi-Config" -DSLANGC=$HOME/.local/opt/slang-2026.20/bin/slangc \
  -DSLANGC_CPP=$HOME/.local/opt/slang-2025.11/bin/slangc -DPython3_EXECUTABLE=$PWD/.venv/bin/python
cmake --build build --config Release -j16
git config core.hooksPath .githooks      # the light CI, once per clone ("Gates")
```

- **Sources are globbed with `CONFIGURE_DEPENDS`**: a new `.cpp` or shader under a globbed directory is picked up
  by the next build. A new directory or target still needs a `CMakeLists.txt` edit.
- **The edit loop is seconds** (2026-11-09): an unchanged tree builds in 0.06 s, a shader edit in about 1 s (CMake
  reconfigures, 0.6 s, for the entry points; only that shader and those importing it are recompiled, from slangc's
  `-depfile`), a renderer `.cpp` in about 2 s (mostly relinking 14 programs). The dependencies are re-checked only
  when their declarations change (`OHAO_DEPENDENCIES_POPULATED`), and NRD's config header keeps its time across
  configures, else every configure recompiles NRD and relinks everything.
- **The `shaders` target is not in `ALL`**, but every target that loads SPIR-V depends on it, so building any of
  them builds the shaders. Binaries land in `build/Release/`; SPIR-V in `build/Release/bin/shaders/`.
- **Every shader is Slang** (no GLSL, no glslc). A `.slang` with one `[shader(...)]` entry point gives
  `<flattened path>.spv` (`postprocess/tonemap.slang` → `postprocess_tonemap.slang.spv`), with several
  `<flattened path>.<entry>.spv`, without one it is a module (`import shared.layout;`). Constants and structs
  shared with C++ live in `shaders/shared/layout.json`; regenerate their `.slang`/`.hpp` with
  `python tools/gen_layout.py` (`--check` says whether they are current). **Bindings come from the shaders:**
  the shader build writes `ohao/render/shader_bindings/<shader>.hpp` (checked in, to be read) from the reflection
  each entry's compile saves (`tools/gen_bindings.py`: each binding named, each set in order, the push constants),
  only the headers whose text changed; C++ names them (`shader::write`, `shader::layoutBindings`) and never types a
  binding number, and a hand-written push struct carries `OHAO_CHECK_PUSH`. `tools/check.py` fails the build on a
  typed number. See ARCHITECTURE.md, "add a render pass".
- **`GLFW_INCLUDE_NONE`** before GLFW's header (`ohao/runtime/window.cpp`): without it GLFW includes `GL/gl.h`,
  which Fedora does not ship by default. The engine is Vulkan only.

## Gates

The tests serve five requirements, and nothing else is a gate (Frank, 2026-11-08; the cut from 74 tests to these
is commit `7e7211cf`). A test is added back only when a real bug got through, never per feature or per spec step.

| Requirement | Gate | Label | Time |
|---|---|---|---|
| 1. Never hurt the machine | every test has a time limit; `tools/check.py` fails a run during which the kernel logged an NVIDIA GPU fault (Xid); `check_validation`: the Vulkan validation layer over Cornell, the helmet, the animation rig, the play scene, caustics and smoke, deferred and path traced, and the teardown | gpu | 2 s |
| 2. The renderer is right | `check_analytic` (both renderers against `albedo/π × irradiance` under one light; an emissive quad's form factor), `check_env_orientation` (a sky the right way up), `check_material` (the white furnace renders 1, the closed emitting box its closed form) | gpu | 6 s |
| 3. No visual accident | `render_golden`: 5 scenes (Cornell deferred and path traced, the Damaged Helmet, the play scene, glass), rendered at 640x360 in one `ohao serve` | golden | 1 s |
| 4. Gameplay replays exactly | `replay_test`: the play scene's fight restored from a midway snapshot, and in a new process, reaches the same `world.hash` at every checkpoint (headless, no GPU) | quick | 0.1 s |
| 5. It builds | `tools/check.py` builds first, then runs `ohao doctor` | | |

**Every gate runs in seconds** (Frank, 2026-11-09): a GPU check is one `ohao serve` process
(`tests/common/ohao_serve.py`: one device, the programs compiled once, scenes loaded in turn) at the fewest pixels
and samples that hold its bars. A check that launches a process per render, or renders more than its measure needs,
is a bug. **While iterating, run `--changed`**: it runs what the changed files reach (an ECS file: `replay_test`,
0.1 s); the whole set only to close a step.

```
python tools/check.py --changed [REF]    # only what the files changed since REF reach (tools/check_changed.py)
python tools/check.py                    # build, ohao doctor, every gate (quick, gpu, golden): about 10 s in all
```

**Light CI, on this machine** (Frank, 2026-11-09): `.githooks/pre-commit` runs `tools/check.py --staged` (what the
commit's files reach; a docs commit builds and runs nothing) and `.githooks/pre-push` the whole check; a failure
blocks the commit or the push. Never `--no-verify`. Two checks never overlap: a second waits on `build/check.lock`.

`check.py` finds the real CMake, builds the `shaders` target and then everything (at most 6 jobs), runs
`ohao doctor` (an error-level failure stops the run), runs CTest with the chosen labels and writes one verdict,
`build/check/check.json`, also its last printed line: `{ok, labels, build, doctor, tests: [{name, labels, ok,
seconds, passed, failed, failures, tail}], counts, gpuFaults}`. A failing test carries the last 40 lines of its
output. Exit 0 = all passed, 1 = something failed (a GPU fault included), 2 = the environment is wrong.

- **Every test exits 0 on success** and nonzero on any failure, and ends with one JSON line `{"suite", "passed",
  "failed", "failures"}`. Register a new one with `ohao_add_python_test`/`ohao_add_test` (`cmake/ohao_tests.cmake`),
  with a `TIMEOUT` and, if it touches the GPU, `GPU` (GPU tests never run two at once).
- **A kept gate is proven, not trusted:** plant a bug in what it covers and watch it fail. 2026-11-09, after the gates
  became one process each: 64x sampler anisotropy (`check_validation`, the first case: the sampler is made once), a
  rough metal's multiple scattering x0.9 (`check_material`, furnace 0.934) and, with the goldens at 640x360, 1% brightness
  in `displayPixel` (all 5 goldens, 75 to 84% of the pixels), each caught. 2026-11-08, each caught:
  a no-op physics restore (`replay_test`, all 3 checkpoints); 1% brightness in `displayPixel` (all 5 goldens);
  the path tracer's sky upside down (`check_env_orientation`); +8% on a light sample's geometry term
  (`check_analytic`, deferred x1.10); a rough metal's multiple scattering x0.9 (`check_material`, furnace 0.93);
  64x sampler anisotropy (`check_validation`, all 20 renders); a planted Xid line (`check.py` fails the run). A
  dielectric's multiple-scattering Fresnel x0.9 passed the furnace: under a white base the energy moves to the
  diffuse and the total stays 1, which is what a furnace measures.
- **`ohao doctor`** says whether this machine can build, run and test OHAO: the Vulkan instance, device, ray
  tracing and validation layer; every shader's SPIR-V present and newer than its sources; the assets the scenes
  reference; slangc at its pinned version; a 64x64 deferred frame within 2 s (a wedged GPU fails here). `--cpu-only`
  creates no device.
- **Never loop a program that opens a window** (Frank, 2026-11-08): one visible run shows it works; repeats are
  headless (`ohao run`, `ohao serve` without `--window`).
- **Validation** (`OHAO_VALIDATION=1`): zero messages, teardown included, each on the logger's `validation` channel
  (`W validation: <VUID> | <text>`). `OHAO_VALIDATION=sync` adds synchronization validation (hazards between
  commands): slower, for chasing a missing barrier. The layer stops at 10 reports per VUID;
  `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` shows them all.
- **Goldens** are a hash of an unchanged renderer, not an image-quality gate: any change to the sample sequence
  moves every pixel by the noise. Deferred scenes are exact; the path tracer holds to the BVH floor (1 level on a
  few pixels). `--update` only after looking at what drifted (`build/golden_diff/<name>.heat.png` and
  `.crop.png`); never widen a tolerance to make one pass.
- **Benchmarks** (`tools/bench/`: `render_bench`, `scene_bench`, `serve_bench`, `physics_bench`, `level_bench`,
  `audio_bench`, `feel_bench`) are tools run by hand, not gates: `<bench> --check tests/bench/<name>_budgets.json`,
  results in `build/<bench>.json`. `render_bench --only SCENE` times one scene's passes from GPU timestamps; it is
  the baseline a renderer change is judged by. A budget changes only by Frank's ruling.

## Rules that hold everywhere

What every change must respect. The build checks some of these; the rest are here because no code can hold them.

- Tests run in parallel under CTest: a test that writes a fixed path (a scratch
  directory under `build/`, a file in `%TEMP%`) races another copy of itself. Give it a
  per-process name or a `LOCKS` entry in its registration.
- ECS writes: `Transform`, `MeshRenderer` and `Light` reach the
  renderer only through `registry.patch<T>()` /
  `replace<T>()` (or emplace/remove). A write through `get<T>()` is never seen.
- **Systems are installed by `engine::installEngine` and `engine::installRendering`** (`ohao/engine/install.hpp`) in
  every host, and run in the order of `ecs/system_order.hpp`, whoever installs them; a new system adds its name
  there (an unlisted name throws). A system with state a snapshot must carry registers `ecs::addSnapshotHook` under a
  `kSnapshotOrder` name.
- World systems have a phase: `addSimulate` (behaviour, once per fixed step, gets dt) or
  `addSync` (derived state, no dt). Before a `render()` call `world.sync()`, never
  `update()`: rendering must not step the simulation.
- Input is stepped per simulation step, not per frame: read `pressed`/`released` in a
  simulate system or `fixedUpdate`, never once per frame (two steps in one frame would
  lose a press). A world with an `InputActions` component rebuilds its bindings from it;
  one without keeps the bindings made in code.
- **A GPU fault is a report and an exit** (system design step 1, `gpu/vulkan/gpu_fault.*`): every submit, frame fence,
  acquire and present goes through it; a lost device, or a fence not signalled within 2 s (`kGpuTimeout`, Windows' TDR
  delay), logs one `GPU fault: {...}` line on the `gpu` channel (what each frame in flight ran, from breadcrumbs the
  pass timer writes with `VK_AMD_buffer_marker`; `VK_EXT_device_fault`'s report) and exits 5 with no Vulkan teardown.
  A new submit uses `gpu::submit` or `gpu::submitAndWait`, never `vkQueueSubmit` + `vkQueueWaitIdle`; a new pass is
  timed (`GpuPassTimer::begin`/`end`), or a fault inside it is reported as its neighbour's. A single submit that
  legitimately needs over 2 s is an unbounded dispatch: split it. `OHAO_INJECT_GPU_FAULT=lost:N` or `hang:N` fakes one
  at frame N without touching the GPU. Windows are capped at the display's refresh rate (`--fps N`, 0 uncapped; fifo
  is paced by the display).
- **Never ship a dispatch whose work is unbounded**: a GPU dispatch is not preempted, and a few lanes walking for
  seconds hold their warps (two firmware crashes came during such frames). Split work that varies by orders of
  magnitude into bounded rounds.
- **Every call site is compiled whole**: Slang inlines every call for SPIR-V, so a function reached from k sites is
  compiled k times and the driver's cold compile grows faster than the code (the path tracer once took 11 minutes).
  Never give an OpenPBR entry a second call site in a kernel (`docs/notes/renderer.md`, "Every call site").
- **The display transform** (`shaders/display/display.slang`) is the only way a frame becomes pixels; never write a
  curve or `pow(1/2.2)` again. **An equirectangular map's row 0 is the zenith**: a new lookup writes
  `v = 0.5 - asin(y) / pi` (`check_env_orientation` holds it).
- **Deferred and the path tracer show the same scene**: a change to one renderer's light or material is a change to
  both, judged against the closed forms and then the path tracer (`docs/notes/renderer.md`, "The referee").
- **Frank's assets stay local**: `tools/level/stage_flat42.py` stages them into git-ignored `assets/local/`; never
  commit them, `external/DLSS` or anything NVIDIA licenses, and never write under `C:\Users\djmax\Desktop\games` or
  the Unreal project.

## Where the rest is

- **The renderer** (`ohao/render/`, `shaders/`): [docs/notes/renderer.md](docs/notes/renderer.md): alpha, the
  G-buffer, glass, subsurface, OpenPBR's layers, the path tracer and its costs, caustics, path guiding, emission,
  the radiance cache, RT GI, RT direct light, lights, the sky, exposure, denoisers. Read the parts you touch first.
- **The simulation** (`ohao/ecs/`, `ohao/level/`, `ohao/anim/`, `ohao/audio/`, `ohao/vfx/`, `ohao/runtime/`):
  [docs/notes/simulation.md](docs/notes/simulation.md): levels, physics in the step, combat, frame timing,
  animation, audio, VFX.
- **Platforms and the machine**: [docs/notes/platform.md](docs/notes/platform.md): Windows (conda's cmake, Git
  Bash, line endings, duplicate symbols), Linux (the port's findings), a wedged GPU and the GSP firmware faults, the
  frozen monograph.

## Conventions

- **Spec → plan → build.** A sub-project gets a design in `docs/specs/`, an
  implementation plan in `docs/plans/`, then a build in the plan's steps, each ending
  with the engine running and every gate green. If the spec turns out wrong, fix the spec
  in the same commit and say so.
- **A change record per sub-project (or build step)** in
  `docs/changes/YYYY-MM-DD-<subproject>[-stepN].md`, written for the project wiki. Shape:
  a title; `**Spec:**` (path and sections) and `**Status:**` lines; then numbered
  sections — summary, what was built, bugs found and fixed, spec corrections, results
  (measurements, benchmark medians/p95, golden max_abs), known gaps. Facts and numbers only,
  from runs, never estimates.
- **Commits:** local, one or a few per step, subject `type(scope): what` (`feat`, `fix`,
  `perf`, `docs`, `test`), body saying why; the pre-commit hook gates each one. Never push, merge, rebase or reset without
  the owner's say-so.
- **Forward only.** Old code and docs are reference, not something to preserve: delete
  what is dead, and describe the tree as it is, not its history.
- **Engine code logs through `OHAO_LOG(level, channel, fmt, args...)`** (`core/log.hpp`),
  never `std::cout`/`std::cerr`/`printf` (stdout is the protocol's in `ohao serve`).
  Per-item and per-frame detail is `Debug` (shown with `OHAO_LOG_LEVEL=debug`).
- **Performance is measured, not assumed:** a new hot path is timed with the benchmark tools by hand
  (`render_bench --only SCENE` for the renderer). Reflection never runs per frame (the ECS system sources
  do not include `reflect/` or `api/`).
- **No test per feature or per spec step.** A step proves itself by measurement and the five gates; a new
  gate needs a requirement it serves or a bug that got through, and a planted bug it catches. A golden is
  updated only after looking at what changed.
