# Denoiser Pipeline Sub-plan 1 — OIDN Switch — Design

**Date:** 2026-04-18
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline (inserted 2026-04-18 before remaining Phase 1 items)
**Sub-plan:** 1 of 5 (OIDN → OptiX → realtime-denoiser foundation → NRD → DLSS RR)

---

## 1. Goal

Wire the already-integrated Intel OpenImageDenoise (OIDN) as a first-class engine toggle. Offline profile denoises by default; CLI flag lets users override per-run for A/B testing. Introduces the `DenoiseMode` enum that Sub-plans 2–5 will extend.

The goal of the full denoiser pipeline is "clean low-spp renders" — the real answer to our noise-at-64-spp problem. Sub-plan 1 is the fastest visible win; Sub-plans 2–5 add alternative denoisers.

## 2. Non-Goals

- Progressive per-sample denoise (viewer feature, different scope)
- Alternative denoisers (OptiX lands in Sub-plan 2; NRD/DLSS RR in Sub-plans 4–5)
- GPU-resident denoise (OIDN 2.x has a GPU device — optional optimization for later)
- Restructuring the full `ohao/render/rt/` tree (wider refactor is a separate task)

## 3. Decisions (from brainstorming)

- **Integration point:** engine API on `VulkanRenderer` (not per-example code)
- **When it runs:** after all samples accumulated (final-only)
- **User control:** both — `RTRenderSettings.denoiseMode` default plus `--denoise=oidn|none` CLI override
- **Architecture:** introduce `ohao/render/rt/denoise/` subfolder; move `oidn_denoise.*` into it; new `denoise_types.hpp` lives there; future denoisers join it
- **Default for offline:** `DenoiseMode::OIDN` (Cycles matches this — offline always denoises)
- **Default for realtime:** `DenoiseMode::None` (NRD/DLSS RR are the realtime path, not OIDN)

## 4. Architecture

### 4.1 Directory reorganization

```
ohao/render/rt/
├── denoise/                            NEW
│   ├── denoise_types.hpp               NEW — DenoiseMode enum
│   ├── oidn_denoise.hpp                moved from render/rt/
│   └── oidn_denoise.cpp                moved from render/rt/
├── path_tracer.{hpp,cpp}               unchanged
├── env_cdf.{hpp,cpp}                   unchanged
├── ...
```

All `#include "render/rt/oidn_denoise.hpp"` paths update to `#include "render/rt/denoise/oidn_denoise.hpp"`. Callers: `examples/model_viewer.cpp`, any other file that references it.

### 4.2 `DenoiseMode` enum

```cpp
// ohao/render/rt/denoise/denoise_types.hpp
#pragma once
#include <cstdint>

namespace ohao {

enum class DenoiseMode : uint32_t {
    None  = 0,   // Raw path-traced output, no denoise
    OIDN  = 1,   // Intel OpenImageDenoise (CPU/GPU, open source)
    // future:
    // OptiX = 2,
    // NRD   = 3,  // realtime (requires motion vectors + history)
    // DLSSRR = 4, // realtime (requires DLSS SDK)
};

// Parse from CLI string. Returns None for unknown values and logs a warning.
DenoiseMode parseDenoiseMode(const std::string& s);

// Human-readable name for logging.
const char* denoiseModeName(DenoiseMode mode);

} // namespace ohao
```

Parse helper avoids duplicating CLI-string logic in every example.

### 4.3 `RTRenderSettings` field

```cpp
struct RTRenderSettings {
    // ... existing ...
    DenoiseMode denoiseMode{DenoiseMode::None};
};
```

Preset defaults:
- `kOfflineRTSettings.denoiseMode = DenoiseMode::OIDN`
- `kRealtimeRTSettings.denoiseMode = DenoiseMode::None`

### 4.4 `VulkanRenderer` API

```cpp
class VulkanRenderer {
public:
    void        setDenoiseMode(DenoiseMode mode);
    DenoiseMode getDenoiseMode() const;

    // Existing — semantics extended:
    // If denoiseMode != None, the returned buffer is the denoised version.
    // Denoise runs lazily on first call after render(), result cached.
    const uint8_t* getPixels() const;
};
```

Lifecycle:

1. `render()` → populates GPU accumBuffer + AOVs, invalidates `m_denoiseCache`
2. `getPixels()` first call after render:
   - if `denoiseMode == None` → return existing CPU pixel buffer (no-op, as today)
   - if `denoiseMode == OIDN` → readback HDR buffers, convert to float3, call `oidnDenoise`, tonemap + store into `m_denoiseCache`, return
3. Subsequent `getPixels()` calls → return cached

Cache invalidation: every `render()` call flips `m_denoiseCacheValid = false`.

Method `getPixels()` remains `const` — the cache is declared `mutable`, or a separate internal method does the denoise and `getPixels()` is the public accessor.

### 4.5 Error handling

`oidnDenoise` returns `bool`. On failure:

```cpp
std::cerr << "[Denoise] OIDN failed — returning noisy pixels\n";
// Fall back: return raw CPU pixel buffer (untouched by denoise pipeline)
return m_pixelBuffer.data();
```

Never crashes the render. User sees noisy image and a warning — they know something went wrong.

### 4.6 CLI integration

Each of the 4 examples (`cornell_box`, `env_demo`, `model_viewer`, `turntable`) gains one argument parser:

```cpp
DenoiseMode denoiseOverride = DenoiseMode::None;   // sentinel: use preset default
bool denoiseOverrideSet = false;
for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg.rfind("--denoise=", 0) == 0) {
        denoiseOverride = parseDenoiseMode(arg.substr(10));
        denoiseOverrideSet = true;
    }
    // ... existing arg parsing ...
}

// After renderer.setScene(scene):
if (denoiseOverrideSet) renderer.setDenoiseMode(denoiseOverride);
// else: preset default (OIDN for offline, None for realtime)
```

The existing `constexpr bool kEnableOIDN = false;` pattern in `model_viewer.cpp` is removed — replaced with the new API.

### 4.7 Usage examples

```bash
# Offline default — OIDN auto-enabled
./build/env_demo model.glb env.hdr out.png 16

# Explicit disable (for noisy reference)
./build/env_demo model.glb env.hdr out_noisy.png 16 --denoise=none

# Realtime via interactive — adjust via code if needed
```

## 5. File structure changes

**New files:**
- `ohao/render/rt/denoise/denoise_types.hpp`

**Moved files:**
- `ohao/render/rt/oidn_denoise.hpp` → `ohao/render/rt/denoise/oidn_denoise.hpp`
- `ohao/render/rt/oidn_denoise.cpp` → `ohao/render/rt/denoise/oidn_denoise.cpp`

**Modified files:**
- `ohao/render/rt/path_tracer.hpp` — RTRenderSettings + preset defaults gain `denoiseMode`
- `ohao/gpu/vulkan/renderer.hpp` — add `setDenoiseMode`/`getDenoiseMode` + private cache fields
- `ohao/gpu/vulkan/renderer.cpp` — implement `getPixels()` lazy-denoise path
- `ohao/render/rt/CMakeLists.txt` (or top-level build scripts) — update source paths for the moved files
- `examples/{cornell_box,env_demo,model_viewer,turntable}.cpp` — add `--denoise=…` CLI arg + call `setDenoiseMode`
- `examples/model_viewer.cpp` — remove `constexpr bool kEnableOIDN = false` + manual readback block

## 6. Verification

### 6.1 Regression — default behavior unchanged for `--denoise=none`

```bash
./build/env_demo model env.hdr out.png 16 --denoise=none
```

Output MUST be bit-identical to pre-change render. Compare via `md5sum` against a saved baseline.

### 6.2 OIDN quality at low spp

```bash
./build/env_demo DamagedHelmet env_studio out_noisy.png 16 --denoise=none
./build/env_demo DamagedHelmet env_studio out_oidn.png  16 --denoise=oidn
python3 tools/compare_variance.py tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png out_noisy.png
python3 tools/compare_variance.py tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png out_oidn.png
```

**Pass criterion:** OIDN RMSE vs truth ≥ 30% lower than noisy at 16 spp. Typical OIDN performance is 40-60% RMSE reduction.

### 6.3 Cornell box at 16 spp

Current Cornell at 16 spp has heavy firefly noise on walls. With OIDN, walls should look clean.

### 6.4 Failure mode

Unit test: call `oidnDenoise` with `beauty = nullptr` → returns `false` → `getPixels()` still returns a valid noisy buffer. No crash.

### 6.5 CLI parse

Unit test: `parseDenoiseMode("oidn")` → `OIDN`; `parseDenoiseMode("none")` → `None`; `parseDenoiseMode("gibberish")` → `None` + warning logged.

## 7. Risks

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Include-path churn from file move breaks deferred pipeline builds | Medium | Plan explicitly finds + updates every `#include "render/rt/oidn_denoise.hpp"` |
| OIDN CPU fallback is too slow (multi-second) on first denoise | Low | Use default CPU device; GPU device is an optimization for later |
| CLI arg parsing conflicts with existing example flags | Low | Prefix-match `--denoise=` rules out ambiguity with `deferred`, sample-count args |
| AOV buffers not populated when denoise runs (disabled `enableAuxiliaryAOVs`) | Medium | Denoise code checks — if AOVs disabled, fall back to beauty-only OIDN mode |
| PathTracer `getPixels` cache wasn't `mutable` before | Low | Mark cache members `mutable`, standard pattern |
| Offline default ON surprises users who want noisy reference | Low | Documented CLI override; `--denoise=none` is one flag away |

## 8. Out-of-scope (Parking Lot)

Tracked for later sub-plans:

- **Progressive denoise** — denoise per-sample for live viewport preview. Needs live viewer — add once we have offline RT in `interactive`.
- **GPU-resident OIDN** — OIDN 2.x supports Vulkan/SYCL devices. Avoids CPU readback round-trip. Good for larger images.
- **Multi-pass denoise** — Cycles uses albedo + normal prefilter first. OIDN supports this via `prefilterGuides` flag — consider exposing.
- **OIDN device selection** — currently defaults to CPU. Could let the user pick CUDA/HIP/SYCL device.

## 9. Success Criteria

1. `RTRenderSettings.denoiseMode = OIDN` on offline preset → renders auto-denoise.
2. `--denoise=none` CLI flag produces bit-identical output to pre-change rendering.
3. `--denoise=oidn` at 16 spp produces RMSE vs truth ≥ 30% lower than noisy.
4. Visually: Cornell 16 spp walls look clean; DamagedHelmet 16 spp edges look clean.
5. `model_viewer.cpp`'s `constexpr bool kEnableOIDN = false` hack removed.
6. `ohao/render/rt/denoise/` directory established with `denoise_types.hpp` + moved OIDN files.
7. OIDN failure produces warning but no crash.
8. Build clean, all existing tests pass.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the detailed, bite-sized implementation plan.
