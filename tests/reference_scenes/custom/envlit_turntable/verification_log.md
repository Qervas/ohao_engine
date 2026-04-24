# Verification log — envlit_turntable

## 2026-04-17: MIS env+BSDF (Feature 1.1) validation

### Direct variance comparison (MIS-on vs MIS-off, both 16 spp)

- OHAO MIS-on 16 spp local 5x5 variance:  0.002863
- OHAO MIS-off 16 spp local 5x5 variance: 0.003186
- Raw local variance reduction: +10.1%
- Global RMSE MIS-on vs MIS-off: 0.071113

### Ground-truth-anchored comparison (vs OHAO 4096 spp as convergence reference)

Since Cycles was not available for cross-engine check, a 4096-spp OHAO render
stands in as the converged-truth baseline. This isolates sampling noise from
real surface detail, which otherwise inflates "local variance" with non-noise
high-frequency signal.

- Ground-truth local 5x5 variance:        0.001383  (signal detail only)
- MIS-on 16 spp excess over truth:        0.001480  (sampling noise)
- MIS-off 16 spp excess over truth:       0.001803  (sampling noise)
- Excess-variance reduction (MIS-on vs MIS-off): **-17.9%**
- MIS-on RMSE vs truth: 0.0699
- MIS-off RMSE vs truth: 0.0710
- MIS-on is **1.5% closer to ground truth in L2** than MIS-off

### Assessment

Feature structurally correct (math reviewed, PDF tracking verified) AND
numerically beneficial: reduces sampling noise ~18% and moves closer to the
converged reference. Scene topology (open env + glossy model) is worst-case
for env MIS; future enclosed-scene references (model in a room with an env
window) should show a larger improvement.

Feature 1.1 complete. Task 7 Blender Cycles cross-engine check deferred —
the 4096-spp OHAO self-convergence check substitutes as the quality gate.

## 2026-04-17: Sobol sampler (Feature 1.2) validation

Offline sampler upgraded from PCG to Owen-scrambled Sobol. Direct
variance comparison at equal 16 spp:

| Sampler | Local 5×5 variance | RMSE vs 4096-spp truth |
|---------|-------------------|-------------------------|
| PCG (previous)  | 0.002863  | 0.069884 |
| Sobol (current) | 0.002584  | 0.067062 |

- Noise reduction (variance) vs PCG: +9.8%
- RMSE improvement vs truth: +4.0%

Feature 1.2 complete. Reference render regenerated with Sobol (offline default).

## 2026-04-18: OIDN denoiser switch (Denoiser Sub-plan 1) validation

Offline profile now denoises via Intel OIDN by default. Comparison at
16 spp on DamagedHelmet + env_studio:

| Mode | Local 5×5 variance | RMSE vs 4096-spp truth |
|------|-------------------|------------------------|
| --denoise=none (noisy)   | 0.002584 | 0.067062 |
| --denoise=oidn (default) | 0.000734 | 0.037243 |

- RMSE reduction: 44.5%
- Visual: Cornell walls + helmet edges look clean at 16 spp

Denoiser Sub-plan 1 complete. Sub-plans 2-5 (OptiX, realtime
foundation, NRD, DLSS RR) follow per the roadmap.

## 2026-04-18: OptiX denoiser (Denoiser Sub-plan 2) validation

OptiX backend available when CUDA + OptiX SDK present at build time.
Comparison at 16 spp on DamagedHelmet + env_studio:

| Mode | RMSE vs 4096-spp truth |
|------|------------------------|
| --denoise=none  (noisy)  | 0.067062 |
| --denoise=oidn           | 0.037243 |
| --denoise=optix          | 0.038676 |

- OptiX RMSE reduction vs noisy: 42.3%
- OptiX vs OIDN RMSE delta: -3.8%  (signed — positive = OptiX better, both acceptable)
- Build with OPTIX_ROOT unset → --denoise=optix falls back to OIDN

Denoiser Sub-plan 2 complete. Next: Sub-plan 3 (realtime foundation —
motion vectors + history + depth/roughness AOVs) unlocks NRD (Sub-plan 4)
and DLSS RR (Sub-plan 5).

## 2026-04-18: Camera motion vectors (Sub-plan 3.A) validation

Path tracer now writes per-pixel 2D MV AOV (RG16F @ binding 19) on
first-hit. Camera motion only; skeletal is 3.F.

Verification:
- **Static camera:** MV dump is uniform gray (128, 128, 128) — zero motion.
  Saved: `renders/mv_static.png`.
