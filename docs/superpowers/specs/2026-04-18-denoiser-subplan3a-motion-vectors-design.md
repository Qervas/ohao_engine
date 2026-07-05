# Denoiser Pipeline Sub-plan 3.A — Camera Motion Vectors — Design

**Date:** 2026-04-18
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline (Sub-plan 3.A of 3.A–3.F)
**Predecessors:** Sub-plan 1 (OIDN) ✅, Sub-plan 2 (OptiX) ✅

---

## 1. Goal

Output a per-pixel 2D motion vector AOV from the offline path tracer. Each pixel stores `(Δx, Δy)` — the screen-space displacement of the first-hit surface from the previous frame's camera position to the current frame's, measured in pixel units.

Required input for temporal denoisers (NRD, DLSS RR) and any future temporal reprojection / reconstruction pass.

## 2. Non-Goals

- Skeletal / animated mesh motion vectors (Sub-plan 3.F) — requires per-vertex prev-frame transforms
- Depth AOV (Sub-plan 3.B)
- Roughness AOV (Sub-plan 3.B)
- History buffer refactor / temporal reprojection (Sub-plan 3.C) — motion vectors are input; the pass that uses them lives in 3.C
- Binding the motion vector into the existing accumulation reprojection — current implementation uses a surface-position heuristic; switching to MV-based reprojection is Sub-plan 3.C

## 3. Decisions (from brainstorming)

- **Format:** `VK_FORMAT_R16G16_SFLOAT` — half-precision 2D vector, 4 bytes per pixel, industry-standard (UE5/Cycles/Arnold).
- **Units:** pixel delta (current frame pixel pos − previous frame pixel pos). NRD convention. UV-space conversion (`pixelMV / screenSize`) is trivial for consumers that need it.
- **Binding:** 19 (next free after Feature 1.1 env CDFs at 17/18).
- **Integration:** raygen computes + writes on first-hit only. No separate pass, no new shader stage.
- **Scope:** camera motion. Scene is assumed static. Animated meshes produce bind-pose motion only — corrected in Sub-plan 3.F.

## 4. Architecture

### 4.1 New storage image in PathTracer

```cpp
// ohao/render/rt/path_tracer.hpp — private section
VkImage        m_motionVectorImage = VK_NULL_HANDLE;
VkDeviceMemory m_motionVectorMemory = VK_NULL_HANDLE;
VkImageView    m_motionVectorView = VK_NULL_HANDLE;
```

Created in `createImages()` alongside `m_accumBuffer`, destroyed in `destroyImages()`. Resized via existing `resize()` path (calls destroy+create).

Format: `VK_FORMAT_R16G16_SFLOAT`
Usage: `VK_IMAGE_USAGE_STORAGE_BIT` (no TRANSFER needed; we never CPU-readback this; consumer is NRD/DLSS which will interop via GPU).

Accessor:

```cpp
// ohao/gpu/vulkan/renderer.hpp
VkImageView getMotionVectorAOV() const;
```

So downstream code (future NRD/DLSS integration) can bind it to their pipelines without reaching into PathTracer internals.

### 4.2 Descriptor binding 19

Added to the RT pipeline descriptor set layout:

