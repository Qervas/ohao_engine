# Denoiser Pipeline Sub-plan 3.C.7 — Dual-Ray Bounce-0 Split — Design

**Date:** 2026-04-20
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline quality pass (3.C.7 of the 3.C.5 → 3.C.6 → 3.C.7 → 3.D → 4 queue)
**Predecessors:** 3.A ✅, 3.B ✅, 3.C ✅, 3.C.5 ✅, 3.C.6 ✅

---

## 1. Goal

At the primary hit, trace **two independent rays** instead of stochastically picking one lobe:

- One sampled via the diffuse lobe (cos-weighted hemisphere), accumulating into `diffContrib`.
- One sampled via the specular lobe (GGX importance sample), accumulating into `specContrib`.

Each ray runs an independent path-trace from bounce 1 onwards. `lobeMask` and its bounce-0 coin-flip disappear entirely.

Outcome: the two radiance AOVs (bindings 22 + 23) now BOTH receive sampled indirect light every frame instead of stochastically filling one and zeroing the other. NRD gets much lower-variance input at 1 spp; offline renders converge faster at low spp.

This is **the first sub-plan in the 3.x chain with a user-visible quality win** at low sample counts. At high spp (≥64) the result converges to the same image as pre-3.C.7 (both are unbiased Monte Carlo), so high-spp offline renders are not visibly different.

## 2. Non-Goals

- Ping-pong prev-depth (Sub-plan 3.D).
- NRD integration (Sub-plan 4).
- Cleanup of readback duplication / half-float decoder consolidation (Sub-plan 4 cleanup).
- Any change to beauty math beyond the sampling strategy change.
- Realtime-profile performance specialization (defer until Sub-plan 4 measures cost).

## 3. Decisions

- **Dual-ray at bounce 0 only.** Bounces ≥1 continue as single path on each of the two independently-sampled trajectories. No further splitting.
- **Shader layout: inline duplication of the indirect loop**, with per-path state prefixed `spec*` and `diff*`. A helper function would need ~10 `inout` parameters; inline is cleaner to read. Extraction parked for 4.
- **Delta-specular case (`roughness < 0.05`):** the specular path still uses pure mirror reflection (skip GGX jitter). The diffuse path still runs — even on mirrors, NEE from bounce 0 already filled diffuse analytically; the diffuse-path indirect just contributes whatever hemisphere-sampled radiance exists. Not wasted.
- **Russian roulette applies per-path** from bounce 2 onwards (same threshold rule as current code).
- **MIS state per-path.** `lastBsdfPdf` + `lastBounceWasDelta` become `specLastBsdfPdf`/`specLastBounceWasDelta` and `diffLastBsdfPdf`/`diffLastBounceWasDelta`.
- **Beauty path preserved.** `radiance` at end-of-main is `diffContrib + specContrib`. Mean matches pre-3.C.7; per-sample variance is lower.
- **All three raygens synced.** Default, offline, and realtime shaders get the same restructure.
- **Primary miss (bounce 0 no hit):** both channels stay zero (as in 3.C.6). No dual-ray work needed.

## 4. Architecture

### 4.1 Raygen main() restructured into 3 stages

