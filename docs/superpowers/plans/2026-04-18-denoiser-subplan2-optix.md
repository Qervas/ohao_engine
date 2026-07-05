# Denoiser Sub-plan 2: OptiX Denoiser — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `DenoiseMode::OptiX` as a second denoiser backend alongside OIDN. Same CPU-roundtrip integration pattern; auto-detected at build time via `OHAO_HAS_OPTIX` compile define; graceful runtime fallback to OIDN when OptiX is unavailable.

**Architecture:** CMake probes for CUDA Toolkit + OptiX SDK (via `OPTIX_ROOT` env var or standard paths). If found, defines `OHAO_HAS_OPTIX` and links CUDA runtime. `optix_denoise.cpp` uses PIMPL to hide OptiX/CUDA types behind a plain `bool optixDenoise(...)` API that mirrors `oidnDenoise` exactly. `VulkanRenderer::getPixels()` dispatches the OptiX branch; on any failure, falls back to OIDN, then raw pixels.

**Tech Stack:** C++20 · Vulkan 1.3 · CUDA 13.2 · NVIDIA OptiX 9.1 · Intel OIDN 2.x (fallback) · GoogleTest.

**Reference spec:** `docs/superpowers/specs/2026-04-18-denoiser-subplan2-optix-design.md`

---

## File Structure

**New files:**

| Path | Responsibility |
|------|---------------|
| `ohao/render/rt/denoise/optix_denoise.hpp` | Public API: `bool optixDenoise(float* beauty, const float* albedo, const float* normal, uint32_t w, uint32_t h, bool hdr)` |
| `ohao/render/rt/denoise/optix_denoise.cpp` | PIMPL impl: CUDA + OptiX wired behind the flat API; no-op fallback when `OHAO_HAS_OPTIX` undefined |
| `cmake/FindOptiX.cmake` | Detection helper — optional; can inline into `ohao/render/CMakeLists.txt` instead |

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/denoise/denoise_types.hpp` | Add `OptiX = 2` enum value |
| `ohao/render/rt/denoise/denoise_types.cpp` | Add `"optix"` case to `parseDenoiseMode` + `denoiseModeName` |
| `ohao/render/CMakeLists.txt` | OptiX SDK + CUDA detection; conditional `OHAO_HAS_OPTIX` define; CUDA link |
| `ohao/gpu/vulkan/renderer.cpp` | Dispatch OptiX branch in `getPixels()` with OIDN fallback |
| `tests/renderer/denoise_parse_test.cpp` | `ParsesOptix` test case |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append Sub-plan 2 entry |
| `CLAUDE.md` | Note on `OPTIX_ROOT` env var for build detection |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-optix -b denoiser-1b-optix HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-optix`.

If `build/_deps/glm-src/` is empty in the fresh worktree (known bootstrap artifact), copy once:

```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/glm-src/. \
      /home/frankyin/Desktop/Github/ohao-optix/build/_deps/glm-src/
```

Ensure the OptiX env var is set in this shell (and children that run the build):

```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Extend `DenoiseMode` enum + parser test (TDD)

**Files:**
- Modify: `ohao/render/rt/denoise/denoise_types.hpp`
- Modify: `ohao/render/rt/denoise/denoise_types.cpp`
- Modify: `tests/renderer/denoise_parse_test.cpp`

### Step 1.1: Add failing test

Edit `tests/renderer/denoise_parse_test.cpp`. Append new test cases after the existing `NameRoundTrip` test (before the `}` closing namespace, if any):

```cpp
TEST(DenoiseTypes, ParsesOptix) {
    EXPECT_EQ(parseDenoiseMode("optix"), DenoiseMode::OptiX);
    EXPECT_EQ(parseDenoiseMode("OptiX"), DenoiseMode::OptiX);
    EXPECT_EQ(parseDenoiseMode("OPTIX"), DenoiseMode::OptiX);
}

