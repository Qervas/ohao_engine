# Phase 1 Feature 1.2 — Owen-Scrambled Sobol Sampler — Design

**Date:** 2026-04-17
**Status:** Approved design, pending implementation plan
**Phase:** Rendering roadmap Phase 1 (Offline Cycles-class), Feature 1.2

---

## 1. Goal

Replace the offline path tracer's PCG pseudo-random sampler with an Owen-scrambled Sobol low-discrepancy sequence. Same per-sample cost, meaningfully lower variance per sample — matches the sampling quality of Cycles, Arnold, Renderman.

Architect the sampler as a pluggable interface so future samplers (blue noise, Halton, neural) can slot in without touching the shading code.

## 2. Non-Goals

- Blue noise texture or screen-space blue-noise error diffusion (future, Phase 2 territory)
- Realtime profile change — realtime keeps PCG for now; blue noise lands there with the Phase 2 denoiser work
- Neural or learned importance sampling
- Generalized rewriting of the shading loop

## 3. Decisions (from brainstorming)

- **Sampler:** Owen-scrambled Sobol (matches Cycles/Arnold; production SOTA, not absolute academic frontier)
- **Scope:** offline-only; realtime keeps PCG
- **Selection mechanism:** Vulkan specialization constants (one SPIR-V variant per sampler, zero runtime branch cost, clean engine-tier abstraction)
- **Framework shape:** enum-driven, pluggable — multiple samplers co-exist in the codebase, per-profile default, runtime-selectable

## 4. Architecture

### 4.1 C++ side

```cpp
// ohao/render/rt/sampler_types.hpp
enum class SamplerType : uint32_t {
    PCG    = 0,   // legacy fallback, realtime default
    Sobol  = 1,   // Owen-scrambled, offline default
    // future: BlueNoise = 2, Halton = 3, ZHBlueNoise = 4 ...
};

struct RTRenderSettings {
    // ... existing fields ...
    SamplerType samplerType{SamplerType::Sobol};  // offline default; realtime overrides to PCG
};
```

Pipeline creation sets up `VkSpecializationInfo` keyed off `samplerType`. The SPIR-V gets specialized per sampler — different pipelines for different samplers, cached by sampler type in the renderer.

### 4.2 GLSL side — sampler interface

Single header that every sampler satisfies:

```glsl
// shaders/includes/rt/sampler_api.glsl
layout(constant_id = 0) const uint SAMPLER_TYPE = 1u;  // default: Sobol

void  samplerInit(uvec2 pixel, uint sampleIdx);
float sampler1D(uint dim);
vec2  sampler2D(uint dim);
```

