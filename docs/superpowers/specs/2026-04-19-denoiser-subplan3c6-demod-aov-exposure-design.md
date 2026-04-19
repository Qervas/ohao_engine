# Denoiser Pipeline Sub-plan 3.C.6 — Demod AOV Exposure — Design

**Date:** 2026-04-19
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline quality pass (3.C.6 of the 3.C.5 → 3.C.6 → 3.C.7 → 3.D → 4 queue)
**Predecessors:** 3.A ✅, 3.B ✅, 3.C ✅, 3.C.5 ✅

---

## 1. Goal

Stop pre-dividing radiance by albedo/F0 in raygen. Instead:

- **Raygen writes RAW radiance** to bindings 22 + 23 (removes the `diffContrib / max(albedo, 0.01)` division and its 100×-amplification-on-dark-channels bug).
- **Two new AOVs** expose the demod factors NRD needs for its own remodulation step:
  - **Binding 24 (diffuse albedo):** `VK_FORMAT_R8G8B8A8_UNORM`. Per-pixel `firstHitDiffAlbedo = isMetal ? vec3(0) : albedo`.
  - **Binding 25 (specular color):** `VK_FORMAT_R8G8B8A8_UNORM`. Per-pixel `firstHitSpecColor = isMetal ? albedo : vec3(0.04)`.
- **Downstream remodulation:** `beauty ≈ denoisedDiffRadiance × diffAlbedo + denoisedSpecRadiance × specColor`. NRD (Sub-plan 4) does this internally; env_demo dumps for debug just show raw AOVs.

## 2. Non-Goals

- Dual-ray bounce-0 split (Sub-plan 3.C.7).
- Ping-pong prev-depth (Sub-plan 3.D).
- NRD integration itself (Sub-plan 4).
- Any change to `radiance` accumulation — the existing beauty path is untouched.
- Replacing or modifying the existing `albedoAOV` (binding 6). It carries a different signal (full albedo including metals, used by OIDN) and stays as-is.

## 3. Decisions

- **Format: RGBA8_UNORM for both new AOVs.** Albedo and F0 are naturally [0, 1]-bounded perceptual colors. RGBA16F would waste 8 bits per channel on data with no dynamic range. Matches NRD reference integration's expected diffuse-albedo / spec-color input format exactly.
- **Binding numbers: 24 (diffAlbedo), 25 (specColor).** Next available after 3.C's 22+23.
- **Raygen exit: write raw `diffContrib` / `specContrib` directly.** No `max(..., 0.01)` floor, no clamp — the in-shader clamp was removed in 3.C.5 for RGBA32F safety; removing the demod division removes the 100× amplification risk at its root.
- **Miss at bounce 0:** both new AOVs write black (`vec4(0, 0, 0, 1)`). Matches the existing diffuse/specular-radiance miss behavior.
- **Shader sync:** all three raygens (`pt_raygen`, `pt_raygen_offline`, `pt_raygen_realtime`) mirror the same edits. Follows the pattern from 3.A/3.B/3.C/3.C.5.

## 4. Architecture

### 4.1 New storage images in `PathTracer`

```cpp
// ohao/render/rt/path_tracer.hpp — private section
// Feature 3.C.6: diffuse albedo for NRD remodulate (RGBA8 UNORM)
VkImage        m_diffAlbedoImage = VK_NULL_HANDLE;
VkDeviceMemory m_diffAlbedoMemory = VK_NULL_HANDLE;
VkImageView    m_diffAlbedoView = VK_NULL_HANDLE;

// Feature 3.C.6: specular color / F0 for NRD remodulate (RGBA8 UNORM)
VkImage        m_specColorImage = VK_NULL_HANDLE;
VkDeviceMemory m_specColorMemory = VK_NULL_HANDLE;
VkImageView    m_specColorView = VK_NULL_HANDLE;
```

Created in `createImages()` with `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. Destroyed in `destroyImages()`. Resized via existing `resize()` → destroy+create path.

Public accessors (mirror 3.C):

```cpp
VkImageView getDiffAlbedoAOV()      const { return m_diffAlbedoView; }
VkImage     getDiffAlbedoAOVImage() const { return m_diffAlbedoImage; }
VkImageView getSpecColorAOV()       const { return m_specColorView; }
VkImage     getSpecColorAOVImage()  const { return m_specColorImage; }
```

Memory cost at 1920×1080: 4 bytes/pixel × 1920 × 1080 × 2 = ~8.3 MB. Trivial.

### 4.2 Descriptor bindings

```cpp
bindings[24].binding         = 24;
bindings[24].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[24].descriptorCount = 1;
bindings[24].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

bindings[25].binding         = 25;
bindings[25].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[25].descriptorCount = 1;
bindings[25].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