TEST(DenoiseTypes, OptixNameRoundTrip) {
    EXPECT_STREQ(denoiseModeName(DenoiseMode::OptiX), "optix");
}
```

### Step 1.2: Confirm compile failure

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
cmake --build build --target denoise_parse_test -j8 2>&1 | head -15
```
Expected: compile error — `DenoiseMode::OptiX` undefined.

### Step 1.3: Extend the enum

Edit `ohao/render/rt/denoise/denoise_types.hpp`. Find `enum class DenoiseMode` and add the `OptiX` value:

```cpp
enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    OptiX  = 2,   // NVIDIA OptiX (requires CUDA + OptiX SDK at build time)
    // future:
    // NRD    = 3,
    // DLSSRR = 4,
};
```

### Step 1.4: Extend parser and name lookup

Edit `ohao/render/rt/denoise/denoise_types.cpp`. Update `parseDenoiseMode` and `denoiseModeName`:

```cpp
DenoiseMode parseDenoiseMode(const std::string& s) {
    const std::string lower = toLower(s);
    if (lower == "none")  return DenoiseMode::None;
    if (lower == "oidn")  return DenoiseMode::OIDN;
    if (lower == "optix") return DenoiseMode::OptiX;
    std::cerr << "[Denoise] Unknown mode '" << s
              << "' — falling back to None\n";
    return DenoiseMode::None;
}

const char* denoiseModeName(DenoiseMode mode) {
    switch (mode) {
        case DenoiseMode::None:  return "none";
        case DenoiseMode::OIDN:  return "oidn";
        case DenoiseMode::OptiX: return "optix";
    }
    return "unknown";
}
```

### Step 1.5: Build + run tests

```bash
cmake --build build --target denoise_parse_test -j8
./build/denoise_parse_test 2>&1 | tail -8
```

Expected: `[  PASSED  ] 6 tests.` (4 existing + 2 new).

### Step 1.6: Commit

```bash
git add ohao/render/rt/denoise/denoise_types.hpp \
        ohao/render/rt/denoise/denoise_types.cpp \
        tests/renderer/denoise_parse_test.cpp
git commit -m "feat(rt): DenoiseMode::OptiX enum + parser case

Adds the OptiX enum value (= 2) and case-insensitive parse. Actual
denoiser implementation lands in the next commit.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 2: OptiX SDK + CUDA detection in CMake

**Files:**
- Modify: `ohao/render/CMakeLists.txt`

### Step 2.1: Add detection block

Edit `ohao/render/CMakeLists.txt`. Immediately after the `find_package(OpenImageDenoise REQUIRED)` line, add:

```cmake
# --- OptiX SDK detection (Denoiser Sub-plan 2) ---
# Header-only SDK. Headers live under $OPTIX_ROOT/include/. CUDA runtime
# is linked separately. If either is missing, OHAO_HAS_OPTIX stays undef
# and optix_denoise.cpp compiles a no-op stub (runtime falls back to OIDN).

find_path(OPTIX_INCLUDE_DIR optix.h
    HINTS
        $ENV{OPTIX_ROOT}
        $ENV{HOME}/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
        /opt/optix-sdk
        /usr/local/optix
    PATH_SUFFIXES include
)
find_package(CUDAToolkit QUIET)

if(OPTIX_INCLUDE_DIR AND CUDAToolkit_FOUND)
    message(STATUS "OptiX SDK found: ${OPTIX_INCLUDE_DIR}")
    message(STATUS "  CUDA Toolkit: ${CUDAToolkit_VERSION}")
    set(OHAO_OPTIX_AVAILABLE TRUE)
else()
    set(OHAO_OPTIX_AVAILABLE FALSE)
    if(NOT OPTIX_INCLUDE_DIR)
        message(STATUS "OptiX SDK: not found (set OPTIX_ROOT env var) — OptiX denoiser unavailable")
    endif()
    if(NOT CUDAToolkit_FOUND)
        message(STATUS "CUDA Toolkit: not found — OptiX denoiser unavailable")
    endif()
