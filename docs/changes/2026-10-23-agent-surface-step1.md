# Sub-project 4, step 1: deterministic deferred rendering

**Spec:** `docs/specs/2026-10-23-agent-control-surface-design.md` (section "Determinism", including "Should 4 fix the 1-LSB post-effect noise?" and its as-built note; "Build order" step 1; "Test plan" tests 1 and 2)
**Status:** Done.

## 1. Summary

Deferred rendering, with TAA, SSAO and bloom on or off, is now bit-identical across fresh renderers, across processes and across repeated captures by one renderer. The spec expected the cause to be the SSAO noise seed (std::random_device); that seed is fixed as specified, but it was not the cause, because the lighting shader never samples the SSAO output. A pass-by-pass bisection found the two real causes, both OHAO code: a read-after-write race in bloom's mip chain, and TAA's first frame reading an uninitialized previous view-projection matrix. The four deferred goldens are now exact.

## 2. What was built

- SSAO noise from the render seed: `SSAOPass::noise(seed)` builds the 16 noise vectors with a counter hash (splitmix32-style) and exact float arithmetic, not a `<random>` distribution (their output differs between standard libraries). `SSAOPass::setNoiseSeed` (after initialization: idles the device, rewrites the staging buffer, uploads on the next execute). `PostProcessingPipeline::setNoiseSeed` / `noiseSeed`. `VulkanRenderer::setRenderSeed` now stores the seed (`getRenderSeed`), applies it to the deferred post-processing pipeline on creation and immediately, and to RT renderers created later.
- `RenderPassBase::sampledColorPassDependencies()`: the two external subpass dependencies of a one-subpass color pass that samples earlier passes' output and is sampled by later ones (in: earlier color-attachment and shader writes visible to this pass's sampling and load/blend, earlier reads of its attachment finished first; out: its writes visible to later sampling and copies). Used by bloom's three render passes (threshold, downsample, upsample), the TAA pass and the tonemap pass.
- `DeferredRenderer`: `m_prevViewProj` initialized, plus a validity flag; the first frame after creation or after `resetTemporalState` uses its own view-projection (no motion into it). `m_view`, `m_proj`, `m_cameraPos` and the G-buffer pass's matrices are initialized too.
- Tests:
  - `deferred_determinism_gpu_test` (new, 43 checks, GPU, validation on). Seven configurations rendered through `api::Session` + `host::RenderHost` at 960x540: Cornell plain, TAA alone, SSAO alone, bloom alone, all three; the helmet room plain and with TAA+SSAO+bloom (30 frames, exposure 1, as its golden). Each is rendered by two fresh renderers in the process, by a fresh renderer in a child process of the test (`--child`), and twice by one renderer; all must be byte-identical, with 0 validation messages. Also: the SSAO noise for a seed is the same every time, seed 0 matches a pinned hash (63fe119f530d173f), another seed differs, all vectors are unit length in XY; a fresh renderer's SSAO uses seed 0; `setRenderSeed(7)` then `(0)` reaches the SSAO pass; captures after reseeding render with 0 validation messages.
  - `reflect_tests` gained sub-project 4 test 2, two tests: `sp4_t02_lint_finds_nondeterminism` (the matcher on a sample) and `sp4_t02_no_unseeded_randomness_or_wall_clock_in_render_gpu_or_systems` (scans `ohao/render`, `ohao/gpu` and the ECS system sources for `random_device`, `rand(`, `srand(`, `_clock::now`, `glfwGetTime`, `time(nullptr)`, `time(NULL)`, `__rdtsc`, outside comments; one allowlist entry, `ohao/render/scene/render_scene.cpp` `_clock::now` for the `recordCpuMs` timing statistic; a stale allowlist entry fails).
  - `restore_render_gpu_test`: the post-effects case now requires a bit-identical restored capture (it allowed 1 LSB).
- Goldens: `damaged_helmet_deferred.png` regenerated after looking at the difference; all four deferred manifest entries (`cornell_box_deferred`, `damaged_helmet_deferred`, `cli_cornell_box_deferred`, `cli_damaged_helmet_deferred`) set to `max_abs_diff` 0 and `max_diff_frac` 0.0, with notes saying why.
- Docs: `AGENTS.md` lists the new suite and says the deferred goldens are exact; the `verify` skill change is drafted in `docs/agents/skills/verify/SKILL.md` with `docs/agents/skills/README.md` (this session cannot write `.claude/`; Frank installs it); the spec's "Why", "Determinism", test 1, build step 1 and risks carry as-built notes; the roadmap's 2b findings list gains SSAO; monograph units `deferred/ssao`, `deferred/post`, `systems/tests`, `systems/status`, `reflection/snapshots` and `architecture/invariants` updated, site rebuilt.

## 3. Bugs found and fixed

1. Bloom's mip chain raced. Post-processing runs outside the render graph; each bloom render pass's only external dependency guarded earlier reads (src FRAGMENT_SHADER / SHADER_READ) against its attachment writes, so nothing made the previous render pass's color writes visible to the next pass's sampling (each downsample reads mip i-1, each upsample reads mip i+1). Measured before the fix with each effect alone on Cornell, two fresh renderers: TAA 0, SSAO 0, bloom 807 differing bytes. The TAA and tonemap render passes had no explicit dependency at all; they share the fix.
2. TAA's first frame read an uninitialized matrix. `DeferredRenderer::m_prevViewProj` was a default-constructed `glm::mat4` (left uninitialized by glm). TAA blends towards the cleared history by the motion it sees (`blend = mix(blendFactor, 0.5, motionWeight)`), so a fresh renderer's first image depended on stale memory, while `resetTemporalState` set the matrix: a second capture by the same renderer differed from the first (Cornell TAA 18,788 bytes, Cornell all three 18,726, helmet with post 2,779).
3. The SSAO noise was seeded from `std::random_device` (different per process). Fixed as specified; it had no visible effect (see section 4).
4. RT renderers created after `setRenderSeed` did not receive the seed (the call reached only renderers that already existed).