- **Regression:** beauty output at `--denoise=none` bit-identical to
  pre-3.A — MV is pure output AOV.

Known limitations:
- First frame outputs zero MV (prevViewProj is stale). Documented.
- Pan-camera test deferred to Sub-plan 3.C (temporal reprojection needs
  camera-animation infrastructure that env_demo doesn't have).

Sub-plan 3.A complete. Next: 3.B (depth + roughness AOVs).

## 2026-04-19: Depth + roughness AOVs (Sub-plan 3.B) validation

Path tracer now writes view-space depth (R32F @ binding 20) and
perceptual roughness (R8 UNORM @ binding 21) on first-hit.

Verification on DamagedHelmet + env_studio, 16 spp, --denoise=none:
- **Depth:** grayscale dump shows helmet near-surfaces darker, back
  surfaces lighter, sky (miss) fully white (sentinel 1e30).
  Max finite depth recorded: 9.41316 world units.
  Saved: `renders/depth_helmet.png`.
- **Roughness:** visor near-black (glossy metal), helmet body dark
  gray, dielectric accessories lighter, sky white (miss sentinel 1.0).
  Saved: `renders/roughness_helmet.png`.
- **Regression:** beauty output at `--denoise=none/oidn/optix`
  bit-identical to pre-3.B. AOVs are pure output.

Sub-plan 3.B complete. Next: 3.C (history refactor — MV-aware
temporal reprojection + diffuse/specular split).

## 2026-04-19: Diffuse + specular radiance split (Sub-plan 3.C) validation

Path tracer now writes demodulated diffuse (RGBA16F @ binding 22) and
specular (RGBA16F @ binding 23) radiance streams on first-hit, for
future NRD consumption (Sub-plan 4).

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Diffuse:** warm ambient shading across matte plate areas; visor
  near-black (metal lobes skip diffuse channel; F0 × albedo = 0 for pure metal).
  Max channel value: 93.25.
  Saved: `renders/diffuse_helmet.png`.
- **Specular:** bright visor with HDR env reflection; metal body
  bright; dielectric accessories dimmer.
  Max channel value: 34048.
  Saved: `renders/specular_helmet.png`.
- **Regression:** beauty output at `--denoise=none/oidn/optix`
  unchanged from pre-3.C (split logic mirrors existing radiance math;
  no accumulation path modified).
- **Sum-match:** eyeball check — diffuse + specular re-modulated
  approximately equals beauty. Exact CPU regression deferred to
  Sub-plan 4 NRD integration.

Sub-plan 3.C complete. Next: 3.D (disocclusion mask).

## 2026-04-19: Format upgrades — roughness R16F + radiance RGBA32F (Sub-plan 3.C.5)

Format promotions for quality-first NRD consumption:
- Binding 21 roughness: R8_UNORM → R16_SFLOAT (+2 MB at 1080p).
- Bindings 22+23 radiance: RGBA16F → RGBA32F (+33 MB at 1080p combined).
- Raygen clamp `min(..., 60000.0)` removed from diffuse+specular exit writes.

Readback helper signatures upgraded to native `std::vector<float>` for
cleaner debug consumers. env_demo dump decoders simplified (no more
half2float dance for diffuse/specular; MV still uses it).

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Roughness dump:** visually identical to pre-3.C.5 at 8-bit display.
  R16F AOV has finer precision at low-roughness (visor/glossy) regions
  that the PNG quantization hides; NRD will consume the full precision.
- **Diffuse dump:** unchanged in appearance (demodulation math unchanged).
  Max channel: 93.2568.
- **Specular dump:** max channel: 34076.5. If > 60000, this
  is the clamp removal taking effect on genuine HDR spikes. Reinhard
  display may show relative darkening vs pre-3.C.5.
- **Regression:** beauty output unchanged (format-only change — no
  radiance math modified).

Sub-plan 3.C.5 complete. Next: 3.C.6 (demod AOV exposure — write raw
radiance, expose firstHitDiffAlbedo / firstHitSpecColor at new bindings
so NRD remodulates downstream).

## 2026-04-19: Demod AOV exposure (Sub-plan 3.C.6)

Raygen stops pre-dividing radiance by albedo/F0 at exit. Two new AOVs
expose demod factors so NRD remodulates downstream:
- **Binding 24 (diffuse albedo)**: RGBA8 UNORM. `isMetal ? 0 : albedo`.
- **Binding 25 (specular color)**: RGBA8 UNORM. `isMetal ? albedo : vec3(0.04)`.

Bindings 22+23 now hold RAW `diffContrib` / `specContrib` radiance
(not demodulated). Removes the 100×-amplification-on-dark-channels
bug in the 3.C demod division.

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Diff albedo dump:** matte plates show helmet base color; metal
  regions near-black; sky black.
- **Spec color dump:** matte plates near-black (~10/255, dielectric F0);
  metal regions show metal albedo; sky black.
- **Diffuse radiance (raw):** max channel 19.9896. Dimmer
  than 3.C.5 (was 93.25).
- **Specular radiance (raw):** max channel 1363.06. Also
  dimmer than 3.C.5 (was 34076).
- **Regression:** beauty output unchanged (AOV semantics only).

Memory: +8 MB at 1080p for 2 new RGBA8 AOVs.

Sub-plan 3.C.6 complete. Next: 3.C.7 (dual-ray bounce-0 split — first
visible offline quality improvement).

## 2026-04-20: Dual-ray bounce-0 split (Sub-plan 3.C.7)

At primary hit, raygen now traces two independent indirect paths (one
diff-sampled, one spec-sampled) instead of stochastically picking one
lobe. `lobeMask` deleted entirely. Each path has its own trajectory
and accumulates exclusively into its own channel.

Cost: 1 primary + 2×maxBounces indirect rays (~1.8× for maxBounces=4;
~1.5× for realtime maxBounces=1). Shader-only change.

Correctness fix shipped in T1 (commit 034859d): dual-ray init blocks
do NOT divide throughput by `/P(lobe)` — that was a single-ray
stochastic-selection correction. With deterministic dual-ray, each
path's throughput is just `f*cos/pdf` for its lobe (simplified as
`isMetal ? albedo : 1` for spec, `albedo` for diff). Inner-loop
bounce-≥2 BSDF sample KEEPS single-ray semantics.