endif()
```

### Step 2.2: Wire compile define + link

Still in `ohao/render/CMakeLists.txt`, after the `add_library(ohao_renderer ...)` and `target_link_libraries(ohao_renderer PUBLIC ...)` blocks, append:

```cmake
# Conditional OptiX support
if(OHAO_OPTIX_AVAILABLE)
    target_compile_definitions(ohao_renderer PUBLIC OHAO_HAS_OPTIX)
    target_include_directories(ohao_renderer PRIVATE ${OPTIX_INCLUDE_DIR})
    target_link_libraries(ohao_renderer PUBLIC CUDA::cudart CUDA::cuda_driver)
endif()
```

### Step 2.3: Re-configure + build

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
cmake -B build -S . 2>&1 | grep -E "OptiX|CUDA"
```

Expected: `OptiX SDK found: /home/frankyin/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64/include` + `CUDA Toolkit: 13.2.xx`.

Then:

```bash
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. No new code consumes `OHAO_HAS_OPTIX` yet — this task just wires detection.

### Step 2.4: Verify the macro

Quick sanity check the macro propagates:

```bash
grep -r OHAO_HAS_OPTIX build/ 2>/dev/null | head -3
```

Expected: at least one hit in build artifacts (compile_commands.json or similar). If zero hits, `target_compile_definitions` may have a typo — fix.

### Step 2.5: Commit

```bash
git add ohao/render/CMakeLists.txt
git commit -m "build(rt): detect OptiX SDK + CUDA Toolkit for conditional compile

CMake probes for CUDA Toolkit (find_package) and OptiX headers (via
OPTIX_ROOT env var or standard Linux paths). When both found, defines
OHAO_HAS_OPTIX and links CUDA runtime + driver libs. When either
missing, build continues and the OptiX denoiser path stays a stub.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 3: `optix_denoise.hpp` — public API stub

**Files:**
- Create: `ohao/render/rt/denoise/optix_denoise.hpp`

### Step 3.1: Create the header

Create `ohao/render/rt/denoise/optix_denoise.hpp`:

```cpp
#pragma once

// OptiX denoiser — NVIDIA RTX-only denoiser backend.
//
// API mirrors oidn_denoise.hpp exactly for drop-in dispatch from
// VulkanRenderer::getPixels(). Implementation in optix_denoise.cpp is
// conditionally compiled:
//
//   OHAO_HAS_OPTIX defined  → real OptiX + CUDA impl (see .cpp)
//   OHAO_HAS_OPTIX undef    → no-op stub returns false; caller falls back to OIDN

#include <cstdint>

namespace ohao {

// Denoise a path-traced image using NVIDIA OptiX.
// Inputs: HDR float3 interleaved buffers (beauty, albedo AOV, normal AOV).
// Output: denoised HDR float3 buffer (written in-place to beauty).
// Returns true on success; false on failure (beauty left unchanged).
bool optixDenoise(float* beauty, const float* albedo, const float* normal,
                  uint32_t width, uint32_t height, bool hdr = true);

} // namespace ohao
```

### Step 3.2: Build — should still link because the .cpp doesn't exist yet

The header has no callers yet. Build should still succeed:

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean (the header is not included by anything yet).

### Step 3.3: Commit

```bash
git add ohao/render/rt/denoise/optix_denoise.hpp
git commit -m "feat(rt): optix_denoise.hpp public API (mirrors oidn_denoise)

Public signature: bool optixDenoise(beauty, albedo, normal, w, h, hdr).
Implementation lands in the next commit — stub when OHAO_HAS_OPTIX
undefined, full OptiX + CUDA pipeline when defined.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 4: `optix_denoise.cpp` — PIMPL implementation + stub fallback

**Files:**
- Create: `ohao/render/rt/denoise/optix_denoise.cpp`

### Step 4.1: Create the implementation

Create `ohao/render/rt/denoise/optix_denoise.cpp`:

```cpp
#include "render/rt/denoise/optix_denoise.hpp"

#ifndef OHAO_HAS_OPTIX

