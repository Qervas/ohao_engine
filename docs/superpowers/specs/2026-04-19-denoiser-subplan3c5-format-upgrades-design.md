# Denoiser Pipeline Sub-plan 3.C.5 — Format Upgrades — Design

**Date:** 2026-04-19
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline quality pass (3.C.5 of the 3.C.5 → 3.C.6 → 3.C.7 → 3.D → 4 queue)
**Predecessors:** 3.A ✅, 3.B ✅, 3.C ✅

---

## 1. Goal

Switch three AOV formats from NRD-minimum-standard to maximum-precision, and remove the defensive clamp that only existed to protect against half-float overflow:

- **Binding 21 (roughness AOV):** `VK_FORMAT_R8_UNORM` → `VK_FORMAT_R16_SFLOAT`. GLSL `r8` → `r16f`.
- **Binding 22 (diffuse radiance):** `VK_FORMAT_R16G16B16A16_SFLOAT` → `VK_FORMAT_R32G32B32A32_SFLOAT`. GLSL `rgba16f` → `rgba32f`.
- **Binding 23 (specular radiance):** same as binding 22.
- **Remove** `min(..., vec3(60000.0))` clamp in raygen exit writes for diffuse + specular (no longer needed at 32F).

Bundle a readback signature cleanup while formats change: the debug readback helpers return native `std::vector<float>` instead of raw half-floats in `std::vector<uint16_t>`. Kills the `half2float` dance for roughness / diffuse / specular dumps in `env_demo.cpp`.

## 2. Non-Goals

- Demodulation AOV exposure (Sub-plan 3.C.6).
- Dual-ray bounce-0 split (Sub-plan 3.C.7).
- Disocclusion depth ping-pong (Sub-plan 3.D).
- Any math change in the shader — formats only.

## 3. Decisions

- **Roughness format:** `VK_FORMAT_R16_SFLOAT`. Half-float, range `[6e-5, 65504]`. Perceptual roughness `[0.01, 1.0]` fits trivially. 16F matches NRD's pack layout for normal+roughness bundles. R16_UNORM would give uniform level spacing but R16F matches NRD reference convention and the project's existing half-float idiom (MV binding 19).
- **Radiance format:** `VK_FORMAT_R32G32B32A32_SFLOAT`. Full single-precision floats. Removes all overflow concerns; the 34048 max channel observed on helmet+env_studio (52% of 16F safe range) no longer risks saturation.
- **Clamp removal:** `min(diffDemod, vec3(60000.0))` and `min(specDemod, vec3(60000.0))` lines deleted. 32F max is `3.4e38`; no defensive clamp needed. If NaN ever appears, that's a shader bug at source — not something a downstream clamp should mask.
- **Readback signatures:** change to `std::vector<float>&` for all three affected helpers. Internal decode happens inside the readback (R16F → float, R32F → float as identity, demod / halfs go away).

## 4. Architecture

### 4.1 Format changes in `path_tracer.cpp`

Inside `createImages()`:
- Roughness block (3.B): `imageInfo.format = VK_FORMAT_R16_SFLOAT` (was R8_UNORM) on image + view create.
- Diffuse radiance block (3.C): `imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT` (was R16G16B16A16_SFLOAT) on image + view create.
- Specular radiance block (3.C): same.

Descriptor binding types unchanged (`VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` for all three). Pool counts unchanged.

### 4.2 Shader changes — all three raygens

Binding decls change format qualifier:

```glsl
layout(set = 0, binding = 21, r16f)  uniform image2D roughnessAOV;      // was r8
layout(set = 0, binding = 22, rgba32f) uniform image2D diffuseRadiance; // was rgba16f
layout(set = 0, binding = 23, rgba32f) uniform image2D specularRadiance; // was rgba16f
```

Raygen exit write block — delete the two clamp lines:

```glsl
    vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
    vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
    // DELETED: diffDemod = min(diffDemod, vec3(60000.0));
    // DELETED: specDemod = min(specDemod, vec3(60000.0));
    imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
    imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

Roughness AOV imageStore unchanged at the GLSL call site — it already writes `vec4(rough, 0.0, 0.0, 0.0)` which is format-agnostic.

### 4.3 Readback helper signature changes

**Header (`renderer.hpp`):**

```cpp
// Was: bool readbackRoughnessAOV(std::vector<uint8_t>& roughData, uint32_t& w, uint32_t& h);
bool readbackRoughnessAOV(std::vector<float>& roughData, uint32_t& width, uint32_t& height);

// Was: bool readbackDiffuseRadiance(std::vector<uint16_t>& halfData, uint32_t& w, uint32_t& h);
bool readbackDiffuseRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);

