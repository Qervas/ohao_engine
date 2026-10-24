# Sub-project 2b, step 1: profile and gate

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Profiling first: per-pass timestamps and `render_bench`", "The agent-facing surface" (`render.stats`), "Performance budgets (summary)", "Test plan" tests 1, 2 and 14 (stats part), "Build order" step 1, as built)
**Status:** Built. Every functional test passes: `tools/check.py` quick + gpu 32/32 and docs 2/2, render goldens 11/11 with the five deferred scenes exact, validation clean. `render_bench` has a clean run: it is the baseline below, and step 1 gates no budget. `serve_bench` was noisy twice (Unreal Editor at 182–190% of a core) and is reported as noisy. No budget or tolerance was changed.

## 1. Summary

Every pass of every frame now has a name, a GPU time and (once its step is done) a budget. A per-frame-slot timestamp timer replaces the deferred renderer's old timer, which shared one query pool across three frames in flight. Every graph pass and every direct pass is timed under a name from one registry. `render.stats` gives agents the per-pass medians over MCP. `render_bench` measures fixed scenes against `tests/bench/render_budgets.json`, where every budget in the spec is tagged with the step that delivers it. The baseline below is what every later step is judged against: Cornell at 720p takes 4.87 ms of GPU time against its 1.50 ms budget, and RT GI is 3.95 ms (81%) of it.

## 2. What was built

- **The pass registry** (`ohao/render/frame/pass_registry.hpp`): 19 passes in frame order, each with its kind and a line of what it does: `scene.upload`, `csm`, `gbuffer`, `ssao`, `rt.shadows`, `rt.gi`, `lighting`, `sss`, `ssr`, `sky`, `particles`, `post.bloom`, `post.taa`, `post.tonemap`, `gizmo`, `present`, `readback`, `pt.render`, `pt.readback`. `passIndex(name)` is `constexpr`.
- **`GpuPassTimer`** (`ohao/render/frame/gpu_pass_timer.{hpp,cpp}`):
  - One timestamp pool per frame slot, 64 passes each.
  - Begin and end both at `BOTTOM_OF_PIPE`, so passes never overlap and their sum is at most the frame's span.
  - Ticks are masked to the queue family's `timestampValidBits`.
  - A slot's results are read when the slot comes round again, after the renderer's fence wait, so timing never stalls a frame.
  - Frames are kept in a 120-frame ring. `collectAll(fenceSignalled)` reads every finished slot, oldest first.
  - A name outside the registry is not timed and is logged once as an error.
- **Wiring:**
  - `VulkanRenderer` owns the timer (`frameTimings(n)`; `gpuFrameMs()` now reads it).
  - `renderDeferred` and `renderRTPipeline` time `scene.upload`, `present`/`readback`, `pt.render` and `pt.readback`, and the frame's CPU record + submit time (`cpuMs`).
  - `RenderGraph::execute` times each graph pass under its `addPass` name. The deferred graph passes are renamed `csm`, `gbuffer`, `ssao`, `rt.shadows`, `rt.gi`, `lighting`.
  - `DeferredRenderer` times `sss`, `ssr`, `sky`, `particles` and `gizmo` directly. `PostProcessingPipeline` times `post.bloom`, `post.taa` and `post.tonemap` inside their enabled branches.
  - Removed: the old timer, `getPerfStats()` and `getPipelineInfo()` (neither had a caller).
- **`render.stats`** `{frames = 1 (1–120), passes = true}` → `{frame, frames, gpuMs, cpuMs, passes?: [[name, ms], ...]}`:
  - Values are medians, rounded to the microsecond; a pass's median is over the frames that ran it.
  - `api::Host::renderStats` (default: nothing rendered, `E_STATE`); no host is `E_COMMAND`. The method is not mutating.
  - `runtime::summarizeFrames` and `runtime::renderStats` serve both the headless `RenderHost` and the window `LiveHost`.
  - Over MCP it is `render_stats`; `tools/list` is now 23 tools, 15,150 B.
- **`render_bench`** (`tools/bench/render_bench.cpp`, CTest `bench`, GPU, serial):
  - Headless captures through `host::RenderHost`, the path `ohao run` takes: `cornell_720p` (hybrid, post off), `helmet_game_720p` and `helmet_game_1080p` (TAA, SSAO, bloom, exposure 1). Each scene gets 5 warm-up and 100 timed frames.
  - Reported per scene: `frame` (the rendering, without the final copy out), `span`, `cpu`, and every pass's median and p95.
  - Budgets measure the frame, the CPU, a CPU ratio, or a group of passes (`x.*` patterns).
  - Outputs: `build/render_bench.json` and the shared summary line.
  - Options: `--only SCENE`, `--frames N`, `--calibrate`, and `--evaluate RESULTS --check BUDGETS`, which judges recorded results without a GPU.