// Stub: when CUDA/OptiX SDK are absent, the call always fails. Caller
// (VulkanRenderer::getPixels) will fall back to OIDN.

#include <iostream>

namespace ohao {

bool optixDenoise(float* /*beauty*/, const float* /*albedo*/, const float* /*normal*/,
                  uint32_t /*width*/, uint32_t /*height*/, bool /*hdr*/) {
    static bool warned = false;
    if (!warned) {
        std::cerr << "[OptiX] not compiled in (set OPTIX_ROOT + rebuild) — "
                     "falling back to OIDN\n";
        warned = true;
    }
    return false;
}

} // namespace ohao

#else // OHAO_HAS_OPTIX defined — full impl follows

#include <cuda.h>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

#include <cstring>
#include <iostream>
#include <mutex>

namespace ohao {

namespace {

// --- Helpers ------------------------------------------------------------

#define CUDA_CHECK(call)                                                         \
    do {                                                                          \
        cudaError_t err = (call);                                                 \
        if (err != cudaSuccess) {                                                 \
            std::cerr << "[OptiX] CUDA error " << cudaGetErrorName(err)           \
                      << " at " << __FILE__ << ":" << __LINE__ << '\n';           \
            return false;                                                         \
        }                                                                         \
    } while (0)

#define OPTIX_CHECK(call)                                                         \
    do {                                                                          \
        OptixResult res = (call);                                                 \
        if (res != OPTIX_SUCCESS) {                                               \
            std::cerr << "[OptiX] OptiX error " << static_cast<int>(res)          \
                      << " at " << __FILE__ << ":" << __LINE__ << '\n';           \
            return false;                                                         \
        }                                                                         \
    } while (0)

void optixLogCallback(unsigned int level, const char* tag, const char* msg, void*) {
    // level 0 = disable, 1 = fatal, 2 = error, 3 = warning, 4 = print
    if (level <= 3) {
        std::cerr << "[OptiX][" << tag << "] " << msg << '\n';
    }
}

// --- PIMPL state --------------------------------------------------------

struct OptixDenoiserState {
    bool              initialized = false;
    CUcontext         cudaCtx     = nullptr;
    OptixDeviceContext context    = nullptr;
    OptixDenoiser     denoiser    = nullptr;
    CUstream          stream      = 0;

    // Sized to current (w, h). Reallocated on resolution change.
    uint32_t     lastWidth  = 0;
    uint32_t     lastHeight = 0;
    CUdeviceptr  scratch    = 0;  size_t scratchSize = 0;
    CUdeviceptr  state      = 0;  size_t stateSize   = 0;
    CUdeviceptr  inBeauty   = 0;
    CUdeviceptr  inAlbedo   = 0;
    CUdeviceptr  inNormal   = 0;
    CUdeviceptr  outBeauty  = 0;

    ~OptixDenoiserState() { destroy(); }