// Was: bool readbackSpecularRadiance(std::vector<uint16_t>& halfData, uint32_t& w, uint32_t& h);
bool readbackSpecularRadiance(std::vector<float>& data, uint32_t& width, uint32_t& height);
```

**Implementation (`renderer.cpp`):**

- `readbackRoughnessAOV`: staging buffer size `w * h * 2` bytes (R16F). Output vector sized `w * h`. After memcpy, decode each half to float (use the existing `half2float` helper in env_demo as reference, or a local inline). Shared staging pattern still applies.
- `readbackDiffuseRadiance` / `readbackSpecularRadiance`: staging buffer size `w * h * 16` bytes (RGBA32F). Output vector sized `w * h * 4`. After memcpy, it's a direct `std::memcpy` to `float*` — no decode needed since R32F is already IEEE single-precision.

### 4.4 env_demo.cpp dump block changes

- `--dump-roughness=<path>`: reads `std::vector<float>` (was `uint8_t`). Encode to grayscale PNG via `clamp(float * 255.0, 0, 255) → uint8_t`. Same visual output.
- `--dump-diffuse=<path>` / `--dump-specular=<path>`: read `std::vector<float>` (was halfs-as-uint16). No `half2float` call. Reinhard + PNG write as before.
- Remove the `dumpRGBA16FStream` lambda's `half2float` calls; replace with direct `float` reads from the vector.

## 5. Verification

1. **Cornell smoke** at 4 spp, `--denoise=none`: build clean, no validation errors, `saved: cornell.png`.
2. **Regression:** beauty output bit-identical to pre-3.C.5 across `--denoise=none/oidn/optix` (format change has no math impact).
3. **env_demo helmet dumps:**
   - Roughness dump: visually identical to pre-3.C.5 (R8 and R16F both quantize a `[0, 1]` signal to 8-bit PNG display — the underlying AOV has higher precision but the display doesn't show it).
   - Diffuse/specular dumps: may show max channel values *higher than 60000* if the scene has genuinely extreme HDR spikes (previously clamped). That's expected.
4. **Sum-match regression:** beauty pixel ≈ diffDemod × firstHitDiffAlbedo + specDemod × firstHitSpecColor at unclamped pixels — should now be tighter (no 60000 clamp artifact).

## 6. Files

**Modified:**
- `ohao/render/rt/path_tracer.cpp` — 3 format field changes in createImages() (roughness + diffuse + specular blocks).
- `shaders/rt/pt_raygen.rgen` + `pt_raygen_offline.rgen` + `pt_raygen_realtime.rgen` — binding decl format strings + remove 2 clamp lines.
- `ohao/gpu/vulkan/renderer.hpp` — 3 readback signature type changes.
- `ohao/gpu/vulkan/renderer.cpp` — 3 readback impls: byte-count changes + vector type changes + half-float decode for roughness.
- `examples/env_demo.cpp` — 3 dump block decoder simplifications (native float instead of halfs).
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append 3.C.5 entry.

## 7. Risks

| Risk | Mitigation |
|------|-----------|
| `r16f` storage-image requires extension | Already used via MV binding 19 (RG16F). Extension is live. |
| Readback signature change breaks callers | Only env_demo uses these (grep confirms). Debug-only API. |
| Clamp removal exposes NaN/inf bugs downstream | Expected: NRD in Sub-plan 4 will surface them. If they show up in env_demo dumps as fully black pixels or max-white pixels, investigate at the source in raygen, not a new clamp. |
| R16F roughness precision vs R8 | R16F has log-density near zero — better precision at roughness < 0.1 (glossy surfaces), slightly worse at roughness > 0.5 (matte). Net: better for denoiser discrimination of smooth-vs-medium surfaces where the split matters most. |

## 8. Memory Impact

At 1920×1080:
- Roughness: 2.1 MB → 4.1 MB (+2 MB)
- Diffuse radiance: 16.6 MB → 33.2 MB (+16.6 MB)
- Specular radiance: 16.6 MB → 33.2 MB (+16.6 MB)

Total: **+35 MB** GPU memory. Trivial (<1% of a typical 8 GB RT-capable GPU).

## 9. Out-of-scope (Parking Lot)

- Exposing `firstHitDiffAlbedo` / `firstHitSpecColor` as AOVs (Sub-plan 3.C.6).
- Replacing single-ray bounce-0 with dual-ray (Sub-plan 3.C.7).
- Ping-pong prev-depth (Sub-plan 3.D).
- Any PathTracer refactor beyond format field changes.

## 10. Success Criteria

1. Binding 21 roughness AOV is R16F; bindings 22 + 23 radiance AOVs are RGBA32F.
2. Raygen clamp lines removed; shaders compile clean on all three files.
3. Regression: beauty bit-identical across all denoise modes.
4. Readback helpers return `std::vector<float>` (not `uint8_t`/`uint16_t`).
5. env_demo roughness dump visually identical; diffuse/specular dumps potentially show higher max channel (that's the point).
6. No new validation errors.
7. Verification log updated with new max channel observations.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the detailed implementation plan.