- **The GPU calibration kernel** (`shaders/bench/gpu_calibrate.comp`, `tools/bench/gpu_calibration.hpp`):
  - An integer hash over 1 MiB, 8,192 rounds per element.
  - Timed best of five after two warm-ups, before and after the frames.
  - Compared with `build/render_bench_reference.json`; more than 25% slower is `noisy`, like the CPU loop and the load record (`bench_env.hpp`).
- **`tests/bench/render_budgets.json`**: every budget of the spec's per-pass table and summary, each with `gatedFrom`, and `currentStep: 1`.
- **`tools/bench/wait_quiet.py`**: waits until other processes use under half a core for a minute. It ends with a JSON line; exit 1 at `--deadline`.
- `VulkanRenderer::graphicsQueue()` and `graphicsQueueFamily()`, used by the calibration kernel.

## 3. Bugs found and fixed

1. **The deferred renderer's GPU timer raced itself.** One query pool served all three frames in flight and was reset by each frame while the two before it could still be writing it, so its timings mixed frames. It is replaced by the per-slot timer.
2. **SSAO's graph pass ran with SSAO off.** The graph registered `ssao` every frame. With the effect off, `executeSSAO` dispatched nothing, but the pass still took the normal target's first read and its barrier, and it would have been timed as an empty pass. It is now registered only while SSAO is on, and lighting binds its fallback view otherwise. Lighting never samples that binding, and the goldens are unchanged.
3. **`docs/ARCHITECTURE.md`'s render-pass recipe** named `getPerfStats()["gpu_timings_ms"]` as the timing label's destination. That function had no caller and is gone; the recipe now says to register the name in the pass registry.
4. **The determinism lint** (`reflect_tests` `sp4_t02`) flagged the frame's CPU stopwatch in `render_dispatch.cpp`. It is a timing statistic, like `RenderSceneStats::recordCpuMs`, and has an allowlist entry saying so.

## 4. Spec corrections

- **Timestamps:** both at `BOTTOM_OF_PIPE` with `vkCmdWriteTimestamp`, not a `TOP_OF_PIPE` begin with `vkCmdWriteTimestamp2`. A top-of-pipe begin lets a pass's time include the tail of the pass before it, so the per-pass sum could exceed the frame.
- **The GPU calibration kernel:** 2^31 hash rounds (about 1.6 ms), not 2^24. 2^24 is about 0.01 ms on this GPU, too short to time against clock ramp-up. It is GLSL until step 6 ports it to Slang, which that step already names as its proof.
- **No per-process GPU sampling:** `nvidia-smi --query-compute-apps` reports memory, not utilisation. The calibration kernel is the GPU contention check.
- **`render.stats`:**
  - The result adds `frames`.
  - Times are rounded to the microsecond, about 20 bytes a pass.
  - `over`, `lightsOverflow` and `shadowLightsDropped` come with steps 15, 13 and 7.
- **`render_bench` results:** the spec's shape plus `span`, `cpu`, per-pass `frames`, and a `budgets` array with each budget's status. Budgets can name groups of passes.

## 5. Results

### The baseline (render_bench, 2026-10-24, clean)

RTX 5070 Laptop GPU, driver 621.1. CPU calibration ×1.12 and GPU ×0.93 against the reference; other processes 19.9% of a core. Medians of 100 frames, GPU ms (p95 where it differs by more than 5%):

| Pass | cornell_720p | helmet_game_720p | helmet_game_1080p |
|---|---|---|---|
| scene.upload | 0.000 | 0.000 | 0.000 |
| csm | 0.034 | 0.044 | 0.046 |
| gbuffer | 0.060 | 0.138 | 0.308 |
| ssao | — | 0.354 | 0.823 |
| rt.shadows | 0.516 | 0.587 | 1.314 (p95 1.466) |
| rt.gi | 3.948 | 4.323 | 10.231 (p95 11.577) |
| lighting | 0.178 | 0.135 | 0.328 |
| sss | 0.046 | 0.070 | 0.171 |
| ssr | 0.021 | 0.072 | 0.180 |
| sky | 0.026 | 0.023 | 0.028 |
| particles | 0.021 | 0.021 | 0.022 |
| post.bloom | — | 0.089 | 0.168 |
| post.taa | — | 0.047 | 0.111 |
| post.tonemap | 0.014 | 0.017 | 0.043 |
| readback | 0.212 | 0.213 | 0.465 |
| **frame** (without readback) | **4.866** | **5.925** | **13.772** (p95 15.449) |
| span | 5.076 | 6.132 | 14.246 |
| CPU record + submit | 0.258 | 0.315 | 0.285 |

