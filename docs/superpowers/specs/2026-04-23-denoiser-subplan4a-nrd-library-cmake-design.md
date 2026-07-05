# Denoiser Pipeline Sub-plan 4.A — NRD Library + CMake — Design

**Date:** 2026-04-23
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 (NRD integration), first of ~5 sub-plans
**Predecessors:** All of 3.A–3.D (every AOV NRD needs is live).

---

## 1. Goal

Wire NVIDIA RayTracingDenoiser (NRD, `github.com/NVIDIAGameWorks/RayTracingDenoiser`) into the OHAO build as a **hard dependency** via CMake FetchContent. Ship a PIMPL wrapper (`NrdDenoiser`) with minimal lifecycle (initialize / shutdown) against the existing Vulkan device.

**Not a stubbed integration.** Unlike the OptiX case in Sub-plan 1.B (which needs a proprietary vendor SDK + CUDA install that many users won't have), NRD is pure open-source Vulkan with no external runtime. FetchContent works on any machine that can run OHAO. Adding stub parity would burn code and review cycles for a marginal use case.

No denoising logic yet — that's 4.B (API expansion) and 4.C (first dispatch). 4.A delivers ONLY the build integration + Vulkan-device lifecycle.

## 2. Non-Goals

- Stub fallback. This is a hard dep; if FetchContent fails, the build fails.
- NRD API wrapping beyond init/shutdown — 4.B.
- Actual denoise dispatch — 4.C.
- Remodulation compositor — 4.D.
- `DenoiseMode::NRD` + CLI + realtime integration — 4.E.
- D3D12 / Windows paths. Linux-first.

## 3. Decisions

- **Library acquisition: FetchContent.** CMake pulls NRD from github at configure time. Matches Jolt / tinygltf / Assimp pattern. Zero user setup.
- **Hard dependency.** No `OHAO_HAS_NRD` conditional, no stub TU. Single code path simplifies maintenance.
- **Opt-out path: `-DOHAO_NRD=OFF`.** User can disable in CMake; then `DenoiseMode::NRD` selection at runtime (when 4.E lands) errors out with a clear "NRD disabled at build time" message rather than silently falling back. Explicit > magic.
- **API shape minimal for 4.A:** `class NrdDenoiser { bool initialize(VkDevice, VkPhysicalDevice, uint32_t w, uint32_t h); void shutdown(); }`. PIMPL via `std::unique_ptr<Impl>`. No `isAvailable()` — presence of the class means NRD is linked.
- **No Vulkan-CUDA interop.** NRD runs pure Vulkan compute against the existing device. Zero runtime dep beyond Vulkan 1.3.
- **Linux-first.** Consistent with OptiX / whole project.

## 4. Architecture

### 4.1 CMake integration

Near existing FetchContent blocks in `CMakeLists.txt`:

```cmake
option(OHAO_NRD "Enable NRD (NVIDIA RayTracingDenoiser)" ON)

if(OHAO_NRD)
    include(FetchContent)
    FetchContent_Declare(
        NRD
        GIT_REPOSITORY https://github.com/NVIDIAGameWorks/RayTracingDenoiser.git
        GIT_TAG        v4.x   # exact stable tag resolved in T1
    )
    FetchContent_MakeAvailable(NRD)
    target_link_libraries(ohao_engine PRIVATE NRD)
    target_compile_definitions(ohao_engine PRIVATE OHAO_NRD_ENABLED)
endif()
```

Sources:

```cmake
if(OHAO_NRD)
    target_sources(ohao_engine PRIVATE
        ohao/render/rt/denoise/nrd_denoise.cpp)
endif()
```

If `OHAO_NRD=OFF` is passed, neither the FetchContent nor the source compiles in. The header file still exists (for consumers that `#include` it) but should be guarded by `#ifdef OHAO_NRD_ENABLED` — or the class is simply not forward-declared when disabled.

Exact CMake syntax will match existing project conventions (check the Jolt FetchContent block for reference).

### 4.2 Public header — `ohao/render/rt/denoise/nrd_denoise.hpp`

```cpp
#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    // Initialize NRD context against the given Vulkan device.
    // Returns false on failure.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

Header is always present; header-only compilation of a consumer works (no NRD symbols needed at the header level). Consumers that actually instantiate `NrdDenoiser` need the library linked (via `OHAO_NRD=ON`).

### 4.3 Implementation — `nrd_denoise.cpp`

Not guarded by `OHAO_NRD_ENABLED` — the CMake already excludes the file entirely when NRD is off. Inside:

```cpp
#include "nrd_denoise.hpp"
#include <NRD.h>  // or equivalent NRD v4 header
#include "core/logging/logger.hpp"

namespace ohao {

struct NrdDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    nrd::Instance*   instance       = nullptr;  // exact NRD v4 type TBD at implementation
};

NrdDenoiser::NrdDenoiser() : m_impl(std::make_unique<Impl>()) {}
NrdDenoiser::~NrdDenoiser() { shutdown(); }

bool NrdDenoiser::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                             uint32_t width, uint32_t height) {
    m_impl->device = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width = width;
    m_impl->height = height;

    // Create NRD instance descriptor for REBLUR_DIFFUSE_SPECULAR at w x h.
    // Call NRD CreateInstance. Log + return false on failure.
    // Exact NRD v4 API resolved by implementer.

    return true;
}