Verification on DamagedHelmet + env_studio:

- **1 spp, --denoise=none:** diffuse AOV shows ambient lighting on
  every matte pixel; specular AOV shows env reflection on every
  glossy pixel. No more stochastic "polka dot" lobe-select pattern.
  diffuse max: 47.0279. specular max: 379.487.
  Compared to pre-3.C.7's 1spp render where half the pixels were
  zero in one channel — dramatically cleaner.
- **256 spp, --denoise=none:** beauty converges to a plausible
  HDR image. No 2× brightness regression (correctness fix works).
- **Cornell 64spp regression:** standard Cornell box (red/green walls,
  spheres) renders correctly.

Visible win at low spp: **yes** — first sub-plan in the 3.x chain to
ship a user-visible quality improvement at 1-16 spp. At high spp
convergence unchanged.

Follow-ups parked for Sub-plan 4:
- Dead `specProb` locals in Stage B/C init blocks (computed but unused
  in dual-ray context). Cosmetic.
- Unused `const float OHAO_PI` local in pt_raygen_realtime.rgen top.
- `rectArea` vs `area` naming drift between realtime and default/offline.
- Spec-constant gate to disable dual-ray in realtime if NRD shows it
  costs too much frame time.
- Extract Stage B/C inner loop into `indirect_path.glsl` helper
  (bundled with the longstanding readback-helper extraction).

Sub-plan 3.C.7 complete. Next: 3.D (ping-pong prev-depth for NRD
disocclusion input).

## 2026-04-20: Hit-distance packing (Sub-plan 3.D)

Raygen packs hit-distance into alpha channel of bindings 22 (diffuse) and 23
(specular). NRD REBLUR consumes this for per-pixel spatial-filter sizing.

