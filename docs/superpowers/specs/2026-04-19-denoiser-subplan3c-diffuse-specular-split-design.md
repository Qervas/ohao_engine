# Denoiser Pipeline Sub-plan 3.C — Diffuse / Specular Radiance Split — Design

**Date:** 2026-04-19
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline (Sub-plan 3.C of 3.A–3.F)
**Predecessors:** 3.A (Motion Vectors) ✅, 3.B (Depth + Roughness AOVs) ✅

---

## 1. Goal

Two more first-hit AOVs written by the path tracer for future realtime denoiser (NRD, Sub-plan 4) consumption:

- **`diffuseRadiance`** — `VK_FORMAT_R16G16B16A16_SFLOAT` @ descriptor binding 22. Per-frame demodulated diffuse radiance (radiance pre-divided by diffuse albedo, with a 0.01 floor).
- **`specularRadiance`** — same format @ binding 23. Per-frame demodulated specular radiance (divided by `F0 + specColor`, 0.01 floor).

Both are output-only per-frame writes. NRD handles temporal accumulation internally, so these images are *not* ping-ponged and hold no history. The existing combined `accumBuffer` beauty path is untouched; OIDN/OptiX offline denoisers continue to consume combined beauty unchanged.

## 2. Non-Goals

- NRD integration itself (Sub-plan 4).
- 2-ray-at-bounce-0 variance reduction (parked — revisit in 4 if single-ray stamp hurts NRD).
- Multi-bounce analytic diffuse/specular split (out of scope; single bounce-0 stamp is standard).
- History ping-pong / temporal accumulation of the split channels (NRD does this).
- Disocclusion mask (Sub-plan 3.D).

## 3. Decisions (from brainstorming)

- **Format:** RGBA16F for both channels. Matches NRD's expected input; 16F handles typical HDR range. Upgrade to RGBA32F parked as a Sub-plan 4 follow-up if precision bites.
- **Demodulation:** Both channels demodulated in raygen (NRD-canonical). Floor at 0.01 per component to avoid divide-by-near-zero on dark materials.
- **Classification:** Primary-surface split with bounce-0 lobe stamp. NEE contribution AT bounce 0 is analytically split; indirect and NEE at bounce ≥ 1 attributed via the stamp.
- **Bindings:** 22 (diffuse), 23 (specular). Both `STORAGE_IMAGE`, raygen-only.
- **Shader sync:** All three raygen shaders write both channels (matches 3.A + 3.B discipline).
- **Ray count at bounce 0:** One ray per pixel (stamp approach). 2-ray upgrade deferred.

## 4. Architecture

### 4.1 New storage images in `PathTracer`

```cpp
// ohao/render/rt/path_tracer.hpp — private section
// Feature 3.C: demodulated diffuse radiance (RGBA16F)
VkImage        m_diffuseRadianceImage = VK_NULL_HANDLE;
VkDeviceMemory m_diffuseRadianceMemory = VK_NULL_HANDLE;
VkImageView    m_diffuseRadianceView = VK_NULL_HANDLE;

// Feature 3.C: demodulated specular radiance (RGBA16F)
VkImage        m_specularRadianceImage = VK_NULL_HANDLE;
VkDeviceMemory m_specularRadianceMemory = VK_NULL_HANDLE;
VkImageView    m_specularRadianceView = VK_NULL_HANDLE;
```

Both created in `createImages()` with `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. Destroyed in `destroyImages()`. Resized via the existing `resize()` → destroy + create path.

Public accessors (mirror 3.B):

```cpp
VkImageView getDiffuseRadianceAOV()      const { return m_diffuseRadianceView; }
VkImage     getDiffuseRadianceAOVImage() const { return m_diffuseRadianceImage; }
VkImageView getSpecularRadianceAOV()      const { return m_specularRadianceView; }
VkImage     getSpecularRadianceAOVImage() const { return m_specularRadianceImage; }
```

Memory cost at 1920×1080: 8 bytes/pixel × 1920 × 1080 × 2 = ~16.6 MB. Trivial.

### 4.2 Descriptor bindings

```cpp
bindings[22].binding         = 22;
bindings[22].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[22].descriptorCount = 1;
bindings[22].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