Implementation strategy: because `SAMPLER_TYPE` is a specialization constant (compile-time from the compiler's perspective), the sampler API functions dispatch via `if (SAMPLER_TYPE == PCG) { ... } else if (SAMPLER_TYPE == SOBOL) { ... }` and the SPIR-V optimizer collapses that to one branch. Zero runtime cost per call.

### 4.3 Sampler implementations

**`shaders/includes/rt/sampler_pcg.glsl`** — existing PCG code extracted into `samplerInit_pcg`, `sampler1D_pcg`, `sampler2D_pcg`. No behavior change. Preserves realtime profile as-is.

**`shaders/includes/rt/sampler_sobol.glsl`** — new Owen-scrambled Sobol:

- 4 dimensions precomputed (Joe-Kuo `new-joe-kuo-6.21201` direction numbers, first 4 columns)
- Direction numbers stored as `const uint kSobolDirs[32 * 4]` in the include or uploaded as SSBO (decided in plan — compile-time constants preferred for no binding cost)
- Per-bounce scramble seed = `hash(pixel.x, pixel.y, bounceIdx)` — decorrelates pixels (blue-noise-like spectrum of per-pixel error)
- Owen scrambling = nested uniform scrambling via bit permutation (use GLSL `bitfieldReverse` + XOR with hashed bits)
- 2D samples pull from consecutive 2D pads within the 4D pattern

**`shaders/includes/rt/sampler_sobol_tables.glsl`** — the direction-number constants; generated once offline from the Joe-Kuo reference and committed. See `ohao/render/rt/sobol_tables.{hpp,cpp}` for the generator.

### 4.4 Dimension strategy (Cycles-style padded 4D)

Per-pixel, per-sample, we need dimensions for:

| Use site | Dims |
|---|---|
| Sub-pixel jitter (bounce 0 only) | 2 |
| NEE light select | 1 |
| NEE light sample | 2 |
| Env MIS sample | 2 |
| BSDF choice (diff vs spec) | 1 |
| BSDF direction sample | 2 |
| Russian roulette (bounces ≥ 2) | 1 |

Per bounce: ~11 dims. With 4 bounces + primary: ~45 unique dims.

Rather than build a 45-dim Sobol table (quality degrades past ~8D for naive Sobol), we use **padded 4D**: the same 4D Sobol pattern is reused per bounce with a bounce-specific Owen scramble. Each 4D sample yields two independent 2D samples + two 1D samples — enough to cover one full bounce's needs by pulling multiple pads if necessary.

This is what Cycles does. It's theoretically slightly weaker than a true high-dim Sobol but in practice produces indistinguishable results and is cache-friendly, table-light, and bug-resistant.

## 5. Integration Points

### 5.1 Replace PCG usage in offline raygen

In `pt_raygen.rgen` and `pt_raygen_offline.rgen`, replace `rand01()` calls with the sampler API:

```glsl
// Old:
float u = rand01();
vec2  uv = vec2(rand01(), rand01());

// New:
float u = sampler1D(dimIdx);  dimIdx += 1u;
vec2  uv = sampler2D(dimIdx); dimIdx += 2u;
```

A single `uint dimIdx` tracks the current dimension within the current bounce's pad. Each bounce resets to 0 (with fresh scramble seed) or continues from a known offset — decided in the plan.

### 5.2 Miss shader

`pt_miss.rmiss` uses no random values today (only env map lookup). No change needed.

### 5.3 Closest-hit / any-hit

No random sampling today. No change.

### 5.4 Realtime raygen

`pt_raygen_realtime.rgen` keeps PCG. It still uses the sampler API, just with `SamplerType::PCG` specialization. Zero behavior change.

## 6. Pipeline and Specialization

At pipeline creation time (in `PathTracer::createRTPipeline`), the renderer builds a `VkSpecializationMapEntry` mapping `constant_id = 0` to the `SamplerType` value. Pipeline cache keys include the sampler type. First render with a new sampler type incurs a one-time pipeline compile; after that it's warm.

Profile defaults:
- `kOfflineRTSettings.samplerType = SamplerType::Sobol`
- `kRealtimeRTSettings.samplerType = SamplerType::PCG`

Users can override per-render via `RTRenderSettings`.

## 7. Verification

### 7.1 Unit tests (CPU)

`tests/renderer/sobol_test.cpp`:

- `SobolFirstSamples` — generate first 16 2D Sobol points (unscrambled), compare to known Joe-Kuo reference values.
- `OwenScrambleDeterminism` — same pixel + same bounce → identical sequence across runs.
- `OwenScrambleDecorrelation` — adjacent pixels have low cross-correlation (proxy for the per-pixel decorrelation that gives the blue-noise-ish error spectrum).
- `DirectionNumberTable` — precomputed table matches the generator output byte-for-byte.

### 7.2 Integration test (GPU)

On `envlit_turntable` at 16 spp:

- Sobol 16 spp local variance must be lower than PCG 16 spp local variance (measured via `compare_variance.py`).
- Expected reduction: 5-15% based on Cycles' published benchmarks for similar scenes.
- No visible structured grid patterns or aliased artifacts in the image (manual visual check).

### 7.3 Regression

- `cornell_box 16 spp` renders correctly with Sobol default.
- `model_viewer 1 deferred` unaffected (deferred pipeline doesn't use path tracer's sampler).
- `env_demo DamagedHelmet 16 spp` — matches or beats current MIS-on-PCG reference.
- Update `tests/reference_scenes/custom/envlit_turntable/reference.png` to the Sobol version; add a verification log entry comparing Sobol vs PCG numerically.

### 7.4 SPP ceiling

Sobol index fits 32 bits; sample count up to 2³² is fine. Owen scramble seed is also 32-bit. If someone renders > 2³² samples (astronomical — 4 billion spp), wrapping is benign because Owen-Sobol naturally repeats the pattern with new scramble. No special handling needed.

## 8. Risks

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Specialization constant GLSL quirks (compiler not folding the branch) | Low | Benchmark both variants; worst case inline the PCG and Sobol paths explicitly |
| Direction-number tables wrong (transcription errors) | Medium | Unit test against Joe-Kuo reference values; CPU-generated table matches a committed reference digest |
| Pipeline cache growth (one extra pipeline per sampler type) | Low | Only 2 sampler types today; VkPipelineCache persists across runs |
| Owen scramble seed collision → non-decorrelated pixels | Low | Scramble seed hash uses 64-bit intermediate; unit-tested decorrelation |
| Visual regression on edge scenes (caustics, dense NEE) | Medium | Keep MIS-on cornell + helmet reference renders; ship PCG as fallback via `RTRenderSettings.samplerType = PCG` |

## 9. Out-of-Scope (Parking Lot)

Tracked for later iterations:

- **Blue noise error diffusion (Heitz 2019 / Ahmed-Wonka 2020)** — layer on top of Sobol, perceptually cleaner at low spp. Add as a 1.2b refinement OR wait for Phase 2 denoiser work.
- **Progressive Multi-Jittered (PMJ02)** — alternate sampler choice; adds another enum value once scaffold exists.
- **Neural importance sampling** — far-future research integration.
- **Realtime sampler upgrade** — Phase 2 denoiser infrastructure work.

## 10. Success Criteria

1. Offline raygen renders `envlit_turntable` at 16 spp with local variance 5-15% lower than current MIS-on-PCG.
2. Unit tests pass: Sobol table correctness, Owen scramble determinism, per-pixel decorrelation.
3. `RTRenderSettings::samplerType` is visible as a pluggable selection — changing it swaps the sampler with no code edits in shading logic.
4. Realtime profile renders identically to pre-1.2 (PCG preserved, zero regression).
5. Committed Sobol reference render updates `envlit_turntable/reference.png`; verification log shows Sobol vs PCG variance delta.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the detailed, bite-sized implementation plan (tasks, file lists, TDD steps, verification gates).
