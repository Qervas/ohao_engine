# Denoiser Pipeline Sub-plan 4.B — NRD API Expansion — Design

**Date:** 2026-04-23
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline — Sub-plan 4 NRD integration, second of 4.A–4.E
**Predecessors:** 4.A ✅ (NrdDenoiser lifecycle + CMake hard-dep).

---

## 1. Goal

Expand `NrdDenoiser` public API from 2 methods (init/shutdown) to enough surface area that per-frame inputs can be pumped into NRD's `Instance`. Actual compute dispatch (`nrd::GetComputeDispatches` → Vulkan `vkCmdDispatch`) is **4.C**, not this sub-plan.

Two concrete deliverables:

1. **New AOV: packed normal+roughness at binding 26** (RGBA8). Raygen writes it on bounce 0 at first hit. NRD REBLUR's `IN_NORMAL_ROUGHNESS` consumes this format directly.
2. **API expansion on `NrdDenoiser`**: `setCommonSettings(const CameraFrameInputs&)` and `setInputImages(const NrdInputs&)`. Implementations call `nrd::SetCommonSettings` + `nrd::SetDenoiserSettings` per frame and record the Vulkan image views NRD will consume in 4.C.

## 2. Non-Goals

- Compute dispatch (4.C).
- Remodulation compositor (4.D).
- `DenoiseMode::NRD` wiring (4.E).
- `NrdDenoiser` held as persistent member of `PathTracer` — it stays probe-shaped in 4.B, 4.C wires the lifecycle properly.
- Fix to 4.A hardening items (move semantics, re-init leak, license comment) — track separately; not gated by 4.B.

## 3. Decisions

- **Packing format:** RGBA8_UNORM. Normal XY encoded as `N.xy * 0.5 + 0.5`, roughness in `B` (we could put it in A, but A is alpha-associated semantically; B works cleanly). Alpha = 1.0. Matches NRD's default `NRD_FrontEnd_PackNormalAndRoughness` output shape at the bit level.
- **Actually use octahedral encoding:** NRD's canonical packing uses 2-channel oct-encoded normal + 1-channel roughness + 1-channel unused/materialID. For 4.B we use the simpler 3-channel linear normal encoding; octahedral can ship in 4.C tuning if NRD complains.
  *Correction after 3s of reflection:* stick with what NRD expects. NRD v4's `NRD_FrontEnd_PackNormalAndRoughness` writes oct-encoded normal XY into RG, roughness into B, materialID (optional) into A. **We'll match that format.** Details in §4.2.
- **Binding 26:** next available (0-25 used). Descriptor set layout grows to 27. Pool `STORAGE_IMAGE` count +1.
- **Raygen sync:** edit happens at first-hit capture (Stage A) in all 3 raygens alongside the other AOVs (diff-albedo, spec-color).
- **API shape:**
  ```cpp
  struct NrdCameraInputs {
      float viewMatrix[16];      // current frame
      float viewMatrixPrev[16];  // previous frame (for temporal MV validation)
      float projMatrix[16];
      float viewToClipMatrix[16];  // combined
      float motionVectorScale[3];  // e.g. (1, 1, 0) for 2D pixel-space
      float jitter[2];              // current frame sub-pixel
      float jitterPrev[2];          // previous frame sub-pixel
      uint32_t frameIndex;
      bool isMotionVectorInWorldSpace;  // false — ours are in pixel delta
  };
  struct NrdInputImages {
      VkImageView viewZ;                 // binding 20 depth
      VkImageView motionVector;           // binding 19 MV
      VkImageView normalRoughness;        // binding 26 packed (NEW)
      VkImageView diffRadianceHitDist;    // binding 22 (diffuse radiance + hit-dist in alpha)
      VkImageView specRadianceHitDist;    // binding 23 (specular + hit-dist)
      VkImageView diffAlbedo;             // binding 24
      VkImageView specColor;              // binding 25
      // Output writeable views (created in 4.C):
      VkImageView outDiffRadiance;        // denoised diffuse radiance + hit-dist
      VkImageView outSpecRadiance;        // denoised specular radiance + hit-dist
  };
  ```
  `NrdDenoiser::setCommonSettings(const NrdCameraInputs&)` translates these into NRD v4's `nrd::CommonSettings` struct + calls `nrd::SetCommonSettings` on the instance.
  `NrdDenoiser::setInputImages(const NrdInputImages&)` stores the image views on `Impl` for 4.C's dispatch to consume — does **not** yet build NRD's UserPool (that happens in 4.C where we also create the output image views).