- Bindings array grown `[24] → [26]`; `bindingFlags`, `flagsInfo.bindingCount`, `layoutInfo.bindingCount` all follow.
- Pool `STORAGE_IMAGE` count += 2 (13 → 15).
- Descriptor writes in `render()`: 2 new `VkDescriptorImageInfo` locals + 2 more entries in `writes[]` array after the 3.C writes. Layout: `VK_IMAGE_LAYOUT_GENERAL`.
- AOV barrier group grown `[7] → [9]`; both images transition `UNDEFINED → GENERAL` before raygen dispatch. `vkCmdPipelineBarrier` count updated to 9.

### 4.3 GLSL — raygen

Binding decls (after binding 23 `specularRadiance`):

```glsl
layout(set = 0, binding = 24, rgba8) uniform image2D diffuseAlbedoAOV;
layout(set = 0, binding = 25, rgba8) uniform image2D specColorAOV;
```

**At bounce 0, after existing F0 capture** — the `firstHitDiffAlbedo` / `firstHitSpecColor` locals are already computed. Just add two writes:

```glsl
if (bounce == 0u) {
    firstHitDiffAlbedo = isMetal ? vec3(0.0) : albedo;
    firstHitSpecColor  = F0;
    // Sub-plan 3.C.6: expose demod factors as AOVs for NRD remodulate
    imageStore(diffuseAlbedoAOV, pixel, vec4(firstHitDiffAlbedo, 1.0));
    imageStore(specColorAOV,     pixel, vec4(firstHitSpecColor,  1.0));
}
```

**Miss-at-bounce-0 handling:** the existing miss path for env radiance runs BEFORE the material decode. The raygen must ensure AOVs are written on miss too — add initial zero-write at the top of the bounce-0 first-hit check:

```glsl
// Raygen prelude, after existing miss test for bounce 0:
if (bounce == 0u && firstHitDist < 0.0) {
    imageStore(diffuseAlbedoAOV, pixel, vec4(0.0));
    imageStore(specColorAOV,     pixel, vec4(0.0));
}
```

Or simpler: initialize both AOVs to zero at pixel ENTRY (before the trace loop). That way, if bounce 0 misses, the zeros stay; if it hits, the F0-capture block above overwrites them. Simpler invariant:

```glsl
// Near top of main(), before the bounce loop:
imageStore(diffuseAlbedoAOV, pixel, vec4(0.0));
imageStore(specColorAOV,     pixel, vec4(0.0));
```

Picks the second approach — one unconditional write per pixel, no branching, always-safe miss handling. Negligible cost.

**At raygen exit (end of `main()`):** REPLACE the existing demod block:

```glsl
// OLD (3.C / 3.C.5):
vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

WITH:

```glsl
// NEW (3.C.6): write raw radiance; NRD remodulates downstream using binding 24/25.
imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));
imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
```

The `firstHitDiffAlbedo` + `firstHitSpecColor` locals are no longer used for division at exit but are still needed (and now ALSO used for the binding 24/25 writes at bounce 0). They stay in scope.

### 4.4 env_demo changes

Two new CLI flags:
- `--dump-diff-albedo=<path>`: readback RGBA8, direct RGB PNG write (no tonemap — data is already in [0,255] after the /255 scale).
- `--dump-spec-color=<path>`: same.

Existing `--dump-diffuse` / `--dump-specular` now show RAW radiance. Reinhard tonemap still applies but values will be substantially LOWER than pre-3.C.6 (no 100× amplification on dark channels).

Two new readback helpers:
```cpp
bool readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
bool readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
```

4 bytes per pixel × width × height; output vector sized `width * height * 4`; direct `memcpy` (no decode). Same staging-buffer pattern as existing 3.B/3.C readbacks.

### 4.5 Accessor chain

Four new pure virtuals on `IRTRendererProfile`:
```cpp
virtual VkImageView getDiffAlbedoAOV()      const = 0;
virtual VkImage     getDiffAlbedoAOVImage() const = 0;
virtual VkImageView getSpecColorAOV()       const = 0;
virtual VkImage     getSpecColorAOVImage()  const = 0;
```

Matching 4 overrides in `RTProfileRendererBase` delegating to `m_pathTracer`. Four dispatchers in `VulkanRenderer` following the MV/depth/roughness/radiance split idiom (view getters use `getRTRenderer(m_renderMode)` helper; image getters use explicit `m_rtOfflineRenderer`/`m_rtRealtimeRenderer` checks).

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | 6 fields + 4 getters. |
| `ohao/render/rt/path_tracer.cpp` | Create/destroy in `createImages`/`destroyImages`; bindings 24+25 in layout + pool +2 + descriptor writes + AOV barrier group `[7]→[9]`. |
| `ohao/render/rt/rt_profile_renderer.hpp` | 4 pure virtuals + 4 overrides in base. |
| `ohao/gpu/vulkan/renderer.hpp` | 4 accessor decls + 2 readback helper decls. |
| `ohao/gpu/vulkan/renderer.cpp` | Accessor impls + 2 readback impls. |
| `shaders/rt/pt_raygen.rgen` + offline + realtime | Binding decls 24+25 + initial zero-writes + imageStore at bounce-0 F0 capture + replace demod block with raw writes at exit. |
| `examples/env_demo.cpp` | `--dump-diff-albedo` / `--dump-spec-color` CLI + 2 new dump blocks. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C.6 entry. |

## 6. Verification

On DamagedHelmet + env_studio, 64 spp, `--denoise=none`:

1. **Diffuse albedo dump (`renders/diff_albedo_helmet.png`):** matte plates show the helmet's base color; visor + metal body regions are **black** (metals have zero diffuse albedo). Sky transparent/black.
2. **Spec color dump (`renders/spec_color_helmet.png`):** matte plates show near-black (0.04 = 10 on 255 scale); metal regions show the albedo color (metals use albedo as F0). Sky transparent/black.
3. **Diffuse radiance dump (updated):** lower max channel than pre-3.C.6 (no 100× amplification on dark channels). Reinhard tonemap may show visibly brighter result than before since there's no demodulation-induced oversaturation.
4. **Specular radiance dump (updated):** similarly lower max channel; visor's env reflection still visible.
5. **Beauty regression:** bit-identical to pre-3.C.6 across `--denoise=none/oidn/optix`. Only change is AOV writes, which don't feed back.
6. **Sum-match sanity:** for a matte plate pixel, `diffRaw * diffAlbedo ≈ NEE+env+emissive diffuse contribution + downstream diffuse indirect` (approximately equal to the corresponding beauty pixel's diffuse portion). Exact CPU validation deferred to Sub-plan 4 NRD integration tests.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| RGBA8 quantizes F0 = 0.04 to `0.04 * 255 ≈ 10.2` → stored as `10/255 ≈ 0.0392` (1% error). | Irrelevant — NRD's remodulation is perceptual; sub-1% F0 error is invisible. |
| "Raw radiance" in `diffRadiance`/`specRadiance` bindings now represents different semantics than pre-3.C.6 dumps. | Commit message + verification log explicitly notes the semantic change. Anyone bisecting against pre-3.C.6 specular-max numbers will see dramatically different values — documented as expected. |
| Miss-at-bounce-0 path for the two new AOVs | Handled by top-of-main unconditional zero-writes that are overwritten on hit at bounce 0. |
| Raygen has one extra `imageStore` pair per pixel at bounce 0 | 2 additional 4-byte writes per pixel. Negligible bandwidth impact at 1080p (16.6 MB/frame, dwarfed by ray tracing). |
| AOV barrier group growing `[7]→[9]` | Same pattern as 3.A/3.B/3.C/3.C.5. Verified safe. |

## 8. Out-of-scope (Parking Lot)

- Dual-ray bounce-0 split (Sub-plan 3.C.7).
- Ping-pong prev-depth (Sub-plan 3.D).
- NRD consumer integration (Sub-plan 4).
- Extracting `copyStorageImageToHost` helper (Sub-plan 4 cleanup).
- Consolidating IEEE 754 half-decode into `ohao/core/half_float.hpp` (Sub-plan 4 cleanup).
- Exposing `metallic` as a separate AOV — implicit in `diffAlbedo == 0` for metals; NRD doesn't need it separately.
- Consuming the existing `albedoAOV` (binding 6) in NRD. That signal is "full albedo including metals" used by OIDN; NRD wants the dielectric-only signal which is what binding 24 provides.

## 9. Success Criteria

1. `m_diffAlbedoImage` (RGBA8) + `m_specColorImage` (RGBA8) created alongside existing AOVs.
2. Bindings 24 + 25 wired through descriptor set + pool (STORAGE_IMAGE count 15).
3. Both AOVs populated on every frame; AOV barrier group at 9 entries.
4. Raygen writes raw (not demodulated) `diffContrib` / `specContrib` at bindings 22 + 23.
5. Raygen writes `firstHitDiffAlbedo` / `firstHitSpecColor` at bindings 24 + 25 on bounce 0 hits; zero on miss.
6. `VulkanRenderer::getDiffAlbedoAOV()` / `getSpecColorAOV()` return valid `VkImageView`s.
7. Debug CLI flags produce recognizable helmet PNGs:
   - diff_albedo: helmet base color on matte, black on metal, black sky
   - spec_color: near-black on matte (0.04), metal albedo on visor, black sky
8. `--dump-diffuse` / `--dump-specular` PNGs show LOWER max channel than pre-3.C.6 (no 100× amplification).
9. Beauty bit-identical to pre-3.C.6 across `--denoise=none/oidn/optix`.
10. Verification log updated with observations.

## 10. Next Step

Invoke `superpowers:writing-plans` to generate the detailed implementation plan.
