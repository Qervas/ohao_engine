# Denoiser Pipeline Sub-plan 2 — OptiX Denoiser — Design

**Date:** 2026-04-18
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline (Sub-plan 2 of 5)
**Predecessor:** Sub-plan 1 (OIDN switch) ✅ shipped 2026-04-18

---

## 1. Goal

Add NVIDIA OptiX as a second denoiser backend (`DenoiseMode::OptiX`). Parallel to OIDN, mirrors its CPU-roundtrip integration pattern exactly. Zero new infrastructure — same `VulkanRenderer::getPixels()` lazy-denoise dispatch, same `--denoise=` CLI surface, just a different backend when the user selects `optix`.

Auto-detected at build time: if CUDA + OptiX SDK are both present, OptiX support compiles in. If either is missing, `DenoiseMode::OptiX` silently falls back to OIDN at runtime with a stderr warning. Non-NVIDIA developers can still build and run.

## 2. Non-Goals

- Vulkan-CUDA interop (Phase 2 optimization; current design uses CPU roundtrip for simplicity and parity with OIDN)
- Temporal denoising (requires motion vectors — lives in Sub-plan 3's realtime foundation)
- OPTIX_DENOISER_MODEL_KIND_UPSCALE2X (4K upscale — niche)
- Per-sample progressive denoise (viewer feature)
- Replacing OIDN as the default — offline profile stays on OIDN; OptiX is opt-in via CLI

## 3. Decisions (from brainstorming)

- **Integration approach:** CPU roundtrip (same as OIDN — HDR readback → float3 → denoise → tonemap → pixels). Vulkan-CUDA interop deferred.
- **Build dependency:** auto-detect + conditional compile via `OHAO_HAS_OPTIX` define. Graceful runtime fallback to OIDN when unavailable.
- **SDK location:** Linux-native install. OPTIX_ROOT env var (default `~/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64`); CMake also searches `/opt/optix-sdk`, `/usr/local/optix`.
- **API parity:** `optixDenoise()` function signature matches `oidnDenoise()` exactly — drop-in dispatch in `VulkanRenderer::getPixels()`.
- **Implementation detail:** PIMPL pattern in `optix_denoise.cpp` keeps CUDA/OptiX headers out of the public API surface.

## 4. Architecture

### 4.1 `DenoiseMode` enum extension

```cpp
// ohao/render/rt/denoise/denoise_types.hpp
enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    OptiX  = 2,   // NEW in Sub-plan 2
    // future: NRD=3, DLSSRR=4
};
```

`parseDenoiseMode` gains an `"optix"` case. `denoiseModeName(OptiX)` returns `"optix"`.

### 4.2 `optix_denoise.{hpp,cpp}` — public API mirrors OIDN

```cpp
// ohao/render/rt/denoise/optix_denoise.hpp
#pragma once
#include <cstdint>

namespace ohao {

// Denoise a path-traced image using NVIDIA OptiX.
// Same signature as oidnDenoise — drop-in dispatch from VulkanRenderer.
// Returns false on any failure; beauty is unchanged in that case and the
// caller should fall back to another denoiser (or raw pixels).
bool optixDenoise(float* beauty, const float* albedo, const float* normal,
                  uint32_t width, uint32_t height, bool hdr = true);

} // namespace ohao
```

Implementation in `optix_denoise.cpp` uses PIMPL (CUDA/OptiX headers hidden) with a singleton-style denoiser state cached across calls.

When `OHAO_HAS_OPTIX` is NOT defined at compile time, `optixDenoise` is implemented as a trivial `return false;` — callers still link cleanly; runtime dispatch falls through to OIDN.

### 4.3 Build detection (CMake)

```cmake
# Detect OptiX SDK location
find_path(OPTIX_INCLUDE_DIR optix.h
    HINTS $ENV{OPTIX_ROOT}
          $ENV{HOME}/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
          /opt/optix-sdk
          /usr/local/optix
    PATH_SUFFIXES include
)
find_package(CUDAToolkit QUIET)

if(OPTIX_INCLUDE_DIR AND CUDAToolkit_FOUND)
    message(STATUS "OptiX SDK found at ${OPTIX_INCLUDE_DIR} — enabling OptiX denoiser")
    target_compile_definitions(ohao_renderer PUBLIC OHAO_HAS_OPTIX)
    target_include_directories(ohao_renderer PRIVATE ${OPTIX_INCLUDE_DIR})
    target_link_libraries(ohao_renderer PUBLIC CUDA::cudart CUDA::cuda_driver)
else()
    message(STATUS "OptiX SDK or CUDA Toolkit not found — OptiX denoiser unavailable (OIDN still works)")
endif()
```

### 4.4 VulkanRenderer dispatch

`VulkanRenderer::getPixels()` gains one `else if` branch:

```cpp
if (m_denoiseMode == DenoiseMode::OptiX) {
    if (!ohao::optixDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                             rw, rh, /*hdr*/ true)) {
        std::cerr << "[Denoise] OptiX failed — falling back to OIDN\n";
        if (!ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                rw, rh, /*hdr*/ true)) {
            std::cerr << "[Denoise] OIDN also failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }
    }
    m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, 0.5f);
    m_denoiseCacheValid = true;
    return m_denoisedPixelBuffer.data();
}
```

Existing OIDN branch remains unchanged.

### 4.5 PIMPL state in optix_denoise.cpp

```cpp
// ohao/render/rt/denoise/optix_denoise.cpp
#ifdef OHAO_HAS_OPTIX
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

namespace ohao::detail {

struct OptixDenoiserState {
    OptixDeviceContext context = nullptr;
    OptixDenoiser      denoiser = nullptr;
    CUstream           stream = 0;
    // GPU scratch/state buffers, sized for current resolution:
    CUdeviceptr scratch = 0; size_t scratchSize = 0;
    CUdeviceptr state   = 0; size_t stateSize   = 0;
    // Persistent I/O buffers (beauty/albedo/normal/output):
    CUdeviceptr inBeauty = 0, inAlbedo = 0, inNormal = 0, outBeauty = 0;
    uint32_t lastWidth = 0, lastHeight = 0;

    ~OptixDenoiserState() { /* cleanup all CUDA resources */ }
};

// Singleton pattern — lazy init on first call, lifetime = process
OptixDenoiserState& getDenoiserState();

}
#endif
```

Resource reuse: denoiser context + denoiser handle created once per process; scratch/state/I-O buffers reallocated only when resolution changes.

## 5. Lifecycle

### 5.1 First call

```
cuInit(0)
cuCtxCreate(&cudaCtx, 0, device)
optixInit()
optixDeviceContextCreate(cudaCtx, options, &ctx)
optixDenoiserCreate(ctx, OPTIX_DENOISER_MODEL_KIND_HDR, &options, &denoiser)
cudaStreamCreate(&stream)
```

### 5.2 Per resolution change (first frame at a new w/h)

```
optixDenoiserComputeMemoryResources(denoiser, w, h, &sizes)
cudaMalloc(&scratch, sizes.withoutOverlapScratchSizeInBytes)
cudaMalloc(&state,   sizes.stateSizeInBytes)
cudaMalloc(&inBeauty, w*h*3*sizeof(float))   // float3
cudaMalloc(&inAlbedo, w*h*3*sizeof(float))
cudaMalloc(&inNormal, w*h*3*sizeof(float))
cudaMalloc(&outBeauty, w*h*3*sizeof(float))
optixDenoiserSetup(denoiser, stream, w, h, state, sizes.stateSizeInBytes,
                   scratch, sizes.withoutOverlapScratchSizeInBytes)
```

### 5.3 Per frame

```
cudaMemcpy H2D (beauty, albedo, normal)
OptixDenoiserLayer layer = { input=inBeauty, output=outBeauty }
OptixDenoiserGuideLayer guide = { albedo=inAlbedo, normal=inNormal }
optixDenoiserInvoke(denoiser, stream, &params, state, stateSize,
                    &guide, &layer, 1, 0, 0, scratch, scratchSize)
cudaStreamSynchronize(stream)
cudaMemcpy D2H (outBeauty → caller's beauty buffer)
```

### 5.4 Shutdown

Singleton destructor handles:
- `cudaFree` all CUDA pointers
- `optixDenoiserDestroy`
- `optixDeviceContextDestroy`
- `cudaStreamDestroy`
- `cuCtxDestroy` (or let process teardown release)

## 6. CLI integration

No new example code — Sub-plan 1 already plumbs `--denoise=<mode>` via `parseDenoiseMode`. Update the parser to accept `"optix"`. Example invocation:

```bash
./build/env_demo DamagedHelmet env_studio out.png 16 --denoise=optix
```

Runtime output:
- Build-time OptiX missing → stderr `[Denoise] OptiX not compiled — falling back to OIDN`
- Runtime OptiX init fails (driver mismatch etc) → stderr `[Denoise] OptiX failed — falling back to OIDN`
- Success → stdout `[OptiX] Denoised 1920x1080`

## 7. Files

**New:**
- `ohao/render/rt/denoise/optix_denoise.hpp` — public API
- `ohao/render/rt/denoise/optix_denoise.cpp` — PIMPL impl; conditionally compiled with `OHAO_HAS_OPTIX`

**Modified:**
- `ohao/render/rt/denoise/denoise_types.hpp` — enum `OptiX = 2`
- `ohao/render/rt/denoise/denoise_types.cpp` — parse/name cases
- `ohao/gpu/vulkan/renderer.cpp` — dispatch OptiX branch in `getPixels()`
- `CMakeLists.txt` (or a new `cmake/FindOptiX.cmake`) — SDK detection + CUDA link
- `tests/renderer/denoise_parse_test.cpp` — add `ParsesOptix` test
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append Sub-plan 2 entry

## 8. Verification

1. **Build with OptiX found** — `cmake -B build` logs "OptiX SDK found ... enabling OptiX denoiser". Build succeeds.
2. **Build without OptiX** — unset `OPTIX_ROOT`, no standard path matches. Build logs "OptiX SDK not found — OptiX denoiser unavailable (OIDN still works)". Build succeeds. Runtime `--denoise=optix` falls back to OIDN with stderr warning.
3. **Runtime OptiX render** — `./build/env_demo ... 16 --denoise=optix` produces denoised output. stdout `[OptiX] Denoised 1920x1080`.
4. **Quality parity** — OptiX RMSE vs truth within ±15% of OIDN on `envlit_turntable`. Either direction acceptable; we're not benchmarking which is "better" — both are industry standard.
5. **CLI parse test** — `parseDenoiseMode("optix")` → `DenoiseMode::OptiX`, `denoiseModeName(OptiX)` → `"optix"`.
6. **Fallback chain** — force OptiX to fail (break init temporarily), verify OIDN fallback kicks in. Image is denoised, stderr shows fallback message.
7. **No regressions** — `--denoise=none` and `--denoise=oidn` behaviors unchanged.

## 9. Risks

| Risk | Mitigation |
|------|-----------|
| OptiX init fails (driver version mismatch) | `optixDenoise` returns false, fallback to OIDN |
| CUDA context leaks on renderer destruction | Singleton destructor runs at program exit; verify with `compute-sanitizer` |
| OptiX 9.1 API diverges from older versions | Lock to OptiX 9.x API in code; document min SDK in CLAUDE.md |
| CUDA runtime not linked | CMake explicitly `target_link_libraries(... CUDA::cudart CUDA::cuda_driver)` |
| Denoiser setup cost per resolution | Cache state keyed on `(w, h)`; re-setup only when mismatched |
| First-frame latency spike (CUDA init ~200ms) | Accepted for offline workflow; log "Initializing OptiX..." if it becomes user-visible |
| Differences between OptiX temporal and static mode | Use `OPTIX_DENOISER_MODEL_KIND_HDR` (static, matches OIDN usage) |
| PIMPL ABI: OptiX symbols leak into ohao_renderer | Guarded by `#ifdef OHAO_HAS_OPTIX`; header-only API is clean |

## 10. Out-of-scope (Parking Lot)

Tracked for later sub-plans:

- **Vulkan-CUDA interop** — share Vulkan images with CUDA directly. Removes CPU roundtrip. Phase 2 optimization when performance matters.
- **OptiX temporal denoiser** — requires motion vectors; lands in Sub-plan 3's realtime foundation.
- **OPTIX_DENOISER_MODEL_KIND_UPSCALE2X** — 4K upscale; niche.
- **Side-by-side quality benchmark** — automated OIDN-vs-OptiX comparison tool. Useful but not blocking.

## 11. Success Criteria

1. CMake auto-detects OptiX via `OPTIX_ROOT` env var or standard paths; logs detection result.
2. `ohao/render/rt/denoise/optix_denoise.{hpp,cpp}` implements `optixDenoise()` with same signature as `oidnDenoise`.
3. `VulkanRenderer::getPixels()` dispatches `DenoiseMode::OptiX`; falls back to OIDN on any failure.
4. `--denoise=optix` CLI works on all 4 examples (cornell_box, env_demo, model_viewer, turntable).
5. `./build/env_demo ... 16 --denoise=optix` produces denoised output visually comparable to OIDN.
6. Builds succeed both with and without OptiX SDK present (conditional compile verified).
7. No regressions on `--denoise=none` and `--denoise=oidn`.
8. Verification log updated with OptiX numbers.

## 12. Next Step

Invoke `superpowers:writing-plans` to generate the detailed, bite-sized implementation plan.