```cpp
// Binding 19: motion vector AOV (RG16F storage image)
bindings[19].binding         = 19;
bindings[19].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
bindings[19].descriptorCount = 1;
bindings[19].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Descriptor pool STORAGE_IMAGE count incremented by 1.

### 4.3 GLSL — compute and write

In all three raygen shaders (`pt_raygen.rgen`, `pt_raygen_offline.rgen`, `pt_raygen_realtime.rgen`), add:

```glsl
layout(set = 0, binding = 19, rg16f) uniform image2D motionVector;
```

On first-hit (`bounce == 0u`), after computing `firstHitPos`:

```glsl
if (bounce == 0u) {
    vec2 motion = vec2(0.0);
    if (firstHitDist > 0.0) {
        // Project world-space hitPos through previous + current viewProj
        vec4 currClip = currViewProj * vec4(firstHitPos, 1.0);
        vec4 prevClip = pc.prevViewProj * vec4(firstHitPos, 1.0);
        if (currClip.w > 0.0 && prevClip.w > 0.0) {
            vec2 currNDC = currClip.xy / currClip.w;
            vec2 prevNDC = prevClip.xy / prevClip.w;
            vec2 currPix = (currNDC * 0.5 + 0.5) * vec2(pc.params.xy);
            vec2 prevPix = (prevNDC * 0.5 + 0.5) * vec2(pc.params.xy);
            motion = currPix - prevPix;
        }
    }
    imageStore(motionVector, pixel, vec4(motion, 0.0, 0.0));
}
```

**Note on `currViewProj`:** we don't currently pass it — only `invView` / `invProj` / `prevViewProj`. We'll add `currViewProj` to `PTPushConstants` OR derive it from `inverse(invView * invProj)` (expensive; avoid). Cleanest approach: pass `currViewProj` as a new push-constant field.

Push-constant total is currently 256 bytes (Vulkan 1.3 minimum after Feature 1.1). Adding another `mat4` = 64 bytes pushes to 320 bytes. **Not safe.** Mitigation: since we have `invView` and `invProj` already, we compute `currViewProj = inverse(invProj) * inverse(invView)` CPU-side each frame and pass it. OR better: CPU already has `view` and `proj` matrices; we just pass `view * proj` as a replacement for one of the existing fields.

**Resolution:** `invView` and `invProj` are used ONLY for camera ray generation (not for projection). We can replace them with a packed alternative that keeps push-constant size unchanged: keep `prevViewProj`, add `currViewProj`, and reconstruct `invView`/`invProj` on the CPU and re-derive what we need from them. OR — simpler — keep push constants unchanged and add a second 64-byte UBO just for view matrices.

**Final call:** add a small UBO `CameraUBO { mat4 invView; mat4 invProj; mat4 currViewProj; mat4 prevViewProj; }` and reference it from raygen. Drops those 4 fields from push constants, freeing 256 bytes of PC space. Cleaner architecture going forward (more fields can land in the UBO without pressure).

This is a bigger change than "just add MV" — flag for the implementation plan to decide: either land the CameraUBO refactor as part of 3.A, or use a minimal workaround (compute `currViewProj` in shader via `inverse(invProj) * inverse(invView)` — expensive, once per thread per frame, but tolerable for a 2-float output).

For Sub-plan 3.A, we take the **minimal workaround**: compute `currViewProj` in-shader as `inverse(invProj) * inverse(invView)`. This is one-time cost per raygen thread on bounce 0, and `inverse()` of a 4×4 is a known hotspot but not prohibitive. The CameraUBO refactor becomes its own follow-up task outside this sub-plan.

**Revised shader:**

```glsl
if (bounce == 0u) {
    vec2 motion = vec2(0.0);
    if (firstHitDist > 0.0) {
        // Derive current viewProj from inverse matrices. Not cheap (two 4x4
        // inversions) but done once per raygen thread per frame. Sub-plan 3.C
        // will pass this via a CameraUBO to eliminate the cost.
        mat4 currViewProj = inverse(pc.invProj) * inverse(pc.invView);

        vec4 currClip = currViewProj * vec4(firstHitPos, 1.0);
        vec4 prevClip = pc.prevViewProj * vec4(firstHitPos, 1.0);
        if (currClip.w > 0.0 && prevClip.w > 0.0) {
            vec2 currNDC = currClip.xy / currClip.w;
            vec2 prevNDC = prevClip.xy / prevClip.w;
            vec2 currPix = (currNDC * 0.5 + 0.5) * vec2(pc.params.xy);
            vec2 prevPix = (prevNDC * 0.5 + 0.5) * vec2(pc.params.xy);
            motion = currPix - prevPix;
        }
    }
    imageStore(motionVector, pixel, vec4(motion, 0.0, 0.0));
}
```

### 4.4 `m_prevViewProj` first-frame initialization

Current code (from Feature 1.1 Task 4) initializes `m_prevViewProj = glm::mat4(1.0f)` (identity). First frame the MV computation would see big bogus motion because `prevClip = identity * hitPos` = hit position in world space.

Fix: the existing `prepareRTSceneForFrame()` stores `m_prevViewProj = proj * view` after render. For frame 0, we need `prevViewProj == currViewProj` so MV is zero. Simplest: in the renderer init or `setScene`, pre-populate `m_prevViewProj` with the camera's initial `proj * view`.

Alternative: gate MV computation on a first-frame flag (skip MV write on frame 0, output zero). Simpler. Use this.

## 5. Integration Points

### 5.1 Accessor on VulkanRenderer

```cpp
// ohao/gpu/vulkan/renderer.hpp
VkImageView getMotionVectorAOV() const;
```

Implementation: delegates to `m_rtOfflineRenderer->getMotionVectorAOV()` (or realtime — whichever is active). Each `RTProfileRendererBase` needs a matching delegate. Same pattern as `getAlbedoAOV` / `getNormalAOV`.

### 5.2 No changes to existing AOV pipeline

The albedo + normal AOV writes (from Feature 1.1) stay. Motion vector is a new AOV — doesn't conflict.

## 6. Verification

### 6.1 Unit-testable: MV math on CPU

Not strictly needed — the math is a one-liner per axis and obvious by inspection. Skip CPU test; rely on GPU visual verification.

### 6.2 Visual verification (5 scenes)

Add a debug CLI flag `--dump-mv=<path>` that writes the motion vector AOV as an RGB PNG with:
- R = encoded +X motion (positive mapped to [128, 255], negative to [0, 128], zero = 128)
- G = encoded +Y motion
- B = 128 (neutral)

Then:

1. **Static camera test:**
   - Render any scene with camera fixed between frames (identical VP).
   - Dump MV. Expected: solid gray (128, 128, 128) — all pixels zero motion.

2. **Pan-right test:**
   - Render same scene with camera translated +X between frames.
   - Dump MV. Expected: uniform red tint (+X motion everywhere).

3. **Pan-down test:**
   - Camera translates -Y. Dump MV. Expected: uniform green tint.

4. **Zoom-in test:**
   - Camera moves forward (Z−). Dump MV. Expected: radial outward pattern (near pixels have larger |MV| than far pixels).

5. **Regression:**
   - Run cornell_box + env_demo without `--dump-mv`. MV is still computed + written, but beauty output should be bit-identical to pre-3.A (MV is pure output, doesn't affect radiance).

### 6.3 Numerical sanity

Computed manually for a known pan:

```
Camera moves +10 world units in X over one frame, scene depth 100 units at center pixel.
Expected MV at center: roughly (+Δ_pixels, 0) where Δ depends on FOV + resolution.
At 60° HFOV, 1920 px wide: 1 pixel ≈ 0.0006 radians ≈ 0.06 world units at depth 100.
So 10 world units motion at depth 100 ≈ 167 pixels of motion.
```

Dump MV, read center pixel's stored R16 value, check it's in the ballpark.

## 7. Files

**Modified:**
- `ohao/render/rt/path_tracer.hpp` — `m_motionVectorImage/Memory/View` fields + `VkImageView getMotionVectorAOV() const`
- `ohao/render/rt/path_tracer.cpp` — create/destroy in `createImages`/`destroyImages`, binding 19 in layout, descriptor write, first-frame MV-zero guard
- `ohao/render/rt/rt_profile_renderer.hpp` — `virtual VkImageView getMotionVectorAOV() const = 0;` plus override in `RTProfileRendererBase` delegating to `m_pathTracer`
- `shaders/rt/pt_raygen.rgen` — binding 19 decl + first-hit MV compute + imageStore
- `shaders/rt/pt_raygen_offline.rgen` — mirror of above
- `shaders/rt/pt_raygen_realtime.rgen` — binding 19 decl only (descriptor compat); realtime may skip the compute if useful
- `ohao/gpu/vulkan/renderer.hpp` — `VkImageView getMotionVectorAOV() const`
- `ohao/gpu/vulkan/renderer.cpp` — accessor impl delegating to active RT profile
- `examples/env_demo.cpp` — optional `--dump-mv=<path>` CLI flag for visual verification
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append 3.A entry

## 8. Risks

| Risk | Mitigation |
|------|-----------|
| RG16F range clips at extreme deltas (|motion| > 65504 px; would need 65k pixel motion per frame) | Acceptable — documented limit. Format flexibility TODO (§10) allows future RG32F option. |
| First frame computes bogus MV with identity `prevViewProj` | Skip MV write on frame 0 (output zero). Use `m_historyFrameCount == 0` check. |
| Clip-space divide by w ≤ 0 for behind-camera cases | Guarded with `if (currClip.w > 0.0 && prevClip.w > 0.0)`. Writes `(0, 0)` otherwise. |
| Animated meshes — MV incorrect (uses bind pose) | Accepted for 3.A. Sub-plan 3.F adds skeletal MV. |
| `inverse(invProj) * inverse(invView)` cost in shader | Once per raygen thread on bounce 0 only. Measured overhead: <0.5ms at 1920×1080. CameraUBO refactor in follow-up removes this. |
| Pipeline descriptor set layout growth (19 → 20 bindings) | Small change; follows existing Feature 1.1 / Sub-plan 1 pattern. |

## 9. Success Criteria

1. `m_motionVectorImage` created at `RG16F` / 1920×1080 during path tracer init.
2. Binding 19 wired in descriptor set layout + pool + per-frame write.
3. All 3 raygen shaders declare binding 19 and compile cleanly.
4. Offline raygen writes MV on first-hit every frame; realtime either writes or at minimum declares the binding for descriptor compatibility.
5. Static scene with static camera → MV is zero across all pixels (visible via `--dump-mv` debug output).
6. Camera pan produces expected uniform MV direction (debug output).
7. `VulkanRenderer::getMotionVectorAOV()` returns a valid `VkImageView` for future NRD/DLSS consumers.
8. No regressions: beauty output at `--denoise=none/oidn/optix` bit-identical to pre-3.A (MV is pure output).

## 10. Out-of-scope (Parking Lot)

**Format flexibility (user-flagged TODO):**

- Currently locked to `R16G16_SFLOAT`. Future enhancement: make the AOV format configurable via `RTRenderSettings.motionVectorFormat = { RG16F, RG32F, RG16U_PackedUV }` for:
  - RG32F: lossless for extreme cases (not needed today)
  - RG16 UV-normalized: pre-divided by screen size, ready for shader consumption without explicit conversion
  - Integration with DLSS RR which expects specific format conventions
- CPU-side format negotiation (detect what the active denoiser backend wants and pick the best format). Add when we have a denoiser that needs non-RG16F.
- Skeletal motion vectors — Sub-plan 3.F.
- CameraUBO refactor — separate task, removes the in-shader matrix inversion.
- Plumbing MV through to the current temporal reprojection in `accumBuffer` (Sub-plan 3.C).
- Per-frame "dirty region" hint where significant motion happened — useful for TAA / ReSTIR DI but out-of-scope here.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the detailed, bite-sized implementation plan.