Against the budgets, all pending (step 1 gates none):

| Budget | Baseline | Budget (gated from) |
|---|---|---|
| `cornell_720p` frame | 4.866 | 1.50 (9) |
| `rt.gi` | 3.948 | 0.30 (8) |
| `rt.shadows` + filter | 0.516 | 0.20 + 0.05 (7) |
| `csm` | 0.034 (runs beside RT shadows) | 0.00 (7) |
| `lighting` | 0.178 | 0.12 (13) |
| `sss` | 0.046 (no skin in Cornell) | 0.02 (9) |
| `gbuffer`, `ssr`, `sky + particles`, `post` | 0.060, 0.021, 0.047, 0.014 | 0.15 (11), 0.10, 0.05, 0.05 (9): already met |
| `helmet_game_720p` frame | 5.925 | 3.0 (9) |
| `helmet_game_1080p` frame | 13.772 | 5.0 (9) |

Where the time goes: RT GI is 81% of Cornell's frame and 74% of the helmet's at 1080p, so steps 7 and 8 carry most of the cut. SSAO costs 0.35–0.82 ms for an image lighting never reads (the monograph's SSAO unit records why), which is step 9's to wire. CSM renders though RT shadows replace it, which is step 7's to remove.

### Tests

- `engine_tests` 59/59, with the new `sp2b_t01_every_pass_is_registered_timed_once_and_within_the_frame`: a 320x180 deferred frame with SSAO, TAA and bloom has 13 passes in 0.133 ms.
- `api_tests` 43/43, with the new `sp2b_t14_render_stats_reports_the_hosts_medians`.
- `runtime_tests` 30/30, with the new `sp2b_t14_stats_are_medians_per_pass_in_registry_order`.
- `check_tests` 11/11, with the new `sp2b_t02_a_pending_budget_is_reported_not_gated`, `sp2b_t02_a_missed_gated_budget_names_its_pass` and `sp2b_t02_a_noisy_run_is_never_a_pass`.
- `check_serve` 166/166 (`render_stats` schema; E_STATE before any frame), `check_live` 26/26 (`render.stats` on the window host), `e2e_mcp` 20/20 (a stats read).
- `reflect_tests` 38/38, `check_cli` 29/29 (GPU) and 25/25 (CPU), `check_docs` 65 commands.
- Render goldens 11/11: the five deferred scenes max_abs 0; path-traced within their known noise (helmet max_abs 1, rt_realtime 2–3).
- `check_validation` 2/2 clean.
- Goldens updated for the new method only: the schema hash (`94eb6d1669f2d5d1` → `f7187a8561ba6b64`) in the CLI and serve expectations, and the E_METHOD hint's count of methods left out.

### Benchmarks

- `render_bench --check`: clean, 0/0 gated, every budget pending (above).
- `serve_bench --check`: noisy twice (Unreal Editor 190% and 182% of a core; the first run's CPU calibration was also ×1.93). That is no verdict. The readings are no different from sub-project 4's clean run: `window_readback` 5.38 / 5.42 ms (clean in step 11: 5.34), `window_capture_inline` 27.6 / 32.5 ms.

## 6. Known gaps

- **`build/render_bench_reference.json` was written on a throttled CPU.** Its CPU calibration is 28.3 ms; `serve_bench`'s quiet reference for the same loop is 23.2 ms. Until it is rewritten on a quiet machine, the CPU-noise check is about 20% lenient. Rewrite it (`render_bench --calibrate`) before step 9 gates the first budgets.
- The `bench100k_1080p` and `bench10k_1080p` scenes and the meshlet build measurement arrive with steps 11–13. Their budgets are in the file and reported `unmeasured`.
- `render.stats` has no `over` yet (step 15).
- A stray scratch file, `build/render_bench_reference_probe.json` (untracked), could not be deleted (`rm` refused this session).

## 7. Running the tests

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/api_tests.exe > /dev/null
$ build/Release/runtime_tests.exe > /dev/null
$ python tests/check/check_tests.py > /dev/null
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
```

`render_bench --calibrate` writes the reference on a quiet machine (`python tools/bench/wait_quiet.py` first).