    void destroy() {
        if (!initialized) return;
        if (inBeauty)  cudaFree(reinterpret_cast<void*>(inBeauty));
        if (inAlbedo)  cudaFree(reinterpret_cast<void*>(inAlbedo));
        if (inNormal)  cudaFree(reinterpret_cast<void*>(inNormal));
        if (outBeauty) cudaFree(reinterpret_cast<void*>(outBeauty));
        if (scratch)   cudaFree(reinterpret_cast<void*>(scratch));
        if (state)     cudaFree(reinterpret_cast<void*>(state));
        if (denoiser)  optixDenoiserDestroy(denoiser);
        if (context)   optixDeviceContextDestroy(context);
        if (stream)    cudaStreamDestroy(stream);
        initialized = false;
        inBeauty = inAlbedo = inNormal = outBeauty = scratch = state = 0;
        denoiser = nullptr;
        context = nullptr;
        stream = 0;
        lastWidth = lastHeight = 0;
    }
};

OptixDenoiserState& getState() {
    static OptixDenoiserState s;
    return s;
}

std::mutex& getMutex() {
    static std::mutex m;
    return m;
}

// --- One-time init -------------------------------------------------------

bool ensureInitialized(OptixDenoiserState& s) {
    if (s.initialized) return true;

    // CUDA init — use default device
    CUDA_CHECK(cudaFree(0));  // implicit cudaSetDevice(0) + cuCtxCreate equivalent
    CUcontext curCtx = nullptr;
    cuCtxGetCurrent(&curCtx);
    s.cudaCtx = curCtx;

    // OptiX init
    OPTIX_CHECK(optixInit());
    OptixDeviceContextOptions devOpts{};
    devOpts.logCallbackFunction = &optixLogCallback;
    devOpts.logCallbackLevel    = 4;
    OPTIX_CHECK(optixDeviceContextCreate(s.cudaCtx, &devOpts, &s.context));

    // Denoiser — HDR model with albedo + normal guides
    OptixDenoiserOptions dnOpts{};
    dnOpts.guideAlbedo = 1;
    dnOpts.guideNormal = 1;
    OPTIX_CHECK(optixDenoiserCreate(s.context, OPTIX_DENOISER_MODEL_KIND_HDR,
                                     &dnOpts, &s.denoiser));

    CUDA_CHECK(cudaStreamCreate(&s.stream));

    s.initialized = true;
    return true;
}

// --- Resolution-dependent buffers ----------------------------------------

bool ensureResolutionBuffers(OptixDenoiserState& s, uint32_t w, uint32_t h) {
    if (s.lastWidth == w && s.lastHeight == h) return true;

    // Free previous
    if (s.inBeauty)  { cudaFree(reinterpret_cast<void*>(s.inBeauty));  s.inBeauty = 0; }
    if (s.inAlbedo)  { cudaFree(reinterpret_cast<void*>(s.inAlbedo));  s.inAlbedo = 0; }
    if (s.inNormal)  { cudaFree(reinterpret_cast<void*>(s.inNormal));  s.inNormal = 0; }
    if (s.outBeauty) { cudaFree(reinterpret_cast<void*>(s.outBeauty)); s.outBeauty = 0; }
    if (s.scratch)   { cudaFree(reinterpret_cast<void*>(s.scratch));   s.scratch = 0; }
    if (s.state)     { cudaFree(reinterpret_cast<void*>(s.state));     s.state = 0; }

    // Compute memory requirements
    OptixDenoiserSizes sizes{};
    OPTIX_CHECK(optixDenoiserComputeMemoryResources(s.denoiser, w, h, &sizes));
    s.stateSize   = sizes.stateSizeInBytes;
    s.scratchSize = sizes.withoutOverlapScratchSizeInBytes;

    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.state),   s.stateSize));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.scratch), s.scratchSize));

    const size_t pixBytes = static_cast<size_t>(w) * h * 3 * sizeof(float);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inBeauty),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inAlbedo),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.inNormal),  pixBytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&s.outBeauty), pixBytes));

    // Setup denoiser for this resolution
    OPTIX_CHECK(optixDenoiserSetup(s.denoiser, s.stream, w, h,
                                    s.state,   s.stateSize,
                                    s.scratch, s.scratchSize));

    s.lastWidth  = w;
    s.lastHeight = h;
    return true;
}

// --- Build an OptixImage2D descriptor ---
OptixImage2D makeImage(CUdeviceptr data, uint32_t w, uint32_t h) {
    OptixImage2D img{};
    img.data               = data;
    img.width              = w;
    img.height             = h;
    img.rowStrideInBytes   = w * 3 * sizeof(float);
    img.pixelStrideInBytes = 3 * sizeof(float);
    img.format             = OPTIX_PIXEL_FORMAT_FLOAT3;
    return img;
}

} // namespace

// --- Public entry --------------------------------------------------------

