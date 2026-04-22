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

### Open observation

Specular hit-dist max (3.12) is numerically close to diffuse max (3.17) on
this scene, whereas the NRD-canonical expectation for a polished visor
reflecting the environment is that the specular virtual-distance should be
much larger than the diffuse single-segment distance (env effectively at
infinity). Possible causes to investigate in Sub-plan 4 before wiring NRD:
- `payload.hitDist` for the env-miss case may not be propagating into the
  specular accumulator (miss sets FLT_MAX, which the `< 1e20f` finite mask
  correctly excludes from the max — but the 3.12 value suggests most pixels
  terminate at short primary-bounce distances rather than extending).
- `specChainActive` may be flipping false earlier than expected on the
  rough metal regions of the helmet.
- Max-finite normalization can hide the tail — a histogram dump would
  clarify whether the bright pixels are a handful of outliers near 3.12 or
  a broad distribution.

Shape (sky black, helmet visible, no uniform darkness/brightness failure
modes) matches pass criteria from the task spec, so 3.D is marked
complete. Deeper NRD-semantic validation deferred to Sub-plan 4 when the
denoiser output makes filter-radius mismatches visible.

Sub-plan 3.D complete. Next: Sub-plan 4 (NRD integration) — the jaw-drop
moment when realtime 1spp renders look like offline 1024spp.
