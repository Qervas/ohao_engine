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