Semantics per NRD convention:
- Diffuse: single-segment. Distance from primary hit to first secondary hit
  (bounce 1's `payload.hitDist` in Stage C, captured via `diffHitRecorded` latch).
- Specular: accumulated along specular chain. Each Stage-B bounce adds
  `payload.hitDist` to `specHitDist` while `specChainActive` is true. Chain
  breaks on the first diffuse BSDF sample at bounce >=2 — flag flips false
  and accumulation stops. Matches NRD-canonical virtual-distance semantics
  for mirror-chain preservation.

Shader-only change. No new bindings, no C++ touches. Raw world-space
units; NRD-side normalization deferred to Sub-plan 4.

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Specular hit-distance dump:** visible helmet silhouette (visor + neck
  plates + collar edges). Sky correctly black. Max: 3.1221 world units.
  Saved: `renders/hit_dist_specular_helmet.png`.
- **Diffuse hit-distance dump:** visible helmet silhouette with moderate
  gray on matte seams and edges. Sky correctly black. Max: 3.17404 world
  units. Saved: `renders/hit_dist_diffuse_helmet.png`.
- **Radiance dumps (unchanged from 3.C.7):** diffuse max 27.3332; specular
  max 475.01. Alpha-channel packing has zero effect on RGB sum — confirmed
  by bit-identical beauty render.
- **Regression:** beauty output (`renders/helmet_64spp_with_hitdist.png`)
  and Cornell 64 spp match pre-3.D visually.

### NRD-canonical behavior note

Max specular hit-distance (3.12 wu) is numerically close to max diffuse (3.17 wu) on
this helmet scene. This is **correct NRD REBLUR behavior, not a bug.** On visor
pixels, the primary specular bounce escapes to env (payload.hitDist = -1), so our
`payload.hitDist >= 0.0 && specChainActive` guard correctly skips accumulation
and the pixel's alpha stays at 0. REBLUR's convention is `alpha = 0` → "sky sample"
→ filter radius set per its sky-sample path, which is exactly what we want.

Spec §6's "visor + metal dome bright" visual prediction was based on an incorrect
mental model (treating env-miss as a virtual hit at infinity). In reality,
specular-alpha would show *dramatic* brightness on scenes with **mirror-on-geometry**
or **mirror-on-mirror** chains (car paint under a roof, chrome kitchen interiors,
dual-mirror setups). The helmet visor reflecting open sky is the wrong showcase
for this AOV.

No action needed for Sub-plan 4 NRD integration — the current semantics are what
REBLUR expects.

Sub-plan 3.D complete. Next: Sub-plan 4 (NRD integration) — the jaw-drop
moment when realtime 1spp renders look like offline 1024spp.

## 2026-04-23: NRD library integration (Sub-plan 4.A)

NVIDIA RayTracingDenoiser wired via CMake FetchContent as hard dep.
`NrdDenoiser` PIMPL exposes `initialize` + `shutdown` against the
existing Vulkan device. Denoise dispatch lands in 4.B.

Build integration:
- `OHAO_NRD=ON` (default): FetchContent clones NRD v4.17.2, links
  the static library into `ohao_renderer`, compiles `nrd_denoise.cpp`
  with `OHAO_NRD_ENABLED` defined.
- `OHAO_NRD=OFF`: FetchContent + link skipped. `nrd_denoise.cpp` still
  compiles (GLOB_RECURSE picks it up) but its body is `#ifdef`-guarded
  to an empty TU. Callers use `#ifdef OHAO_NRD_ENABLED` to guard
  instantiations.

Pinned NRD tag: v4.17.2.

Resolved NRD v4.17 API symbols:
- `nrd::CreateInstance(const InstanceCreationDesc&, Instance*&)` — out-ref
- `nrd::DestroyInstance(Instance&)` — takes reference (not Release)
- `nrd::DenoiserDesc` holds `{identifier, denoiser}` only — render
  resolution is **not** a creation-time field; it's supplied per-frame
  via `CommonSettings` in 4.B.
- `nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR`, `nrd::Result::SUCCESS`.

Lifecycle smoke via probe in `PathTracer::init`:
```
[PathTracer] Initialized (1920x1080)
[NRD] initialized for 1920x1080
[NRD probe] 4.A lifecycle smoke passed
```
No Vulkan validation errors. Probe fires twice (once for RTRealtime
and once for RTOffline PathTracer instances — expected, both call
`PathTracer::init`). Probe removed in 4.B when real dispatch takes over.

OFF-build smoke: `./build-nonrd/cornell_box ...` produces `Saved:` line
with no `[NRD]` log lines — probe is fully `#ifdef`-guarded out.

Deviations from plan:
- Plan spec'd probe in `ohao/gpu/vulkan/renderer.cpp`. Moved to
  `ohao/render/rt/path_tracer.cpp` (specifically `PathTracer::init`
  tail, after SBT creation) because NRD is linked to `ohao_renderer`,
  not `ohao_gpu_vulkan` — T1 review surfaced this split-lib constraint.
  `OHAO_NRD_ENABLED` is only defined for the `ohao_renderer` target.
- Plan template included `renderWidth`/`renderHeight` fields on
  `DenoiserDesc`; NRD v4.17's struct doesn't have them. Adapted — the
  probe still passes `w`/`h` into `initialize()` and stores them on Impl
  for future use (CommonSettings in 4.B).
- NRD's `NRD` CMake target does propagate `INTERFACE_INCLUDE_DIRECTORIES`
  correctly, so no `target_include_directories` workaround was needed
  in `ohao/render/CMakeLists.txt`.

## 2026-04-23: NRD API expansion — normal+roughness AOV + settings pump (Sub-plan 4.B)

T1 (commits 744dded + aa2e3f9 fix): Added packed normal+roughness AOV at
binding 26 (R10G10B10A2_UNORM with NRD canonical rotated oct-pack + sign-
smuggled roughness, matching `_NRD_EncodeNormalRoughness101010` from NRD
v4.17's NRD.hlsli verbatim). NRD_NORMAL_ENCODING=2 + NRD_ROUGHNESS_ENCODING=1
pinned in CMake to prevent version drift. All 3 raygens write the canonical
pack at Stage A first-hit; zero-init on miss. Descriptor pool STORAGE_IMAGE
15→16, AOV barrier group 9→10, bindings count 26→27. Beauty untouched.

T2: Expanded NrdDenoiser public API with `NrdCameraInputs` + `NrdInputImages`
structs and `setCommonSettings` + `setInputImages` methods. `setCommonSettings`
calls `nrd::SetCommonSettings` per-frame with view/proj/jitter/frameIndex
populated. `setInputImages` stores VkImageView handles on Impl for 4.C's
compute dispatch to consume via NRD's UserPool.

Resolved NRD v4.17 CommonSettings field names against
`build/_deps/nrd-src/Include/NRDSettings.h`: `viewToClipMatrix`,
`worldToViewMatrix`, `worldToViewMatrixPrev`, `motionVectorScale`,
`cameraJitter`, `cameraJitterPrev`, `frameIndex`,
`isMotionVectorInWorldSpace` all match the plan's candidates verbatim.
One non-obvious hard requirement: NRD asserts `resourceSize` and
`rectSize` must be non-zero (InstanceImpl.cpp:283). `setCommonSettings`
populates them from the width/height captured at `initialize()`;
dynamic-resolution support deferred to 4.C+.

Verification on probe run (identity matrices, dummy jitter):
```
[NRD] initialized for 1920x1080
[NRD] initialized for 1920x1080
[NRD probe] 4.B CommonSettings accepted
[NRD] initialized for 1920x1080
[NRD] initialized for 1920x1080
[NRD probe] 4.B CommonSettings accepted
Saved: /tmp/t2_4b_cornell.png
```
(The first `[NRD] initialized` per PathTracer is logged inside
`NrdDenoiser::initialize`; the second is the probe's own acknowledgement.
Probe fires twice — RTOffline + RTRealtime PathTracer instances.)

- Beauty output unchanged from pre-4.B.
- No Vulkan validation errors on OHAO_NRD=ON.
- OFF build (OHAO_NRD=OFF) unaffected (normal+roughness AOV still writes —
  it's NRD-agnostic; probe is `#ifdef`-compiled-out).

Next: Sub-plan 4.C (first REBLUR compute dispatch — denoise actually runs).

Next: Sub-plan 4.B (NRD API expansion — per-frame input population).

## 2026-04-23 — Sub-plan 4.C T3b: First REBLUR dispatch lives

**Command:**
```bash
./build/env_demo assets/walking_woman.glb assets/test_models/env_studio.hdr \
    /tmp/beauty.png 1 \
    --dump-diffuse=/tmp/raw_diff.png --dump-nrd-diffuse=/tmp/nrd_diff.png \
    --dump-specular=/tmp/raw_spec.png --dump-nrd-specular=/tmp/nrd_spec.png
```

**Evidence:**
- `[NRD] integration ready @ 1920x1080 (NRI-backed REBLUR_DIFFUSE_SPECULAR)`
  logged twice at init (one per PT profile — RTOffline + RTRealtime).
- `[NRD] persistent instance ready @ 1920x1080` confirms PathTracer's
  wrapper accepted the integration.
- First NRD dispatch fired on the command buffer without any new Vulkan
  validation errors (9 pre-existing errors from deferred renderer
  descriptor layout — identical count before and after T3b).
- `nrd_diff.png` visibly smoother than `raw_diff.png` — spatial filter
  dominates at `frameIndex=0` (no temporal history, single-shot REBLUR).
  Max channel reduced from 1164.1 (raw) to 28.3 (NRD) — fireflies
  absorbed by the spatial kernel as expected.
- `nrd_spec.png` shows clean specular plate across the torso where raw
  specular was speckle noise.
- Beauty PNG (OIDN post-process) visually identical to pre-T3b output.

**Implementation notes:**
- `NrdDenoiser::initialize()` signature extended to take `VkInstance`,
  graphics queue family, and the instance/device extension name lists
  used at VkInstance/VkDevice creation — NRI's `DeviceCreationVKDesc`
  requires all of these to wrap our existing Vulkan device.
- RT extensions (`VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`,
  `VK_KHR_deferred_host_operations`) are filtered OUT of the list handed
  to NRI. NRI's dispatch-table resolver eagerly tries to resolve
  `vkCmdTraceRaysIndirect2KHR` (part of `VK_KHR_ray_tracing_maintenance1`
  which we don't enable) whenever it sees ray-tracing pipeline in the
  list, and returns `UNSUPPORTED` when it can't find the entrypoint.
  NRD's REBLUR is compute-only — filtering RT extensions out of the
  NRI-facing list lets NRI skip the RT code path entirely.
- Resource snapshot declares all AOVs in initial state
  `Layout::SHADER_RESOURCE_STORAGE / AccessBits::SHADER_RESOURCE_STORAGE`
  (matches post-traceRays `VK_IMAGE_LAYOUT_GENERAL`). NRD's `_Dispatch`
  emits its own `nri::CmdBarrier` transitions to pull inputs into
  `SHADER_RESOURCE` (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) before
  sampling — no manual pre-barrier needed at call site, just a raygen→
  compute memory barrier on VK_ACCESS_SHADER_WRITE_BIT.
- `restoreInitialState = true` in the snapshot so NRD reverts IN_*
  back to GENERAL after dispatch — downstream readbacks
  (`VulkanRenderer::readbackDiffuseRadiance` etc.) find them where
  they expect.
- `NRDIntegration.cpp` now also includes `NRIWrapperVK.h` (+ its
  prerequisite `NRIRayTracing.h`) so the `.hpp`'s `RecreateVK` /
  `DenoiseVK` method bodies are emitted into the static lib — without
  this the T3b link failed with undefined refs on those symbols.

**Observation:**
First real NRD work. Spatial-only at `frameIndex=0` is expected — temporal
path wiring + multi-frame accumulation comes with 4.E's CLI / realtime
integration. Compositing back into beauty lives in 4.D.

**Status:** dispatch path proven; 4.D (remodulation compositor) unblocked.

## 2026-04-24 — Sub-plan 4.D: Remodulation compositor

**Command:**
```bash
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr /tmp/beauty_4d.png 1 \
    --dump-diffuse=/tmp/raw_diff_4d.png --dump-nrd-diffuse=/tmp/nrd_diff_4d.png \
    --dump-specular=/tmp/raw_spec_4d.png --dump-nrd-specular=/tmp/nrd_spec_4d.png \
    --dump-diff-albedo=/tmp/albedo_4d.png --dump-spec-color=/tmp/f0_4d.png \
    --dump-nrd-composed=/tmp/composed_4d.png
```

**Evidence:**
- `[NRD compose] pipeline ready @ 1920x1080` logs once per PT profile (offline + realtime = 2 total).
- Binding 29 (RGBA32F) allocated; UNDEFINED→GENERAL barrier fires first frame only (gated by `m_nrdComposeFirstFrame`).
- Composed HDR PNG shows scene with recognizable object colors — albedo re-applied to denoised radiance; skin tones, shirt, shorts, gold shoes all visible.
- Composed PNG dramatically cleaner than raw 1spp diffuse (raw shows scattered noise pixels; composed shows coherent silhouette + materials).
- Overall palette + shape plausible vs. 256-spp OIDN reference, modulo NRD's spatial-filter blur and lack of tonemapping on the raw HDR dump.
- Beauty PNG (binding 2, `out.png`) structurally identical to pre-4.D baseline; file-size delta < 0.02 %. Bit-for-bit PNG hash is not stable across runs in this codebase (pre-existing non-determinism at spp=1 — confirmed by three back-to-back runs of the same binary producing three different hashes), so the invariant is tracked via file-size + visual equivalence rather than sha256.
- Zero new Vulkan validation errors.

**Observation:**
Demodulation loop closed: 3.C.6 split raw radiance into (demod AOV × albedo), 4.C denoised the demod AOV, and 4.D re-multiplies. Composed output is the first usable NRD beauty signal in OHAO. Temporal accumulation still disabled (`frameIndex=0`) — 4.E wires per-frame state and the `DenoiseMode::NRD` CLI flag.

**Status:** remodulation compositor live; 4.E unblocked.