bool optixDenoise(float* beauty, const float* albedo, const float* normal,
                  uint32_t width, uint32_t height, bool /*hdr*/) {
    std::lock_guard<std::mutex> lock(getMutex());
    OptixDenoiserState& s = getState();

    if (!ensureInitialized(s))                  return false;
    if (!ensureResolutionBuffers(s, width, height)) return false;

    const size_t pixBytes = static_cast<size_t>(width) * height * 3 * sizeof(float);

    // H2D
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inBeauty), beauty,  pixBytes,
                                cudaMemcpyHostToDevice, s.stream));
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inAlbedo), albedo,  pixBytes,
                                cudaMemcpyHostToDevice, s.stream));
    CUDA_CHECK(cudaMemcpyAsync(reinterpret_cast<void*>(s.inNormal), normal,  pixBytes,
                                cudaMemcpyHostToDevice, s.stream));

    // Build layers
    OptixDenoiserGuideLayer guide{};
    guide.albedo = makeImage(s.inAlbedo, width, height);
    guide.normal = makeImage(s.inNormal, width, height);

    OptixDenoiserLayer layer{};
    layer.input  = makeImage(s.inBeauty,  width, height);
    layer.output = makeImage(s.outBeauty, width, height);

    OptixDenoiserParams params{};
    params.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
    params.hdrIntensity = 0;   // we already exposed internally; hdrMinRGB 0 = no adjustment
    params.blendFactor  = 0.0f;

    OPTIX_CHECK(optixDenoiserInvoke(s.denoiser, s.stream,
                                     &params,
                                     s.state,   s.stateSize,
                                     &guide,
                                     &layer, 1,
                                     0, 0,
                                     s.scratch, s.scratchSize));

    CUDA_CHECK(cudaStreamSynchronize(s.stream));

    // D2H
    CUDA_CHECK(cudaMemcpy(beauty, reinterpret_cast<void*>(s.outBeauty), pixBytes,
                           cudaMemcpyDeviceToHost));

    std::cout << "[OptiX] Denoised " << width << "x" << height << '\n';
    return true;
}

} // namespace ohao

#endif // OHAO_HAS_OPTIX
```

### Step 4.2: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
cmake --build build -j8 2>&1 | tail -10
```

Expected: clean build. The OptiX branch compiles against the SDK headers; CUDA runtime + driver are linked via `target_link_libraries`.

If you see "undefined reference to cudaFree / cudaMalloc / cuInit" — verify `target_link_libraries(ohao_renderer PUBLIC ... CUDA::cudart CUDA::cuda_driver)` is in place (added in Task 2).

### Step 4.3: Smoke — still no callers, should just link

At this point `getPixels()` doesn't dispatch to `optixDenoise`, so runtime behavior is unchanged. Verify:

```bash
./build/cornell_box /tmp/t4_cornell.png 4 2>&1 | tail -3
```

Expected: renders cleanly. No OptiX messages (since nothing calls it).

### Step 4.4: Commit

```bash
git add ohao/render/rt/denoise/optix_denoise.cpp
git commit -m "feat(rt): optix_denoise.cpp — OptiX + CUDA impl (PIMPL)

Header-only API mirrors oidnDenoise signature. Implementation:
  - Stub (returns false) when OHAO_HAS_OPTIX undefined.
  - Full OptiX + CUDA pipeline when defined. Singleton state cached
    across calls; resolution-dependent buffers resized lazily.
  - HDR denoiser with albedo + normal guide layers. H2D/D2H via
    cudaMemcpy; invoked on a single CUDA stream.
  - Mutex-guarded entry so multi-threaded callers serialise cleanly.
  - Cleanup in destructor runs at process exit.

VulkanRenderer dispatch lands in the next commit.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 5: `VulkanRenderer::getPixels()` OptiX dispatch + fallback

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.cpp`

### Step 5.1: Include the header

Edit `ohao/gpu/vulkan/renderer.cpp`. Near the existing `#include "render/rt/denoise/oidn_denoise.hpp"` include, add:

```cpp
#include "render/rt/denoise/optix_denoise.hpp"
```

### Step 5.2: Add OptiX branch