```glsl
void main() {
    ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
    samplerInit(uvec2(pixel), frameIdx);
    uint dimIdx = 0u;

    // ---- Stage A: primary ray + first-hit capture + AOVs + analytic direct ----
    vec2 jitter = getSample2D(dimIdx); dimIdx += 2u;
    vec3 rayOrigin, rayDir;  // constructed from pc.invView/invProj as today

    payload.hitDist = -1.0;
    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                rayOrigin, 0.001, rayDir, 10000.0, 0);

    // Capture firstHitPos / firstHitDist / hitNormal / albedo / etc.
    // Write AOVs (binding 6 albedo, 7 normal, 19 motionVector, 20 depth, 21 roughness,
    //             24 diffuseAlbedoAOV, 25 specColorAOV). Miss path writes sentinels.

    if (payload.hitDist < 0.0) {
        // Primary miss — beauty = env. Both radiance channels stay zero (zero-init covers it).
        radiance = throughput * payload.color;
        // ... accumulation + tonemap + imageStore output ...
        imageStore(diffuseRadiance, pixel, vec4(0.0));
        imageStore(specularRadiance, pixel, vec4(0.0));
        return;
    }

    // Extract material: albedo, N, isMetal, roughness, F0, firstHitDiffAlbedo, firstHitSpecColor
    // Emissive at primary hit → diffContrib (convention from 3.C.6)
    // Analytic NEE at bounce 0: split into diffContrib + specContrib (3.C code, unchanged)
    // Analytic env-MIS at bounce 0: same split (3.C code, unchanged)

    // ---- Stage B: specular-sampled indirect path ----
    {
        // Sample specular lobe from first hit → specDir
        // specThroughput = isMetal ? albedo : vec3(1.0); /= max(specProb, 0.01)
        // specLastBsdfPdf, specLastBounceWasDelta init from the sample

        vec3 specRayOrigin = firstHitPos + N * 0.01;
        vec3 specRayDir = specDir;
        vec3 specThroughput = /* from sample */;

        for (uint bounce = 1u; bounce <= maxBounces; bounce++) {
            // Trace, NEE, env-MIS, emissive, BSDF sample → accumulate into specContrib
            // Standard inner loop, specContrib += specThroughput * contribution at each site
            // Russian roulette on specThroughput from bounce == 2u onwards
        }
    }

    // ---- Stage C: diffuse-sampled indirect path ----
    {
        // Sample diffuse lobe from first hit (cosine-hemisphere) → diffDir
        // diffThroughput = albedo; /= max(1.0 - specProb, 0.01)
        // diffLastBsdfPdf, diffLastBounceWasDelta init from the cosine PDF

        vec3 diffRayOrigin = firstHitPos + N * 0.01;
        vec3 diffRayDir = diffDir;
        vec3 diffThroughput = /* from sample */;

        for (uint bounce = 1u; bounce <= maxBounces; bounce++) {
            // Identical structure to specular loop, accumulating into diffContrib
        }
    }

    // ---- Beauty sum + AOV writes ----
    vec3 radiance = diffContrib + specContrib;
    // Apply optional firefly clamp + accumulation as today
    imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));  // raw; 3.C.6 semantics
    imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
    // ACES tonemap + outputImage write unchanged
}
```

### 4.2 Removed state

Delete entirely:
- `uint lobeMask` local
- `if (bounce == 0u) lobeMask = 0u/1u;` stamps (currently at both BSDF-sample branches)
- All `else if (lobeMask == 0u) { ... } else { ... }` attribution branches in NEE / env-MIS / emissive / env-miss blocks
- The single-loop outer bounce counter 0..maxBounces (replaced by Stage A + two Stage-B/C inner loops over 1..maxBounces)

### 4.3 Inline duplication sites

Stage B and Stage C contain **textually duplicated inner loops**. The loop body does:

- `traceRayEXT` with current `{spec,diff}RayOrigin` / `Dir`
- Miss → env-miss contribution: `{spec,diff}Contrib += {spec,diff}Throughput * payload.color * envMisWeight;` and `break`
- Emissive: `{spec,diff}Contrib += {spec,diff}Throughput * emissive;`
- NEE (full implementation from current raygen; whichever lobe's light contribution goes fully to THIS channel — no per-bounce split logic, unlike 3.C.6's bounce-0 analytic split)
- Env-MIS (default + offline only): same contribution goes fully to THIS channel
- Russian roulette: applied to `{spec,diff}Throughput` from bounce ≥2
- BSDF sample: updates `{spec,diff}Ray{Origin,Dir}` + `{spec,diff}Throughput`; tracks `{spec,diff}LastBsdfPdf`/`{spec,diff}LastBounceWasDelta`

Both loops are ~130 lines of near-identical GLSL. Keeping them inline (rather than a helper) avoids a function with 10+ `inout` parameters and preserves the "3 raygens stay easy to diff" discipline.

### 4.4 Stage-A first-hit AOVs