bindings[23].binding         = 23;
bindings[23].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[23].descriptorCount = 1;
bindings[23].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

- Bindings array grown `[22] → [24]`; `bindingFlags`, `flagsInfo.bindingCount`, `layoutInfo.bindingCount` all follow.
- Pool `STORAGE_IMAGE` count += 2 (11 → 13).
- Descriptor writes in `render()` grow `writes[]` bound by 2; `VkDescriptorImageInfo` locals scoped to function.
- AOV barrier group grown `[5] → [7]`; both images transition `UNDEFINED → GENERAL` before raygen dispatch (same pattern as MV / depth / roughness).
- `vkCmdPipelineBarrier` count updated to 7.

### 4.3 Payload — lobe stamp

`pt_payload.glsl` (or the inline payload struct used by raygen + closesthit) gains:

```glsl
struct RayPayload {
    // ... existing fields ...
    uint lobeMask;  // Sub-plan 3.C: 0 = diffuse, 1 = specular. Set at bounce 0.
};
```

Any closesthit logic that already writes to payload should NOT touch `lobeMask` — only raygen sets it, and only at bounce 0. At bounce ≥ 1 it is read-only from the raygen's perspective.

### 4.4 GLSL — raygen

All three raygen shaders (`pt_raygen.rgen`, `pt_raygen_offline.rgen`, `pt_raygen_realtime.rgen`) declare:

```glsl
layout(set = 0, binding = 22, rgba16f) uniform image2D diffuseRadiance;
layout(set = 0, binding = 23, rgba16f) uniform image2D specularRadiance;
```

**Accumulator locals** at the start of the raygen per-pixel loop:

```glsl
vec3 diffContrib = vec3(0.0);
vec3 specContrib = vec3(0.0);
vec3 firstHitDiffAlbedo = vec3(0.0);
vec3 firstHitSpecColor  = vec3(0.0);  // F0 + specular tint
```

**At bounce 0 first hit** (after material + normal unpack, before NEE):

```glsl
if (bounce == 0u) {
    firstHitDiffAlbedo = unpackedBaseColor * (1.0 - metallic);
    firstHitSpecColor  = mix(vec3(0.04), unpackedBaseColor, metallic);
}
```

**NEE contribution at bounce 0** (split analytically):

```glsl
if (bounce == 0u) {
    vec3 fDiff = evalDiffuseBRDF(...);
    vec3 fSpec = evalSpecularBRDF(...);
    vec3 neeCommon = cosTheta * Li / pdf_light * misW;
    diffContrib += fDiff * neeCommon;
    specContrib += fSpec * neeCommon;
} else {
    // bounce ≥ 1: attribute to stamped lobe
    vec3 neeContribution = f_total * cosTheta * Li / pdf_light * misW * throughput;
    if (payload.lobeMask == 0u) diffContrib += neeContribution;
    else                        specContrib += neeContribution;
}
```

**BSDF-sampled ray at bounce 0** (stamp the chosen lobe):

```glsl
if (bounce == 0u) {
    payload.lobeMask = sampledLobeIsSpecular ? 1u : 0u;
}
```

**Indirect radiance at any bounce** (already returned via payload or throughput):

```glsl
if (bounce >= 1u) {
    vec3 indirect = throughput * payload.radiance;
    if (payload.lobeMask == 0u) diffContrib += indirect;
    else                        specContrib += indirect;
}
```

**Env miss attribution at bounce ≥ 1:** same as indirect — attribute via `payload.lobeMask`.

**Env miss at bounce 0 (no hit at all):** both channels zero. Write black sentinels.

**At raygen exit:**