Before any fix, two fresh renderers differed by 427 to 1,299 bytes on Cornell with all three effects and by 29,361 to 104,416 bytes on the helmet with post effects, across runs.

## 4. Spec corrections

The spec named the SSAO seed as the cause of every symptom. It is not: `deferred_lighting.frag` never samples the SSAO output (the lighting pass binds it at binding 10, which the shader does not declare), so SSAO reaches no pixel (`cornell_ssao` hashed the same as `cornell_plain`). The spec's "Why" bullet, its Determinism section (as built), test 1 and build step 1 now say so. Wiring SSAO into lighting changes the look and is listed under 2b in the roadmap.

## 5. Results

- `deferred_determinism_gpu_test` 43/43. `reflect_tests` 33/33. `restore_render_gpu_test` 14/14.
- Other suites: `runtime_tests` 21/21, `scene_tests` 60/60, `api_tests` 17/17, `systems_tests` 6/6, `physics_backend_tests` and `audio_system_tests` pass (exit 0), `force_generator_tests` 54/54, `env_cdf_test` 2/2, `denoise_parse_test` 5/5, `sobol_test` 8/8, `engine_tests` 58/58, `ecs_tests` 26/26, `renderer_test` passes, `instantiate_gpu_test` 5/5, `scene_view_gpu_test` 7/7.
- `check_docs.py` (every documented command, CPU and GPU, including `check_cli`, `check_scene_schema`, `check_validation` and the `ohao_game` presentation gate): 71/71 passed, 16 skipped (builds and slow gates, run separately).
- Ten separate-process renders of each deferred golden command, compared by full-resolution pixel hash: one distinct hash per scene (`cornell_box_deferred` and `cli_cornell_box_deferred` 43a989005529d1cc; `damaged_helmet_deferred` and `cli_damaged_helmet_deferred` 62be94cdc98ad35c). The example and the CLI give the same pixels.
- Old vs new helmet deferred golden at its 640x360 compare resolution: max_abs 1 on 6,778 of 230,400 pixels (2.9%), scattered, mean +0.01 LSB per channel; visually identical side by side. The Cornell deferred golden already matched (max_abs 0) and was not regenerated.
- Render goldens 10/10 after the change: `cornell_box` 0, `damaged_helmet` 1, `cornell_box_deferred` 0 (tolerance 0), `damaged_helmet_deferred` 0 (tolerance 0), `cornell_box_rt_realtime` 3, `cli_cornell_box` 0, `cli_damaged_helmet` 1, `cli_cornell_box_deferred` 0 (tolerance 0), `cli_damaged_helmet_deferred` 0 (tolerance 0), `cli_cornell_box_rt_realtime` 5 (max_abs per scene; path-traced tolerances unchanged).
- Monograph: `check_unit` passes for every edited unit; `test_monograph_structure` 16 tests OK.

### Benchmark

This step changes no code `scene_bench` runs: the codec, API and bench objects it executes were all compiled before the step began (12:13–14:43); the full build only relinked it. Six `scene_bench --check` runs, in order:

- Runs 1–3: FAIL. Every operation slower at once, by 15–25% (runs 1 and 2) and about 2× (run 3), snapshot included. Over budget: save pretty (10.75, 11.89, 21.11 ms), validate (10.25, 10.77, 19.35 ms), apply (0.248, 0.230, 0.360 ms); in run 3 also load, restore unchanged and query, and several bench100k ratios. CPU sampled 9–12% busy, then 30%, from other jobs on the machine.
- Runs 4–5: PASS, every budget and ratio. bench10k save pretty 9.461 / 9.448 ms, load 13.426 ms (run 4), validate 8.546 / 8.643 ms, restore into an empty world 2.018 ms (run 4), apply 0.186 / 0.188 ms.
- Run 6: FAIL, every operation about 2.6× slower again (save pretty 24.68 ms, snapshot 1.037 ms).

The pattern (a uniform slowdown of the same binary, coming and going between consecutive runs) is machine load, not this change. Sub-project 4's step 6 adds the calibration loop and `noisy` verdict for exactly this case.

## 6. Known gaps

- SSAO is computed when enabled but never applied by the lighting shader (also its noise tiling and decode are wrong, per the monograph's SSAO unit). Listed for 2b.
- The helmet deferred golden's exactness is measured within 2026-10-23; the Cornell deferred golden has measured max_abs 0 in every session since 2026-10-09.
- The `verify` skill change waits in `docs/agents/skills/` until Frank installs it.
- The nondeterminism lint matches tokens; it does not see randomness reached through a helper defined elsewhere.

## 7. Running the tests

```
build/Release/deferred_determinism_gpu_test.exe
build/Release/reflect_tests.exe
build/Release/restore_render_gpu_test.exe
python tests/golden/render_golden.py tests/golden/manifest.json
python tests/docs/check_docs.py
```