All existing AOV writes remain at Stage A (bounce 0 == primary hit):
- Binding 6 (albedoAOV): unchanged
- Binding 7 (normalAOV): unchanged
- Binding 19 (motionVector): unchanged
- Binding 20 (depth): unchanged
- Binding 21 (roughness): unchanged
- Binding 24 (diffuseAlbedoAOV): unchanged
- Binding 25 (specColorAOV): unchanged

Binding 22 (diffuseRadiance) and 23 (specularRadiance) are written ONCE at the end of main() with final `diffContrib` and `specContrib` — same as 3.C.6.

### 4.5 Primary miss path

If bounce-0 traceRayEXT misses (camera ray into sky):
- `radiance = throughput * payload.color` (env radiance)
- `diffContrib = specContrib = 0` (zero-init covers it; Stage B and C never run)
- Both radiance AOVs written with `vec4(0.0)`
- Tonemap + outputImage as today

This is strictly simpler than today's bounce==0 miss handling (no need to track which lobe was chosen — primary miss has no lobe).

### 4.6 NEE / env-MIS contribution accounting

At bounce 0 (Stage A): contribution is analytically split using the existing diff/spec BRDF terms (same code as 3.C).

At bounce ≥ 1 (Stage B/C): contribution goes **fully** to the path's own channel. The BRDF at bounce ≥ 1 includes both lobes (regardless of which lobe the first-hit ray chose), but since the downstream-light attribution is by the bounce-0 lobe choice only, we keep the 3.C convention of attributing all post-bounce-0 radiance to the starting lobe.

This is a slight simplification vs. "pure Monte Carlo per bounce" but matches the NRD-canonical primary-surface split semantics.

### 4.7 Offline vs realtime vs default raygens

All three raygens restructure identically. The realtime raygen's inner loops are shorter (no env-MIS block) but otherwise mirror the default/offline structure.

Realtime profile accepts the ~1.5× bounce-cost increase (maxBounces=1 → 2 → 3 rays). If NRD integration in 4 reports unacceptable frame time, a specialization constant can gate single-ray vs dual-ray. Parked.

### 4.8 Sampler dimension budget

The current raygen uses the sampler API with `dimIdx` incrementing per-sample. Stage B and Stage C must not share dim indices — one path's noise would correlate with the other's, defeating the variance-reduction goal.

Strategy: after Stage A, partition the remaining budget. The simplest approach: Stage B consumes dims starting at some offset, Stage C starts at a later offset. Concretely:

```glsl
// After Stage A: dimIdx = D_end_of_A (some value)
uint specDimBase = D_end_of_A;
uint diffDimBase = specDimBase + 64u;  // reserve 64 dims for spec path (4 bounces × ~15 dims/bounce)
```

For Sobol / Owen-scrambled sequences, this partitioning gives decorrelated low-discrepancy samples to each path. Must be consistent across frames.

Alternative: use two sampler state objects, each `samplerInit` with a different seed. Simpler but changes sampler semantics. Stick with dim-partitioning for YAGNI.

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `shaders/rt/pt_raygen.rgen` | Main restructure: Stage A primary + Stage B spec indirect + Stage C diff indirect. Delete `lobeMask` + all bounce-0 stamp sites + all bounce-≥1 attribution branches. Per-path MIS state locals. Sampler dim partitioning. |
| `shaders/rt/pt_raygen_offline.rgen` | Verbatim mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Same restructure, shorter inner loops (no env-MIS). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C.7 entry with before/after 1spp visual comparison. |
| `examples/env_demo.cpp` | No change (AOV bindings + dump logic unchanged; just observes different values). |
| `ohao/render/rt/path_tracer.cpp` | No change (no new bindings; descriptor layout unchanged). |
| `ohao/render/rt/path_tracer.hpp` | No change. |
| `ohao/gpu/vulkan/renderer.*` | No change (no new AOVs). |

**Scope is shader-only** — no C++ touches. First sub-plan in 3.x with this property.

## 6. Verification

### 6.1 Low-spp visual win (THE verification)

