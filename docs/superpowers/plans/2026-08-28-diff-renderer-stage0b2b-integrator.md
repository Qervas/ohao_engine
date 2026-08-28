# Stage 0b-2b — The Integrator and PathTracer Parity

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port BSDF evaluation, next-event estimation, MIS and environment importance sampling into the wavefront scatter stage, and gate the result on matching `PathTracer` output within Monte Carlo noise.

**Architecture:** The wavefront library from Stage 0b-2a (`ComputePipeline` / `WavefrontStage` / `WavefrontLoop`) already fuses a correct bounce loop. This stage fills in the physics: `wf_scatter.comp` grows from a placeholder cosine-hemisphere bounce with a hardcoded normal into a real integrator, reusing the existing RT shader includes rather than reimplementing them.

**Tech Stack:** Vulkan 1.3, `VK_KHR_ray_query`, GLSL compute, C++20.

**Spec:** `docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md`

---

## Global Constraints

- **No gradients.** Still Stage 1. This stage produces a correct *forward* integrator only.
- **No device-local state arena.** The host-visible arena's ~141 MB at 1080p is a known sizing issue; changing it here would confound this stage's physics work with a performance change.
- **No material sorting.** Still one material.
- **Reuse, do not reimplement:** `shaders/includes/rt/mis.glsl` (18 lines, two pure functions), `shaders/includes/rt/env_sampling.glsl` (96), `shaders/includes/rt/sampler_api.glsl` (43), `shaders/includes/material/ggx_aniso.glsl`, `shaders/includes/pbr_unpack.glsl`. If one needs changing to be reusable from a compute stage, change it in place and keep the RT pipeline working — do not fork it.
- **No new Vulkan features or extensions** beyond `VK_KHR_ray_query` and `shaderBufferFloat32AtomicAdd`. Never image atomics.
- **Do not validate against `examples/`.** Every check is probe-owned.
- **A stage records WORK, never ORDERING.** `WavefrontStage` contains no barriers. `WavefrontLoop` owns them all.
- **Editing a shader can break the documentation build.** `site/content/units/**/*.md` cites shader source
  by EXACT SUBSTRING via `{{cite <path> "<text>"}}`, and `site/tools/monograph/cite.py` raises
  `CitationError` when the substring is gone — there is deliberately no fallback from the working tree to
  history. So deleting or reworded a cited line breaks the site build, silently as far as the C++ test suite
  is concerned: `diff_gpu_probe`, `diff_unit_tests` and `renderer_test` all still pass. **Before committing a
  change to any file under `shaders/`, grep `site/content/` for citations of the lines you touched.** If a
  cited line is genuinely wrong (as `ggxDiso`'s `+1e-8` was), fix the code and update the prose — do not
  preserve a bug to keep a citation resolving. Pin with `{{cite path@<rev> "text"}}` only where the text is
  deliberately discussing the historical form.

- **Check counts are descriptions, never targets.** Count before, count after, report both. The only failure is a check that disappears, goes silent, or gets weaker. The probe currently prints 23 `OK:` lines.

## Inherited hazards — read before Task 1

**1. Synchronization validation is measurably blind here.** With every compute-side memory barrier in the fused loop disabled, all checks pass across three runs with zero `SYNC-` diagnostics. The only barrier whose absence anything detects is `COMPUTE_SHADER → DRAW_INDIRECT`. **A passing test run is not evidence about barriers.** See the spec's section 3.1 measurement.

**2. New scatter-side outputs are caller-owned, and that is exactly how a real bug got in.** Stage 0b-2a's final review found a write-after-write on the probe's debug-draws buffer with no barrier at all: `wf_scatter.comp` wrote it at a fixed offset every bounce, and `WavefrontLoop` only barriered `state`/`queue`/`counter`. It passed because the driver serialises. `record()` now takes `std::span<const VkBuffer> extraBarrierBuffers` for precisely this. **Every new buffer this stage's scatter stage writes — radiance accumulation, film, light-sample scratch — must go through it.** If you add a buffer and do not pass it, nothing will fail and the bug ships.

**3. The staircase scene is engineered around the placeholder normal and Task 1 destroys it.** `wf_scatter.comp` currently hardcodes `normal = (0,0,1)`, which makes every path march monotonically in +Z. The probe's 8-stacked-quad scene exists so paths survive four bounces under that assumption. The moment Task 1 reads a real geometric normal, that stops holding and the survival check fails loudly (it asserts `liveCount == kCapacity`). **This is intended.** The check is doing its job; the scene must be replaced in the same task.

**4. The recurring defect class: a check whose expected value derives from the same constant, shader or helper as the measured value cannot fail, while looking rigorous.** Stage 0b-1 produced six instances, one of which compared throughput against `pow(albedo, 4)` computed from the very constant being perturbed. This stage is *full* of GPU-vs-CPU comparisons and is the highest-risk stage yet for it. **Every parity assertion needs an oracle derived independently of the code path it measures, and every one needs a demonstrated failure.** A check that has never been seen to fail is a claim, not a check.

**5. Assertions quantified over "surviving paths" can pass vacuously.** Stage 0b-2a nearly shipped one. Prefer quantifying over all `kCapacity` entries — dead paths retain stale values and fail loudly — and assert the live count separately and explicitly.

**6. `ComputePipeline::build()` now has a re-entrancy guard** (added in `e2f88e7`); it destroys any existing build first. Comments claiming otherwise are stale.

## Source material — what you are porting from

`shaders/rt/pt_raygen.rgen` is **1270 lines, of which `main()` is roughly 1150.** It has almost no function decomposition: `cosineHemisphere`, `ACES` and `nrdPackNormalRoughness` are the only helpers; everything else is inlined.

It also contains **duplicated logic you must not carry across**:
- NEE appears twice — at `:584` attributing to `specContrib`, and at `:892` attributing to `diffContrib`. Same computation, different accumulation target.
- `sampleEnvMap` is called at `:443` and `:713`.

Port these as *one* parameterised implementation. A port that reproduces the duplication has copied the bug along with the physics, and this project has been bitten by duplicated GPU code three times (`camera_ray.glsl`, `loadSpv`, barrier (5)).

---

### Task 1: Real geometric normals, and the scene that survives them

**Files:**
- Modify: `shaders/diff/wf_scatter.comp`, `shaders/diff/wf_intersect.comp`
- Modify: `ohao/diff/wavefront/path_state_layout.hpp/.cpp`
- Modify: `tests/diff/gpu_probe_context.cpp/.hpp`, `tests/diff/diff_gpu_probe.cpp`

**Interfaces produced:** `PathStateField::NormalX/Y/Z` (or a packed octahedral equivalent — decide and justify; `shaders/includes/common/encoding.glsl` already has octahedral encode/decode).

`wf_intersect.comp` must write the hit's geometric normal into path state; `wf_scatter.comp` must read it instead of hardcoding `(0,0,1)`.

- [ ] **Step 1: Write the failing check first.** A closed-form check: for a scene of known analytic geometry, assert the stored normal at every hit matches the analytic surface normal exactly (to float tolerance you state and justify). The oracle is the analytic geometry, computed host-side — **not** anything the shader computes.
- [ ] **Step 2: Run it. Confirm it fails** (normals not yet written).
- [ ] **Step 3: Implement.** Add the field(s), write from intersect, read from scatter.
- [ ] **Step 4: Replace the probe scene.** The staircase's survival guarantee is now void — see inherited hazard 3. Design a scene whose four-bounce survival is provable rather than probable, state the derivation, and keep the existing `maxBounces` guard meaningful (it currently ties bounce count to `kFusedLoopHalfExtent`, `kFusedLoopPlaneGap` and `kFusedLoopPlaneCount`; update its arithmetic to the new scene or replace it).
- [ ] **Step 5: Verify** every pre-existing check still passes with its original expected values. Report the `OK:` count before and after.
- [ ] **Step 6: Commit.**

---

### Task 2: BSDF evaluation

**Files:** Modify `shaders/diff/wf_scatter.comp`; reuse `shaders/includes/material/ggx_aniso.glsl` and `shaders/includes/pbr_unpack.glsl`.

Replace the placeholder `p=0.5` throughput decay with real BSDF evaluation and sampling (GGX specular + Lambertian diffuse, matching what `pt_raygen.rgen` does).

- [ ] **Step 1: Write the failing check first — with an independent oracle.** Assert the GPU's BSDF value and PDF match a **CPU reimplementation written from the published formulas**, not from a transcription of the GLSL. This is the defect class in inherited hazard 4: a CPU oracle transcribed from the shader under test cannot fail. State in a comment where your oracle's formulas came from.
- [ ] **Step 2: Confirm it fails.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify, and demonstrate the check can fail** — perturb one BSDF term (e.g. a Fresnel exponent), confirm the check fails loudly, restore, reverify. Paste both outputs.
- [ ] **Step 5:** Furnace test — with albedo 1 and no absorption, a closed white environment must converge to 1.0 within stated Monte Carlo error. This is an independent global check that catches energy-loss bugs the per-term check cannot.
- [ ] **Step 6: Commit.**

---

### Task 3: Environment importance sampling

**Files:** Modify `shaders/diff/wf_scatter.comp`, `ohao/diff/wavefront/wavefront_buffers.hpp/.cpp` (env CDF bindings), `tests/diff/gpu_probe_context.cpp`.

Reuse `shaders/includes/rt/env_sampling.glsl` verbatim. Note `sampleEnvMap(u1, u2, W, H, envIntegral, out dir, out pdf)` calls `searchMarginal`/`searchConditional`, which read SSBOs — so this task adds bindings, and every new buffer must reach `record()`'s `extraBarrierBuffers` if the scatter stage writes it (see inherited hazard 2).

- [ ] **Step 1: Write the failing check first.** χ² test: draw N samples, bin them, assert the empirical distribution matches the CDF's own marginal/conditional within a stated confidence. The oracle is the CDF **as built on the host**, independent of the GPU search.
- [ ] **Step 2: Confirm it fails.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify**, and assert `pdf > 0` for every sampled direction plus that returned PDFs integrate to 1 within tolerance.
- [ ] **Step 5: Demonstrate failure** — perturb the CDF (e.g. shift one row), confirm the χ² check rejects, restore.
- [ ] **Step 6: Commit.**

---

### Task 4: NEE and MIS — one implementation, not two

**Files:** Modify `shaders/diff/wf_scatter.comp`; reuse `shaders/includes/rt/mis.glsl`.

Add next-event estimation with MIS against the BSDF sampling strategy. `mis.glsl` provides `misBalanceHeuristic(pdfA,pdfB)` and `misPowerHeuristic(pdfA,pdfB)` — use them; do not write a third.

**Port the source's two NEE blocks (`pt_raygen.rgen:584` and `:892`) as ONE parameterised implementation.** They differ only in accumulation target.

- [ ] **Step 1: Write the failing check first.** The strong, independent oracle here is **strategy agreement**: BSDF-sampling-only and NEE-only estimators of the same scene must converge to the same value within Monte Carlo error, and the MIS combination must match both. Three estimators, one truth — no shared expected value. State the error bound and how you derived it.
- [ ] **Step 2: Confirm it fails.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Assert MIS weights for the two strategies sum to 1 for every sample — cheap, exact, catches an entire class of weighting bug.
- [ ] **Step 5: Demonstrate failure** — invert one heuristic's arguments, confirm the strategy-agreement check rejects, restore. An inverted heuristic still produces plausible images, which is why this demonstration matters.
- [ ] **Step 6: Commit.**

---

### Task 5: Radiance accumulation and film output

**Files:** Modify `shaders/diff/wf_scatter.comp`, `ohao/diff/wavefront/wavefront_buffers.hpp/.cpp`, `ohao/diff/wavefront/wavefront_loop.hpp/.cpp` (only if the extras mechanism needs extending), `tests/diff/gpu_probe_context.cpp`.

- [ ] **Step 1:** Add the radiance/film buffer. **It is caller-owned and written by the scatter stage every bounce — pass it through `record()`'s `extraBarrierBuffers`.** Inherited hazard 2 is exactly this shape and it shipped a real bug last stage.
- [ ] **Step 2: Write the check first** — assert accumulated radiance across bounces equals the host-computed sum of per-bounce contributions read back independently.
- [ ] **Step 3: Confirm it fails. Step 4: Implement. Step 5: Verify.**
- [ ] **Step 6:** State explicitly, in the code, which barrier orders this buffer between bounces, and why. Nothing will test it.
- [ ] **Step 7: Commit.**

---

### Task 6: PathTracer parity — the gate

**Files:** Modify `tests/diff/gpu_probe_context.cpp/.hpp`, `tests/diff/diff_gpu_probe.cpp`.

Render the same probe-owned scene through both `PathTracer` and the wavefront integrator and assert they agree within Monte Carlo noise.

**Not through `examples/cornell_box.cpp`.** The scene is probe-owned so its geometry, materials and light are known to the check.

- [ ] **Step 1:** Build the shared scene and drive both integrators over it at a fixed seed and sample count.
- [ ] **Step 2: Write the check.** Assert per-pixel agreement within a bound **you derive from the sample count and scene variance and state**, not one tuned until it passes. A tolerance widened until green is not a gate. If you cannot derive it, say so and use a variance estimate from the run itself — but say which you did.
- [ ] **Step 3: Verify.** Also assert the two agree *better* as sample count rises (e.g. at 4× samples the error bound halves) — a fixed-tolerance check can pass on two wrong-but-close images; a convergence check cannot.
- [ ] **Step 4: Demonstrate failure** — perturb one material constant in one integrator only, confirm the parity check rejects, restore. **This is the single most important demonstration in the stage.** Without it, parity is an assertion that has never been observed to fail.
- [ ] **Step 5: Commit.**

---

## Exit criteria

- [ ] `diff_gpu_probe` exits 0, 0 validation errors, every check existing at the start of this stage still passing with its original expected values, plus the new integrator checks. Report the `OK:` count — it is a description, never a target.
- [ ] Wavefront and `PathTracer` agree within a **stated, derived** Monte Carlo bound on a probe-owned scene, and agreement improves with sample count.
- [ ] Every new parity/physics check has a **demonstrated failure** recorded with pasted output. A check never seen to fail is not yet a check.
- [ ] Furnace test converges to 1.0 within stated error.
- [ ] MIS weights sum to 1 for every sample.
- [ ] Every new scatter-written buffer is passed through `record()`'s `extraBarrierBuffers`, and the barrier ordering it is documented in code.
- [ ] `diff_unit_tests` and `renderer_test` still pass; clean `-j8` build.

## What this stage deliberately does not do

- **No gradients.** Stage 1.
- **No device-local arena.** Known ~141 MB at 1080p; deferred deliberately.
- **No material sorting.** Still one material.
- **No denoiser integration.**

## Next plan

**Stage 1 — gradients.** The forward integrator becomes differentiable via Path Replay Backpropagation: the seed invariant established in Stage 0a makes a path a pure function of `(pixel, sampleIndex, iterationSeed)`, so the backward pass replays rather than stores. Gate on finite differences against this stage's forward output.