Find `VulkanRenderer::getPixels() const` (added in Sub-plan 1 Task 4). The current body handles `DenoiseMode::None` and `DenoiseMode::OIDN`. Extend to add the `OptiX` branch. Target shape (adapt to the exact existing code structure):

```cpp
const uint8_t* VulkanRenderer::getPixels() const {
    if (m_denoiseMode == DenoiseMode::None) {
        return m_pixelBuffer.data();
    }
    if (m_denoiseCacheValid) {
        return m_denoisedPixelBuffer.data();
    }

    // Shared readback + float3 conversion for any denoiser backend
    std::vector<float> beautyRGBA, albedoRGBA, normalRGBA;
    uint32_t rw = 0, rh = 0;
    auto* self = const_cast<VulkanRenderer*>(this);
    if (!self->readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {
        std::cerr << "[Denoise] readbackHDRBuffers failed — returning noisy pixels\n";
        return m_pixelBuffer.data();
    }
    auto beauty3 = ohao::rgba32fToFloat3(beautyRGBA.data(), rw, rh);
    auto albedo3 = ohao::rgba32fToFloat3(albedoRGBA.data(), rw, rh);
    auto normal3 = ohao::rgba32fToFloat3(normalRGBA.data(), rw, rh);

    bool denoised = false;
    if (m_denoiseMode == DenoiseMode::OptiX) {
        denoised = ohao::optixDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                       rw, rh, /*hdr*/ true);
        if (!denoised) {
            std::cerr << "[Denoise] OptiX unavailable or failed — falling back to OIDN\n";
        }
    }
    if (!denoised && (m_denoiseMode == DenoiseMode::OIDN || m_denoiseMode == DenoiseMode::OptiX)) {
        denoised = ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                      rw, rh, /*hdr*/ true);
        if (!denoised) {
            std::cerr << "[Denoise] OIDN failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }
    }

    m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);
    m_denoiseCacheValid = true;
    return m_denoisedPixelBuffer.data();
}
```

The key structural change: unified readback+convert path, then branch on mode for denoiser selection, then shared tonemap. If OptiX fails, automatically retries with OIDN. If OIDN also fails (or the mode was OIDN to begin with and OIDN fails), returns raw pixels. Eliminates duplication between the existing OIDN-only branch and the new OptiX branch.

### Step 5.3: Build + smoke test

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

```bash
./build/cornell_box /tmp/t5_default.png 4 2>&1 | tail -3
./build/cornell_box /tmp/t5_oidn.png    4 --denoise=oidn  2>&1 | tail -3
./build/cornell_box /tmp/t5_optix.png   4 --denoise=optix 2>&1 | tail -3
./build/cornell_box /tmp/t5_none.png    4 --denoise=none  2>&1 | tail -3
```

Expected:
- `default` and `--denoise=oidn` — OIDN runs (stderr `[OIDN] Denoised 1920x1080`).
- `--denoise=optix` — stderr `[OptiX] Denoised 1920x1080`, no fallback message.
- `--denoise=none` — no denoise messages.

All four PNGs produced, no Vulkan validation errors.

### Step 5.4: Commit

```bash
git add ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): getPixels() dispatches OptiX with OIDN fallback

Unifies the readback+convert+tonemap pipeline; branches on
DenoiseMode::OptiX to call optixDenoise, silently retries OIDN on
failure, raw pixels on double-failure. Existing OIDN-only path reuses
the same flow.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 6: Verification + reference log

**Files:**
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`
- Modify: `CLAUDE.md` (document OPTIX_ROOT)

### Step 6.1: Quality comparison

```bash
cd /home/frankyin/Desktop/Github/ohao-optix
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/optix_out.png 16 --denoise=optix 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/oidn_out.png  16 --denoise=oidn  2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/noisy_out.png 16 --denoise=none  2>&1 | tail -3
```

### Step 6.2: Measure RMSE vs truth

```bash
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png \
    /tmp/noisy_out.png
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png \
    /tmp/oidn_out.png
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png \
    /tmp/optix_out.png
```