- **Pre-3.C.7 reference:** render helmet + env_studio at **1 spp, `--denoise=none`**, save.
- **Post-3.C.7:** same setup, save.
- Expected: post-3.C.7 specular channel shows env reflection on visor on every pixel (not random subset); diff channel shows ambient on every matte pixel. Grain level visibly lower.
- Pan test (move camera 1 pixel between frames): pre-3.C.7 shows "specular flicker" as lobe choice shuffles; post-3.C.7 shows stable env reflection.

### 6.2 High-spp convergence regression

- **Pre vs post at 256 spp:** beauty images should be visually identical within Monte Carlo noise. A CPU per-pixel RMSE ≤ 2% is expected (both are unbiased estimators of the same integral).
- If RMSE > 2%: investigate — likely a math bug in one of the inner loops.

### 6.3 AOV sanity

- `--dump-diffuse` / `--dump-specular`: both channels now have content at low spp (not one zero + one full).
- `--dump-diff-albedo` / `--dump-spec-color`: unchanged from 3.C.6 (same values, same visuals).

### 6.4 Denoise-mode regression

- `--denoise=none`: beauty as above.
- `--denoise=oidn`: OIDN takes beauty + albedo + normal; not touched by 3.C.7 — output should be ~identical (minor RNG variation from dim partitioning).
- `--denoise=optix`: same as OIDN.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| Shader file grows ~100 lines per raygen (3×) | Accepted. Inline duplication beats helper-function parameter explosion. Extract in Sub-plan 4 if needed. |
| MIS state accidentally shared between paths | Distinct `spec*` / `diff*` prefixes on ALL path-scoped locals. Review-checklist item. |
| Sampler dim correlation between paths | Partition `dimIdx` with 64-dim gap. Sobol/Owen decorrelates within each partition. |
| Bounce-0 NEE/env-MIS double-counted? | NO — analytic split at Stage A is atomic (runs once). Stage B/C loops start at bounce 1. |
| Russian roulette path termination | Applied to `specThroughput` and `diffThroughput` independently from bounce ≥2. Standard. |
| Delta-specular metal (chrome) exhibits new noise in diffuse channel | Diffuse path still runs; metal's albedo-scaled throughput × cos-sampled dir gives some (small) radiance. At `firstHitDiffAlbedo = 0` the diffuse AOV demodulates to 0 anyway (from 3.C.6 floor semantics). Net: visually null. |
| Roughness-0.05 threshold behavior | Same as today: specular path below threshold uses pure mirror reflection, skips GGX jitter. Diffuse path always uses cos-hemisphere. |
| Cost doubles at maxBounces — realtime frame time | Realtime typically maxBounces=1, so cost 2 rays → 3 rays (+50%). Acceptable. Monitor in Sub-plan 4. |

## 8. Out-of-scope (Parking Lot)

- Realtime-profile specialization (spec constant) to toggle dual-ray off for perf.
- Extract indirect-path inner loop into shared GLSL helper (Sub-plan 4 cleanup, bundled with readback helper extraction).
- Consolidate path-trace state into a struct passed to a function.
- Writing separate AOV channels for "diffuse direct" vs "diffuse indirect" vs "specular direct" vs "specular indirect" (NRD's REBLUR variant supports this; out of scope for initial NRD integration).
- Hit-distance AOVs for NRD specular shadow reconstruction (may be needed in Sub-plan 4).

## 9. Success Criteria

1. All three raygens restructured: Stage A + Stage B + Stage C.
2. `lobeMask` and its attribution branches fully removed.
3. Per-path MIS state (`specLastBsdfPdf`/`Delta`, `diffLastBsdfPdf`/`Delta`) declared and maintained independently.
4. Sampler dim partitioning prevents inter-path correlation.
5. Low-spp (1 spp) helmet render shows both diff + spec channels filled on every pixel — no one-channel-zero artifacts.
6. High-spp (256 spp) beauty RMSE ≤ 2% vs pre-3.C.7.
7. `--dump-diffuse` + `--dump-specular` at 16 spp show visually cleaner results (less flicker) than pre-3.C.7.
8. Build clean; shader compiles without warnings beyond the pre-existing set.
9. No validation errors on Cornell + env_demo smoke.
10. Verification log updated with before/after 1spp comparison images.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the detailed implementation plan.
