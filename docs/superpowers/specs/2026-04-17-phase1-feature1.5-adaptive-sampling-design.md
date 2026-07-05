# Phase 1 Feature 1.5 — Adaptive Sampling — Design

**Date:** 2026-04-17
**Status:** Approved design, pending implementation plan
**Phase:** Rendering roadmap Phase 1 (Offline Cycles-class), Feature 1.5

---

## 1. Goal

Per-pixel sample budget. Each pixel samples only as many rays as it needs to hit a confidence threshold, then early-outs. Easy pixels (sky, matte surfaces) stop fast; hard pixels (caustics, rough metal under env) keep going up to a cap. Same total compute, dramatically better quality — Cycles and Arnold report 3–10× effective sample-count improvement on typical scenes.

Unbiased: no radiance clipping, no bias toward any image property. This simply redistributes compute to where it's needed.

## 2. Non-Goals

- Tile-based convergence (coarser granularity; Phase 2)
- Temporal adaptive (reuse convergence state across frames; Phase 2)
- GBuffer-guided adaptive (use surface normal / depth to spatially predict convergence; Phase 3)
- Applying to realtime profile — realtime renders at fixed 1–2 spp per frame, no budget to redistribute

## 3. Decisions (from brainstorming)

- **Variance estimator:** Welford running luminance variance (exact, numerically stable, matches Cycles)
- **Early-out:** per-pixel done-mask (R8UI storage image). Raygen reads mask first thing and returns if set. Modest warp divergence is acceptable; ray traces (the expensive part) are fully skipped for converged pixels
- **Threshold:** relative (confidence-interval / mean) with `minSamples` guard so the noise estimate is trustworthy before we trust it
- **Scope:** offline profile only. Realtime keeps the existing fixed-budget accumulation
- **Defaults (offline):** `minSamples=8, maxSamples=1024, threshold=0.01`

## 4. Architecture

### 4.1 New GPU buffers

Added alongside the path tracer's existing resources:

| Binding | Type                 | Purpose                                                   |
|---------|----------------------|-----------------------------------------------------------|
| 19      | R32F storage image   | Welford M2 — running sum of squared luminance deviations  |
| 20      | R8UI storage image   | Per-pixel done flag (0 = active, 1 = converged)           |

Storage-image cost at 1920×1080: ~8.3MB (4B) + ~2.1MB (1B) ≈ 10MB. Negligible.

### 4.2 C++ side

Extend `RTRenderSettings`:

```cpp
struct RTRenderSettings {
    // ... existing fields ...
    bool     adaptiveEnabled{false};
    uint32_t adaptiveMinSamples{8};
    uint32_t adaptiveMaxSamples{1024};
    float    adaptiveThreshold{0.01f};
};
```

Preset defaults:

- `kOfflineRTSettings.adaptiveEnabled = true` (use defaults for min / max / threshold)
- `kRealtimeRTSettings.adaptiveEnabled = false`

Push constants extended:

```cpp
struct PTPushConstants {
    // ... existing fields ...
    uvec4 adaptive;   // x = enabled, y = minSamples, z = maxSamples, w = unused
    vec4  adaptiveF;  // x = threshold, yzw = unused
};
```

The existing push constants total 240 bytes (within the Vulkan 256-byte minimum). Adding one `uvec4` + one `vec4` brings the total to 272 bytes, which exceeds the minimum. **Resolution:** repurpose the remaining unused lanes in `control.w`/`tuning.w` or a currently-unused channel before adding new fields. The plan will allocate exact lanes.

### 4.3 GLSL shader changes (offline raygen only)

At the top of `main()`, before any ray tracing:

```glsl
if (adaptiveEnabled && imageLoad(doneMask, pixel).r != 0u) {
    return;  // pixel converged in a previous sample — skip entirely
}
```

After existing radiance → accumBuffer update, add Welford step:

```glsl
// Luminance of the fresh sample (before accumulation)
float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));

// Welford running variance: online update of M2
// count/prevMean come from the existing accumBuffer; M2 from the new buffer.
uint count = uint(acc.w);  // post-update count
vec3 newMean = acc.rgb;    // post-update mean
float newMeanLum = dot(newMean, vec3(0.2126, 0.7152, 0.0722));
float prevMeanLum = newMeanLum - (lum - newMeanLum) / float(count);
float delta  = lum - prevMeanLum;
float delta2 = lum - newMeanLum;
float M2 = imageLoad(varianceM2, pixel).r + delta * delta2;
imageStore(varianceM2, pixel, vec4(M2, 0, 0, 0));

if (adaptiveEnabled && count >= minSamples) {
    float variance = M2 / float(count - 1);
    float sigma    = sqrt(max(variance, 0.0));
    float halfCI   = sigma / sqrt(float(count));
    if (halfCI < threshold * max(newMeanLum, 1e-3)) {
        imageStore(doneMask, pixel, uvec4(1));
    }
}
```

The Welford update writes `M2 := M2_prev + (x - prevMean) * (x - newMean)`; sample variance is `M2 / (count - 1)`. Standard, numerically stable.

### 4.4 Reset semantics

`PathTracer::resetAccumulation()` must now clear three things:

1. `accumBuffer` count channel (existing)
2. `varianceM2Buffer` (new — zero it)
3. `doneMaskBuffer` (new — zero = all active)

