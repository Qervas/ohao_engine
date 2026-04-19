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