- **No-dispatch verification:** 4.B's success is that `SetCommonSettings` + `SetDenoiserSettings` run without error and log acceptance. No visual output changes.

## 4. Architecture

### 4.1 New AOV — packed normal+roughness at binding 26

#### PathTracer changes (mirror of 3.B/3.C.6 AOV pattern):

- New fields on `PathTracer`: `m_normalRoughnessImage/Memory/View`.
- `createImages()`: RGBA8_UNORM, 2D, STORAGE+TRANSFER_SRC, TILING_OPTIMAL, DEVICE_LOCAL.
- `destroyImages()`: teardown.
- `createDescriptorResources()`: `bindings[27]` (was 26 → 27), entry for binding 26 (STORAGE_IMAGE, RAYGEN, count 1). Pool STORAGE_IMAGE count 15 → 16. `aovBarriers[10]` (was [9] → [10]), UNDEFINED→GENERAL before raygen.
- `render()`: descriptor write at binding 26.
- 4 new getters: `getNormalRoughnessAOV()`/`Image()` on PathTracer + IRTRendererProfile + RTProfileRendererBase + VulkanRenderer. Mirrors 3.B accessor chain exactly.

#### Raygen changes (all 3 raygens):

New binding decl after binding 25:
```glsl
layout(set = 0, binding = 26, rgba8) uniform image2D normalRoughnessAOV;
```

At Stage A first-hit capture, AFTER the existing `imageStore(diffuseAlbedoAOV, ...)` and `imageStore(specColorAOV, ...)` — pack normal + roughness per NRD convention:

```glsl
// Sub-plan 4.B: pack normal+roughness for NRD REBLUR IN_NORMAL_ROUGHNESS
// Octahedral encoding (2 channels) + roughness + materialID (unused = 0)
vec2 octN = octEncode(N);  // helper: world-space normal → oct 2D
imageStore(normalRoughnessAOV, pixel, vec4(octN, roughness, 0.0));
```

Add `octEncode` helper (small GLSL function, ~6 lines) in raygen top or in a shared include file. Standard algorithm:
```glsl
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 enc = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
    return enc * 0.5 + 0.5;
}
```

Zero-init at pixel entry (miss case): `imageStore(normalRoughnessAOV, pixel, vec4(0.0));`.

### 4.2 NrdDenoiser API expansion

Expand `ohao/render/rt/denoise/nrd_denoise.hpp`:

```cpp
#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>

namespace ohao {

struct NrdCameraInputs {
    std::array<float, 16> viewMatrix;
    std::array<float, 16> viewMatrixPrev;
    std::array<float, 16> projMatrix;
    std::array<float, 2> jitter;
    std::array<float, 2> jitterPrev;
    std::array<float, 3> motionVectorScale;
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
};

struct NrdInputImages {
    VkImageView viewZ                 = VK_NULL_HANDLE;
    VkImageView motionVector          = VK_NULL_HANDLE;
    VkImageView normalRoughness       = VK_NULL_HANDLE;
    VkImageView diffRadianceHitDist   = VK_NULL_HANDLE;
    VkImageView specRadianceHitDist   = VK_NULL_HANDLE;
    VkImageView diffAlbedo            = VK_NULL_HANDLE;
    VkImageView specColor             = VK_NULL_HANDLE;
    // Outputs — filled by 4.C:
    VkImageView outDiffRadianceHitDist = VK_NULL_HANDLE;
    VkImageView outSpecRadianceHitDist = VK_NULL_HANDLE;
};

class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&)            = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    bool initialize(VkDevice, VkPhysicalDevice, uint32_t width, uint32_t height);
    void shutdown();

    /// Per-frame: update NRD's internal CommonSettings (camera, jitter, frame index).
    /// Must be called before denoise dispatch (4.C).
    bool setCommonSettings(const NrdCameraInputs& inputs);

    /// Per-frame: record the image views NRD will read/write during dispatch (4.C).
    /// Does not yet bind via UserPool — 4.C does that.
    void setInputImages(const NrdInputImages& images);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

Implementation in `nrd_denoise.cpp`:

```cpp
bool NrdDenoiser::setCommonSettings(const NrdCameraInputs& in) {
    if (!m_impl->instance) return false;

    nrd::CommonSettings s = {};
    std::memcpy(s.viewToClipMatrix,     in.projMatrix.data(),    sizeof(float)*16);
    std::memcpy(s.worldToViewMatrix,    in.viewMatrix.data(),    sizeof(float)*16);
    std::memcpy(s.worldToViewMatrixPrev, in.viewMatrixPrev.data(), sizeof(float)*16);
    std::memcpy(s.motionVectorScale,    in.motionVectorScale.data(), sizeof(float)*3);
    std::memcpy(s.cameraJitter,         in.jitter.data(),        sizeof(float)*2);
    std::memcpy(s.cameraJitterPrev,     in.jitterPrev.data(),    sizeof(float)*2);
    s.frameIndex = in.frameIndex;
    s.isMotionVectorInWorldSpace = in.isMotionVectorInWorldSpace;
    // Leave resolutionScale, resolutionScalePrev, hitDistanceParameters, etc. at defaults for 4.B.

    nrd::Result r = nrd::SetCommonSettings(*m_impl->instance, s);
    if (r != nrd::Result::SUCCESS) {
        OHAO_LOG_ERROR("[NRD] SetCommonSettings failed: {}", int(r));
        return false;
    }
    return true;
}

void NrdDenoiser::setInputImages(const NrdInputImages& images) {
    m_impl->inputs = images;  // Impl adds `NrdInputImages inputs` field; 4.C consumes it.
}
```

Exact NRD v4.17 `nrd::CommonSettings` field names resolved at implementation time (same pattern as 4.A resolving CreateInstance).

### 4.3 Probe expansion

Update the probe in `PathTracer::init` (where 4.A added lifecycle smoke) to also exercise `setCommonSettings` once:

```cpp
NrdDenoiser nrdProbe;
if (nrdProbe.initialize(m_device, m_physicalDevice, m_width, m_height)) {
    NrdCameraInputs dummyInputs = {};
    // Fill identity-ish matrices; all other fields zero-initialized.
    for (int i = 0; i < 4; ++i) {
        dummyInputs.viewMatrix[i*4+i]     = 1.0f;
        dummyInputs.viewMatrixPrev[i*4+i] = 1.0f;
        dummyInputs.projMatrix[i*4+i]     = 1.0f;
    }
    dummyInputs.motionVectorScale = {1.0f, 1.0f, 0.0f};
    if (nrdProbe.setCommonSettings(dummyInputs)) {
        OHAO_LOG_INFO("[NRD probe] 4.B CommonSettings accepted");
    }
}
nrdProbe.shutdown();
```

Smoke: log expected: `[NRD probe] 4.B CommonSettings accepted`.

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | +3 fields (normal-rough image/memory/view) + 2 getters. |
| `ohao/render/rt/path_tracer.cpp` | createImages/destroyImages; descriptor binding 26; pool +1; aovBarriers [9]→[10]. Update probe with setCommonSettings call. |
| `ohao/render/rt/rt_profile_renderer.hpp` | +2 pure virtuals + 2 overrides. |
| `ohao/gpu/vulkan/renderer.hpp` | +2 accessor decls. |
| `ohao/gpu/vulkan/renderer.cpp` | +2 accessor impls. |
| `shaders/rt/pt_raygen.rgen` + offline + realtime | Binding 26 decl + octEncode helper + imageStore at first-hit + zero-init at pixel entry. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` | Add `NrdCameraInputs`, `NrdInputImages` structs + 2 public methods. |
| `ohao/render/rt/denoise/nrd_denoise.cpp` | Implement setCommonSettings + setInputImages. Impl gains `nrd::CommonSettings`-related fields + `NrdInputImages inputs` storage. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 4.B entry with settings-accepted log + normal-rough AOV visual. |