Trigger: camera change, scene change, settings change (sampler flip, threshold tune).

### 4.5 Instrumentation AOV (optional, ships enabled)

Expose per-pixel sample count as a debug AOV so the user can see "heat map" of where the renderer spent effort. The existing `accumBuffer.w` already stores this — just need a CPU-accessible copy path when `RTRenderSettings.emitSampleCountAOV` is set. Wire to one of the existing AOV outputs for visualization (albedoAOV or normalAOV aren't used when adaptive debugging is on; we can alias or add a new output).

**Keep simple:** Phase 1 ships without the heatmap AOV; it's a Phase 2 debug feature. For Phase 1 verification, read back `accumBuffer.w` via a one-shot CPU compute pass in the test harness.

## 5. Integration Points

### 5.1 Realtime raygen unchanged

`pt_raygen_realtime.rgen` doesn't include the adaptive check. Its `RTRenderSettings.adaptiveEnabled` is false, push constant reflects that, but the shader simply skips the entire branch.

### 5.2 Offline raygen (pt_raygen.rgen / pt_raygen_offline.rgen)

Both files gain:

- Declarations for bindings 19 (varianceM2) and 20 (doneMask)
- Early-out at top of `main()` if done
- Welford update + convergence check after accumulation

The two files remain in sync (current design contract).

### 5.3 Pipeline + descriptor set

`createDescriptorResources()`:

- `bindings[]` grows from 19 entries to 21 entries
- Storage image pool size incremented by 2
- Descriptor set writes extended for bindings 19, 20

`createRTPipeline()`:

- Storage image counts updated in the pool-size calc
- No new specialization constants needed

### 5.4 Renderer-side ownership

`PathTracer` owns the new images:

```cpp
VkImage        m_varianceM2Image;     VkDeviceMemory m_varianceM2Memory;     VkImageView m_varianceM2View;
VkImage        m_doneMaskImage;       VkDeviceMemory m_doneMaskMemory;       VkImageView m_doneMaskView;
```

Created in `createImages()`, destroyed in `destroyImages()`. Sized to match output resolution; re-created on `resize()`.

## 6. Verification

### 6.1 Unit test (CPU)

`WelfordVarianceTest`: feed a known sequence of samples into a CPU Welford, check the variance matches the closed-form result for that sequence. Standard, 15-minute test.

### 6.2 Integration test (GPU)

On `envlit_turntable` DamagedHelmet + env_studio:

- **Baseline:** `adaptiveEnabled=false, maxSamples=64` (uniform Sobol + MIS) — record avg variance, RMSE vs 4096 spp truth.
- **Adaptive:** `adaptiveEnabled=true, minSamples=8, maxSamples=128, threshold=0.01` — record avg samples/pixel (from accumBuffer.w mean), RMSE vs truth, total render time.

**Pass criteria:**

1. Adaptive average sample count substantially lower than baseline's uniform 64.
2. Adaptive RMSE vs truth ≤ baseline RMSE (adaptive matches or beats uniform at equal *compute* budget).
3. Visually: adaptive looks like higher-spp render, not like lower-spp.

Numbers expected (Cycles benchmarks as a sanity check): ≥2× effective samples for the same wall-clock time, or equivalently ≥30% RMSE reduction at matched render time.

### 6.3 Reset correctness

On `resetAccumulation()`, confirm both new buffers clear. Sanity test: take a render, change camera, verify adaptive doesn't immediately mark the first frame done (which would happen if done-mask wasn't cleared).

### 6.4 Heatmap spot check

After a render, read back `accumBuffer.w` to a Python array and save as grayscale PNG. Expected: sky and matte surfaces show low counts (close to minSamples); glossy metal and env reflection areas show high counts (close to maxSamples). This isn't a hard test, but it visually confirms the feature is doing what it should.

## 7. Risks

| Risk                                                              | Likelihood | Mitigation                                   |
|-------------------------------------------------------------------|-----------|----------------------------------------------|
| Welford underflow/overflow at large N                             | Low       | `maxSamples=1024`, float32 safe up to ~2²⁴   |
| Warp divergence hurts perf on mostly-converged frames             | Low       | Still strictly better than rendering all pixels; Phase 2 tile-based version addresses |
| Threshold too aggressive — premature stop → blotchy result        | Medium    | `minSamples=8` guard, tunable via settings  |
| Race between accumBuffer update and Welford read                  | None      | One invocation per pixel per sample; serial |
| Push-constant size overflow (240 → 272 bytes > 256 minimum)       | Medium    | Plan allocates lanes in existing `control`/`tuning` rather than new fields |
| Heatmap readback adds CPU sync point                              | Low       | Only used for verification, not production  |

## 8. Success Criteria

1. Offline render with adaptive enabled at `maxSamples=128, threshold=0.01` produces an RMSE vs truth that beats uniform Sobol at the same average sample count.
2. On the `envlit_turntable` scene, adaptive average sample count is at least 30% lower than the max for equivalent quality.
3. Realtime profile is unaffected (adaptive disabled).
4. Unit test for Welford math passes.
5. `resetAccumulation()` correctly clears both new buffers.
6. `RTRenderSettings::adaptiveEnabled` is a clean on/off toggle — works in both states.

## 9. Next Step

Invoke `superpowers:writing-plans` to generate the detailed, bite-sized implementation plan with TDD steps and verification gates.