Record `Global RMSE` for each of the three. Expected: OptiX and OIDN RMSE within ±15% of each other (both industry denoisers, both should massively beat noisy).

### Step 6.3: Document `OPTIX_ROOT` in CLAUDE.md

Edit `CLAUDE.md`. In the "## Dependencies" section, add an entry:

```markdown
- NVIDIA OptiX SDK 9.1 (optional) — for OptiX denoiser backend. Set `OPTIX_ROOT` env var to the SDK root, or install under `$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64/` (CMake auto-detects). When absent, OptiX denoiser compiles as a no-op stub and `--denoise=optix` falls back to OIDN at runtime.
```

Also, in the "## Run" section, note `--denoise=optix` as a new option.

### Step 6.4: Update verification log

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. APPEND:

```markdown
## 2026-04-18: OptiX denoiser (Denoiser Sub-plan 2) validation

OptiX backend available when CUDA + OptiX SDK present at build time.
Comparison at 16 spp on DamagedHelmet + env_studio:

| Mode | RMSE vs 4096-spp truth |
|------|------------------------|
| --denoise=none  (noisy) | <FILL>  |
| --denoise=oidn          | <FILL>  |
| --denoise=optix         | <FILL>  |

- OptiX vs OIDN RMSE delta: <X>%
- Fallback tested: building without OPTIX_ROOT → --denoise=optix logs
  "[OptiX] not compiled in — falling back to OIDN", image still denoised

Denoiser Sub-plan 2 complete. Next: Sub-plan 3 (realtime foundation —
motion vectors + history + depth/roughness AOVs).
```

Replace `<FILL>` with the three RMSE values and `<X>` with the percentage delta (signed — OptiX could be better or worse, both acceptable).

### Step 6.5: Commit

```bash
git add tests/reference_scenes/custom/envlit_turntable/verification_log.md CLAUDE.md
git commit -m "test(rt): OptiX denoiser quality gate (Denoiser Sub-plan 2)

At 16 spp on envlit_turntable, OptiX RMSE vs truth is <X>% of OIDN.
Both denoisers massively reduce noise vs uniform path-traced output.
CLAUDE.md documents OPTIX_ROOT env var for opt-in builds.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Replace `<X>` with the actual delta. Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 `DenoiseMode::OptiX` enum + parser | Task 1 |
| §4.2 optix_denoise.hpp public API | Task 3 |
| §4.3 CMake auto-detect + conditional define | Task 2 |
| §4.4 VulkanRenderer dispatch with OIDN fallback | Task 5 |
| §4.5 PIMPL implementation | Task 4 |
| §5 Lifecycle (init, per-res, per-frame, shutdown) | Task 4 (ensureInitialized / ensureResolutionBuffers / optixDenoise / dtor) |
| §6 CLI integration | Task 1 (parser) + existing Sub-plan 1 plumbing |
| §8 Verification (build w/ and w/o OptiX, runtime dispatch, quality parity) | Tasks 2, 5, 6 |

**Placeholder scan:** Only `<FILL>` and `<X>` in Task 6 verification log — deliberately for measured numbers.

**Type consistency:**
- `optixDenoise(float*, const float*, const float*, uint32_t, uint32_t, bool)` signature matches OIDN exactly across hpp/cpp/call site.
- `OHAO_HAS_OPTIX` define used consistently in cmake (target_compile_definitions), cpp (#ifdef), and documented in CLAUDE.md.
- `OPTIX_ROOT` env var used consistently in cmake find_path, task commands, CLAUDE.md.

**External dep:** Task 2 requires OPTIX_ROOT to be set when building. If the subagent runs the build without it, CMake will log "OptiX SDK: not found" and the conditional compile skips OptiX — acceptable but the tests in Task 5/6 that exercise `--denoise=optix` will hit the fallback path. Subagent should verify `OPTIX_ROOT` is set before Task 2.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-18-denoiser-subplan2-optix.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