### 5.2 Task shape

**T1 — Normal+roughness AOV pipeline (RGBA8 @ binding 26):**
  - PathTracer image + memory + view
  - Descriptor binding + pool + barriers
  - Accessor chain (2 getters × 3 layers)
  - Raygen: binding decl + octEncode + imageStore in all 3 raygens
  - Build clean; smoke test renders beauty unchanged; eyeball the new AOV dump (via a temporary env_demo `--dump-normal-rough=` flag, or just readback in a followup — plan makes the CLI optional)

**T2 — NrdDenoiser API expansion + probe update:**
  - Add `NrdCameraInputs` + `NrdInputImages` structs to header
  - Add `setCommonSettings(...)` + `setInputImages(...)` public methods
  - Implement `nrd::SetCommonSettings` call
  - Update `PathTracer::init` probe to exercise the new API
  - Verification log entry: `[NRD probe] 4.B CommonSettings accepted` log line

## 6. Verification

1. **Beauty unchanged** — 4.B adds an AOV write but doesn't touch radiance math. 256spp helmet render should match pre-4.B exactly.
2. **Normal-rough AOV visible content** — via a temporary dump or readback, pixel values at helmet visor should show oct-encoded normals (mid-gray gradient) with roughness in B (dark for glossy metal, lighter for matte).
3. **NRD accepts CommonSettings** — probe log `[NRD probe] 4.B CommonSettings accepted` fires without validation errors.
4. **Descriptor pool sufficient** — STORAGE_IMAGE count 15 → 16, no overflow.
5. **Build clean** on both `OHAO_NRD=ON` and `OHAO_NRD=OFF` (the AOV itself is NRD-independent; only the probe expansion is NRD-guarded).

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| `nrd::CommonSettings` field names shift between NRD minor versions | Pin matches v4.17.2 from 4.A. T2 resolves exact names against the fetched source. |
| Oct-encoding drift — NRD expects a specific sign convention | Use NRD's own helper definition (ship `NRD_FrontEnd_PackNormalAndRoughness` equivalent from their docs). Visually verify via dump in verification log. |
| Binding 26 descriptor index collision with any pending work on another branch | No other 3.x branches pending — merge base is clean. |
| AOV barrier growth breaks other pipelines | Same additive pattern as 3.B/3.C — no risk. |
| RGBA8 loses precision for near-grazing normals | Accepted trade-off; NRD uses 8-bit internally. Can promote to RGBA16 later if needed. |

## 8. Success Criteria

1. `m_normalRoughnessImage` (RGBA8) created/destroyed alongside existing AOVs.
2. Binding 26 wired through descriptor set + pool + AOV barriers.
3. All 3 raygens write octEncoded normal + roughness to binding 26 on first-hit, zero on miss.
4. Accessor chain complete (PathTracer → IRTRendererProfile → VulkanRenderer).
5. `NrdDenoiser` exposes `setCommonSettings` + `setInputImages`.
6. `setCommonSettings` calls `nrd::SetCommonSettings` and returns true on success.
7. Probe log shows `[NRD probe] 4.B CommonSettings accepted` on ON build.
8. Beauty bit-identical to pre-4.B; OFF build still clean.

## 9. Next Step

Invoke `superpowers:writing-plans` to generate the 2-task implementation plan.
