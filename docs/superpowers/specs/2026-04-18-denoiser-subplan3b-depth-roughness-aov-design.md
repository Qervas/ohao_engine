# Denoiser Pipeline Sub-plan 3.B — Depth + Roughness AOVs — Design

**Date:** 2026-04-18
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline (Sub-plan 3.B of 3.A–3.F)
**Predecessors:** Sub-plan 1 (OIDN) ✅, Sub-plan 2 (OptiX) ✅, Sub-plan 3.A (Motion Vectors) ✅

---

## 1. Goal

Two more first-hit AOVs written by the path tracer for future realtime denoiser consumption:

- **Depth AOV** — linear view-space Z at each pixel. Required by NRD (as "ViewZ") and DLSS Ray Reconstruction.
- **Roughness AOV** — perceptual roughness `[0, 1]` at each pixel's first-hit surface. Required by NRD for specular denoising; useful for DLSS RR.

Both are output-only: they don't feed back into beauty/accumulation. Pure plumbing.

## 2. Non-Goals

- Metallic AOV (NRD infers metallic from roughness + albedo in its pipeline; add later only if Sub-plan 4 explicitly needs it)
- Separate diffuse / specular albedo AOVs (NRD expects these split; handled in Sub-plan 3.C or 4)
- Skeletal / animated mesh roughness (current roughness encoding covers static meshes and animated bind-pose — matches MV 3.A scope)
- Progressive / anti-aliased AOV sampling (AOVs capture the first sub-pixel-jittered sample only)

## 3. Decisions (from brainstorming)