```glsl
vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
// Clamp to RGBA16F safe range (~65504)
diffDemod = min(diffDemod, vec3(60000.0));
specDemod = min(specDemod, vec3(60000.0));
imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

Writes happen every frame regardless of `bounce == 0u` guard (the accumulators are always zero-initialized).

### 4.5 `pt_closesthit.rchit` propagation

`lobeMask` is a pure stamp: raygen writes, raygen reads. The closesthit shader should leave the field untouched on every hit. Verify by grepping `payload.lobeMask` after implementation — should appear only in raygen files.

## 5. Integration Points

### 5.1 Accessor chain (mirrors 3.A + 3.B)

Four new pure virtuals on `IRTRendererProfile`:
```cpp
virtual VkImageView getDiffuseRadianceAOV()      const = 0;
virtual VkImage     getDiffuseRadianceAOVImage() const = 0;
virtual VkImageView getSpecularRadianceAOV()     const = 0;
virtual VkImage     getSpecularRadianceAOVImage() const = 0;
```

Matching overrides in `RTProfileRendererBase` delegate to `m_pathTracer`. `VulkanRenderer` adds four dispatchers following the MV/depth/roughness split idiom (view getters use `getRTRenderer(m_renderMode)` helper; image getters use explicit `m_rtOfflineRenderer`/`m_rtRealtimeRenderer` checks).

### 5.2 Debug readback + CLI

Two new readback helpers on `VulkanRenderer`:

```cpp
bool readbackDiffuseRadiance(std::vector<uint16_t>& halfData, uint32_t& w, uint32_t& h);
bool readbackSpecularRadiance(std::vector<uint16_t>& halfData, uint32_t& w, uint32_t& h);
```

Same staging-buffer pattern as `readbackMotionVector`. The `uint16_t` vector holds raw half-floats; caller decodes via the existing half2float helper (already present in `env_demo.cpp` from 3.A).

`env_demo.cpp` gains `--dump-diffuse=<path>` + `--dump-specular=<path>`:
- Decode half-float to linear float, reapply a fixed tonemap (ACES or reinhard) for visual readability, write 8-bit RGB PNG.
- Print max channel value to stdout for sanity (expect non-zero, non-NaN, not all-65504).

## 6. Verification

1. **Helmet + env scene (DamagedHelmet + env_studio, 16 spp, `--denoise=none`):**
   - Diffuse dump: warm ambient tint across matte plate areas; visor near-black (visor is glossy metal — all light is specular).
   - Specular dump: bright visor with HDR env reflection visible; dielectric accessories dim.
2. **Sum-match regression:** compute `diffDemod × firstHitDiffAlbedo + specDemod × firstHitSpecColor` per pixel and compare to combined beauty (pre-3.C). Expect ≤1% RMSE (single-sample path; not bit-identical due to stamp randomness, but very close at 64+ spp).
3. **Denoise-mode regression:** `--denoise=none/oidn/optix` produces beauty PNG within ±0 bytes of pre-3.C (AOVs are pure output, no feedback).
4. **Format sanity:** readback max channel > 0 (not all-zero), < 65504 (not saturated), no NaNs (hex-dump first 100 halfs to confirm).
5. **Per-pixel center check:** pixel at helmet center should have diffDemod ≈ ambient color / albedo (roughly uniform across the face), specDemod at visor pixel should be high (reflection).

## 7. Files

**New:** none.

**Modified:**
- `ohao/render/rt/path_tracer.hpp` — 6 fields + 4 accessors.
- `ohao/render/rt/path_tracer.cpp` — create/destroy + bindings 22+23 in layout + pool +2 + descriptor writes + AOV barrier group [5]→[7].
- `ohao/render/rt/rt_profile_renderer.hpp` — 4 pure virtuals + 4 overrides in base.
- `ohao/gpu/vulkan/renderer.hpp` — 4 accessor decls + 2 readback helper decls.
- `ohao/gpu/vulkan/renderer.cpp` — accessor impls + readback impls.
- `shaders/rt/pt_raygen.rgen` + `pt_raygen_offline.rgen` + `pt_raygen_realtime.rgen` — binding decls + lobe stamp at bounce 0 + NEE split + indirect attribution + demodulated writes at exit.
- `shaders/rt/pt_payload.glsl` (or wherever the payload struct is defined) — `uint lobeMask` field.
- `examples/env_demo.cpp` — `--dump-diffuse=` + `--dump-specular=` flags + grayscale/tonemap encode.
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append 3.C entry.

## 8. Risks

| Risk | Mitigation |
|------|-----------|
| **Single-ray bounce-0 variance** — at low spp one lobe is picked per pixel, leaving the other channel empty. At 16+ spp it self-averages. At 1 spp (realtime) NRD expects noisy input — this is fine. | 2-ray upgrade parked for Sub-plan 4 if NRD reports excess variance. |
| **Demod divide by near-zero albedo** | 0.01 floor on each component before divide. |
| **Demodulated radiance > 65504 (RGBA16F max)** — strong sunlight on matte surface → finite but huge demod value. | `min(..., 60000.0)` clamp at imageStore. Promote to RGBA32F in 4 if clamp hits. |
| **Payload field layout change** — adding `lobeMask` reshuffles `RayPayload`. | Verify SPV reflection + closesthit writes don't overflow old offsets. Single-struct layout is shared across all shaders. |
| **NEE MIS weight divergence between diff/spec** — current NEE code computes one BRDF evaluation; splitting into two doubles the f(...) evaluations at bounce 0. | ~negligible cost for first-hit only; no pdf change since light-pdf is the same. |
| **Metal encoding mismatch** — `firstHitSpecColor = mix(0.04, baseColor, metallic)` assumes the existing shading convention. | Mirror the exact formula from `pt_closesthit.rchit` where specular F0 is computed. Verify grep. |

## 9. Out-of-scope (Parking Lot)

- **2-ray at bounce 0** — trace diff-sampled + spec-sampled rays separately. 2× cost at first bounce, but 1-spp NRD input variance drops dramatically. Revisit in Sub-plan 4 if needed.
- **Third channel: direct-only vs indirect split** — NRD's full pipeline wants direct and indirect separated. Our single split is "diffuse vs specular" regardless of direct/indirect. Expand if NRD's REBLUR variant needs it.
- **Emission channel** — emissive surfaces contribute neither diffuse nor specular. Currently dumped into both with weight 0 via `firstHitDiffAlbedo/SpecColor` being 0 for pure emitters → 0.01 floor kicks in → near-zero contribution / 0.01 = finite but tiny. Emitters appear black-ish in both channels. NRD will see the beauty emission channel separately in Sub-plan 4; fine for now.
- **Skeletal motion / animated BLAS** (Sub-plan 3.F still pending).
- **MV AOV consumption refactor** — realtime raygen still computes reprojection inline vs reading binding 19. Orthogonal; parked.

## 10. Success Criteria

1. `m_diffuseRadianceImage` + `m_specularRadianceImage` (RGBA16F) created alongside existing AOVs.
2. Bindings 22 + 23 wired through descriptor set + pool (count 13).
3. Both AOVs populated every frame; AOV barrier group grown to 7 entries.
4. Raygen writes correct demodulated values at both bindings on every pixel (not just hits).
5. Miss rays at bounce 0 write black (both channels zero).
6. NEE at bounce 0 analytically splits contribution; bounce ≥ 1 uses stamp.
7. `VulkanRenderer::getDiffuseRadianceAOV()` / `getSpecularRadianceAOV()` return valid `VkImageView`s.
8. Debug CLI flags produce recognizable helmet PNGs (diffuse = ambient-shaded; specular = visor-bright with env reflection).
9. Sum-match regression within ±1% at ≥64 spp on helmet scene.
10. Beauty bit-identical to pre-3.C across `--denoise=none/oidn/optix`.
11. Verification log updated with sum-match RMSE + channel max values.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the detailed implementation plan.