void NrdDenoiser::shutdown() {
    if (m_impl->instance) {
        // NRD DestroyInstance
        m_impl->instance = nullptr;
    }
}

}  // namespace ohao
```

NRD v4 API specifics (struct names, init function names, REBLUR descriptor format) resolved during T2 against the FetchContent'd source tree.

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `CMakeLists.txt` | Add `OHAO_NRD` option, FetchContent block (conditional), link `NRD` target, `OHAO_NRD_ENABLED` define, conditional source listing. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` | NEW — public PIMPL API (init/shutdown). |
| `ohao/render/rt/denoise/nrd_denoise.cpp` | NEW — real impl. Compiled only when `OHAO_NRD=ON`. |
| `CLAUDE.md` | Append NRD dependency note (near OptiX entry). |

No VulkanRenderer changes, no enum changes, no CLI changes. 4.A is pure build + lifecycle.

## 6. Verification

1. **Fresh configure pulls NRD:** `cmake -B build -S .` clones github.com/NVIDIAGameWorks/RayTracingDenoiser into `build/_deps/NRD-src/`. First configure takes several minutes (NRD has its own shader compilation step).
2. **Build links:** `cmake --build build -j8` produces `ohao_engine` with NRD symbols linked. No undefined references.
3. **Opt-out path works:** `cmake -B build-nonrd -S . -DOHAO_NRD=OFF` configures without fetching NRD. `cmake --build build-nonrd -j8` produces `ohao_engine` sans NRD (no `NrdDenoiser` instantiation sites compile in — ensured by 4.E's guards).
4. **Lifecycle smoke:** a temporary one-shot probe in `VulkanRenderer::initialize` that constructs `NrdDenoiser`, calls `initialize(...)`, logs success, calls `shutdown()`. Expected: `"[NRD] initialized for 1920x1080"`. Probe removed in 4.B when real dispatch replaces it.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| NRD FetchContent pulls large dep tree (NVRHI, slang, shaderMake); first configure slow | ~5 min acceptable infra cost. Cached afterwards. |
| NRD v4 API drift | Pin to specific release tag in FetchContent. T1 implementer picks the current stable tag. |
| NRD internal CMake pollutes build with extra targets | FetchContent scope is local; use `set(NRD_*)` flags to limit build surface if needed (T1 finds the right knobs). |
| License | NRD uses NVIDIA Source Code License — permissive for embedding. Same as OptiX precedent. |
| NRD v4 requires specific Vulkan extensions not yet enabled | Check device_setup.cpp; NRD needs features like `VK_KHR_push_descriptor` and similar. Enable in T2 if needed. |
| Hard-dep means CI/users without github access can't build | Accepted trade-off. `-DOHAO_NRD=OFF` is the escape hatch; documented in CLAUDE.md update. |

## 8. Success Criteria

1. `CMakeLists.txt` has `OHAO_NRD` option (default ON) + FetchContent block.
2. `OHAO_NRD_ENABLED` preprocessor define is set when `OHAO_NRD=ON`.
3. `nrd_denoise.hpp` public API with 2 methods (`initialize`, `shutdown`) exists.
4. `nrd_denoise.cpp` compiles and links against real NRD, successfully calls NRD's CreateInstance/DestroyInstance.
5. `-DOHAO_NRD=OFF` CMake invocation produces a working build with NRD excluded.
6. Engine smoke log shows NRD initialize success on an `OHAO_NRD=ON` build.
7. No validation errors, no link errors.
8. Existing denoise modes (OIDN/OptiX/none) untouched.

## 9. Task Shape (preview for writing-plans)

Simplified to 2 tasks (stub removal collapsed T1+T2):

- **T1**: CMake FetchContent + option + conditional source listing + `OHAO_NRD_ENABLED` define. Resolve the exact NRD tag; verify configure-and-build succeeds with NRD ON and OFF.
- **T2**: `nrd_denoise.hpp` public API + `nrd_denoise.cpp` real impl (init/shutdown with NRD v4 API) + probe in VulkanRenderer + CLAUDE.md + verification log.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the 2-task implementation plan.