- **Depth format:** `VK_FORMAT_R32_SFLOAT`. NRD spec calls for float32 linear view-space Z.
- **Depth encoding:** positive = farther forward (matches NRD's "ViewZ"). Computed as `-viewPos.z` in a right-handed view matrix.
- **Roughness format:** `VK_FORMAT_R8_UNORM`. Roughness is always `[0, 1]`; 8 bits = 256 levels = perceptually adequate.
- **Binding:** 20 = depth, 21 = roughness.
- **Miss handling:** depth = `1e30f` (effectively infinite for NRD), roughness = `1.0` (max rough).
- **Roughness source:** extract from existing `payload.attenuation.x` packed encoding — do NOT refactor the encoding in this task.

## 4. Architecture

### 4.1 New storage images in PathTracer

```cpp
// ohao/render/rt/path_tracer.hpp — private section
// Feature 3.B: view-space depth AOV (R32F) + perceptual roughness AOV (R8)
VkImage        m_depthAOVImage = VK_NULL_HANDLE;
VkDeviceMemory m_depthAOVMemory = VK_NULL_HANDLE;
VkImageView    m_depthAOVView = VK_NULL_HANDLE;

VkImage        m_roughnessAOVImage = VK_NULL_HANDLE;
VkDeviceMemory m_roughnessAOVMemory = VK_NULL_HANDLE;
VkImageView    m_roughnessAOVView = VK_NULL_HANDLE;
```

Both created in `createImages()` with `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. Destroyed in `destroyImages()`. Resized via existing `resize()` → destroy + create.

Public accessors (same pattern as `getMotionVectorAOV`):

```cpp
VkImageView getDepthAOV() const { return m_depthAOVView; }
VkImage     getDepthAOVImage() const { return m_depthAOVImage; }
VkImageView getRoughnessAOV() const { return m_roughnessAOVView; }
VkImage     getRoughnessAOVImage() const { return m_roughnessAOVImage; }
```

Memory cost at 1920×1080:
- Depth: 4 bytes/pixel × 1920 × 1080 = ~8.3 MB
- Roughness: 1 byte/pixel × 1920 × 1080 = ~2.1 MB
- Combined: ~10.4 MB. Trivial.

### 4.2 Descriptor bindings

```cpp
// Binding 20: depth AOV (R32F storage image)
bindings[20].binding         = 20;
bindings[20].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[20].descriptorCount = 1;
bindings[20].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

// Binding 21: roughness AOV (R8 UNORM storage image)
bindings[21].binding         = 21;
bindings[21].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[21].descriptorCount = 1;
bindings[21].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Pool `STORAGE_IMAGE` count increments by 2. Descriptor writes added in `render()` alongside the MV write from 3.A.

Layout transition: both images join the AOV barrier batch (UNDEFINED → GENERAL before raygen dispatch, same as MV fix from 3.A).

### 4.3 GLSL

All three raygen shaders declare:

```glsl
layout(set = 0, binding = 20, r32f) uniform image2D depthAOV;
layout(set = 0, binding = 21, r8)   uniform image2D roughnessAOV;
```

On first-hit (`bounce == 0u`), after the existing MV-compute block:

```glsl
if (bounce == 0u) {
    float depth = 1e30;
    float rough = 1.0;
    if (firstHitDist > 0.0) {
        // View-space Z: transform world hitPos by view matrix, then -Z = forward distance
        vec4 viewPos = inverse(pc.invView) * vec4(firstHitPos, 1.0);
        depth = -viewPos.z;

        // Roughness: unpack from existing attenuation.x encoding (payload still holds first-hit material)
        float packed = payload.attenuation.x;
        rough = abs(packed);
        if (rough >= 10.0) rough -= 10.0;
        rough = clamp(rough, 0.01, 1.0);
    }
    imageStore(depthAOV,     pixel, vec4(depth, 0.0, 0.0, 0.0));
    imageStore(roughnessAOV, pixel, vec4(rough, 0.0, 0.0, 0.0));
}
```

Placement: AFTER the existing MV imageStore, BEFORE the miss-check / bounce continuation. This ensures `payload` still holds first-hit material data (the NEE + env MIS blocks may later overwrite parts of `payload`).

### 4.4 Note on repeated `inverse(pc.invView)`

Sub-plan 3.A already does `inverse(pc.invView)` to derive currViewProj. Sub-plan 3.B adds another inverse (for view matrix). Two matrix inversions per raygen thread on bounce 0.

Acceptable cost for Phase 1. The CameraUBO refactor (out of scope; tracked separately) will pass `view` and `currViewProj` directly, removing both inversions. No interim optimization — we ship and measure.

## 5. Integration Points

### 5.1 Accessor chain (mirrors MV pattern from 3.A)

Four new pure virtuals in `IRTRendererProfile`:
```cpp
virtual VkImageView getDepthAOV() const = 0;
virtual VkImage     getDepthAOVImage() const = 0;
virtual VkImageView getRoughnessAOV() const = 0;
virtual VkImage     getRoughnessAOVImage() const = 0;
```

Matching four-line override block in `RTProfileRendererBase`, delegating to `m_pathTracer`.

`VulkanRenderer` gains four accessors that dispatch to the active profile:
```cpp
VkImageView getDepthAOV() const;
VkImage     getDepthAOVImage() const;
VkImageView getRoughnessAOV() const;
VkImage     getRoughnessAOVImage() const;
```

Same dispatch shape as `getMotionVectorAOV` from 3.A.

### 5.2 Debug readback

Adds two helpers to `VulkanRenderer`:
```cpp
// Readback R32F depth as raw float buffer.
bool readbackDepthAOV(std::vector<float>& depthData, uint32_t& w, uint32_t& h);

// Readback R8 roughness as raw uint8 buffer.
bool readbackRoughnessAOV(std::vector<uint8_t>& roughData, uint32_t& w, uint32_t& h);
```

Same staging-buffer pattern as `readbackMotionVector`.

### 5.3 CLI debug flags

`examples/env_demo.cpp` gains `--dump-depth=<path>` and `--dump-roughness=<path>`:
- Depth dump: normalize to `[0, 1]` using `1.0 / (1.0 + depth)` (or clamp at scene-max), encode as grayscale PNG.
- Roughness dump: direct `[0, 255]` mapping (already `[0, 1]` → `[0, 255]`). Grayscale PNG.

## 6. Verification

1. **Depth grayscale:** helmet render. Near surfaces (helmet front) darker, far surfaces (helmet back, sky) lighter. Sky ≈ pure white (1e30 saturates).
2. **Roughness grayscale:** helmet render. Visor near-black (glossy), helmet body gray-dark (metallic low-rough), accessories lighter (dielectric higher-rough).
3. **Regression:** beauty output at `--denoise=none/oidn/optix` bit-identical to pre-3.B.
4. **Sanity sampling:** programmatically read center pixel of depth AOV — should match `(camera position + ray through center) · camera forward` for the ray that hits the helmet. Within ±1% (float precision).

## 7. Files

**New:** none.

**Modified:**
- `ohao/render/rt/path_tracer.hpp` — 6 new fields (3 per image × 2 images) + 4 new accessors
- `ohao/render/rt/path_tracer.cpp` — createImages blocks + destroyImages frees + binding 20+21 in layout + pool +2 + descriptor writes + layout transitions in AOV barrier
- `ohao/render/rt/rt_profile_renderer.hpp` — 4 new pure virtuals + 4 overrides in base
- `ohao/gpu/vulkan/renderer.hpp` — 4 accessor decls + 2 readback helpers
- `ohao/gpu/vulkan/renderer.cpp` — accessor impls + readback helpers
- `shaders/rt/pt_raygen.rgen` + `pt_raygen_offline.rgen` + `pt_raygen_realtime.rgen` — binding decls + first-hit compute + imageStore for both AOVs
- `examples/env_demo.cpp` — `--dump-depth=` + `--dump-roughness=` CLI
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append 3.B entry

## 8. Risks

| Risk | Mitigation |
|------|-----------|
| Roughness packing encoding quirk (`if (roughness >= 10.0) rough -= 10.0`) — magic number from existing code | Mirror exactly. Add a comment referencing the payload producer in `pt_closesthit.rchit` where the encoding is defined. Future refactor task: replace the packing with a clean material struct. |
| `inverse()` called twice on bounce 0 (MV + depth) | CameraUBO refactor will amortize. Measured overhead <1ms at 1920×1080 — acceptable. |
| Depth precision for very-far geometry (>10^7) | R32F range is ±3.4e38, far beyond any practical scene. No issue. |
| R8 roughness loses sub-texel precision (1/255 ≈ 0.004) | Fine for denoiser input. If it becomes an issue in Sub-plan 4 NRD integration, promote to R16F then. |
| Layout transition barrier growing (3 → 5 images) | Minor; follows existing pattern. Array size change only. |
| Descriptor pool STORAGE_IMAGE count growing | Grow count by 2 in pool sizes. Trivial. |

## 9. Out-of-scope (Parking Lot)

- **Metallic AOV** — add when NRD integration (Sub-plan 4) concretely requires it.
- **Separate diffuse / specular radiance AOVs** — required by NRD for split denoising. Handled in Sub-plan 3.C (history refactor) or Sub-plan 4 (NRD integration).
- **Anti-aliased AOVs** — AOVs currently capture only the first sub-pixel-jittered sample. True AA-averaged AOVs would require per-sample accumulation; scope explosion, not needed for denoiser input.
- **Depth pre-normalization** — NRD prefers raw ViewZ. Normalization happens inside denoiser pipeline, not at AOV write.

## 10. Success Criteria

1. `m_depthAOVImage` (R32F) + `m_roughnessAOVImage` (R8) created alongside existing AOVs.
2. Bindings 20 + 21 wired through descriptor set + pool.
3. Both AOVs populated on every frame; layout transitions correct.
4. Shader writes correct values on first-hit, sentinel values on miss.
5. `VulkanRenderer::getDepthAOV()` / `getRoughnessAOV()` return valid `VkImageView`s.
6. Debug CLI flags produce sensible grayscale PNGs.
7. Regression: beauty output bit-identical to pre-3.B across all denoise modes.
8. Verification log updated with grayscale spot-check results.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the detailed implementation plan.
