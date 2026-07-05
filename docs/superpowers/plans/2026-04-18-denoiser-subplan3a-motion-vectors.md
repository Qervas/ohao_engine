# Denoiser Sub-plan 3.A: Camera Motion Vectors — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Path tracer emits a per-pixel 2D motion vector AOV (RG16F at binding 19) on first-hit, computed from previous + current viewProj matrices. Output-only AOV — does not affect beauty/accumulation. Required input for future NRD + DLSS RR.

**Architecture:** New `m_motionVectorImage` (R16G16_SFLOAT storage image) owned by `PathTracer`. Created/destroyed alongside existing AOVs. Descriptor binding 19 written in the raygen-stage layout. All three raygen shaders declare the binding. On first-hit only, raygen derives `currViewProj` in-shader via `inverse(invProj) * inverse(invView)`, projects `firstHitPos` through both current and `pc.prevViewProj`, computes pixel-space delta, writes to binding 19. First frame (`historyFrameCount == 0`) writes zero to avoid bogus motion from identity prevVP.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · RG16_SFLOAT storage image · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-18-denoiser-subplan3a-motion-vectors-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Add `m_motionVectorImage/Memory/View` fields + `VkImageView getMotionVectorAOV() const`. |
| `ohao/render/rt/path_tracer.cpp` | Create/destroy in `createImages`/`destroyImages`; descriptor binding 19; pool size +1 STORAGE_IMAGE; descriptor write. |
| `ohao/render/rt/rt_profile_renderer.hpp` | Add virtual `getMotionVectorAOV()` + override delegating to `m_pathTracer`. |
| `ohao/gpu/vulkan/renderer.hpp` | `VkImageView getMotionVectorAOV() const` accessor. |
| `ohao/gpu/vulkan/renderer.cpp` | Accessor impl delegating to active RT profile. |
| `shaders/rt/pt_raygen.rgen` | Binding 19 decl + first-hit MV compute + imageStore; first-frame guard. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of pt_raygen.rgen (same edits). |
| `shaders/rt/pt_raygen_realtime.rgen` | Binding 19 decl + MV compute (shares offline logic for descriptor compat + useful on its own). |
| `examples/env_demo.cpp` | Optional `--dump-mv=<path>` CLI flag — reads back MV AOV and saves as RGB debug PNG. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.A entry after visual tests. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-mv -b denoiser-3a-motion-vectors HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-mv`.

If the fresh worktree's `build/_deps/glm-src/` is empty (known bootstrap artifact):
```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/glm-src/. \
      /home/frankyin/Desktop/Github/ohao-mv/build/_deps/glm-src/
```

If using OptiX (unchanged from Sub-plan 2 — needs `OPTIX_ROOT` env var):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Create / destroy motion vector storage image

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 1.1: Add private image handles to header

Edit `ohao/render/rt/path_tracer.hpp`. Find the existing block of AOV handles (search `m_albedoAOV`). Immediately after `m_normalAOV` view (or wherever AOVs are grouped), add:

```cpp
    // Feature 3.A: camera motion vector AOV (RG16F)
    VkImage        m_motionVectorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_motionVectorMemory = VK_NULL_HANDLE;
    VkImageView    m_motionVectorView = VK_NULL_HANDLE;
```

Also add the public accessor in the PathTracer's public section (near `getAlbedoAOV`, `getNormalAOV`):

```cpp
VkImageView getMotionVectorAOV() const { return m_motionVectorView; }
```

### Step 1.2: Create the image in createImages

Edit `ohao/render/rt/path_tracer.cpp` `PathTracer::createImages()`. The existing function creates `m_accumBuffer`, `m_outputImage`, `m_albedoAOV`, `m_normalAOV`, and history buffers each as a self-contained block. Right BEFORE the final `return true;`, add a new block:

```cpp
    // ---- Feature 3.A: Motion vector AOV (RG16F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_motionVectorImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_motionVectorImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_motionVectorMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_motionVectorImage, m_motionVectorMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_motionVectorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_motionVectorView) != VK_SUCCESS) return false;
    }
```

`TRANSFER_SRC_BIT` is included so the debug `--dump-mv` path (Task 5) can read back via staging buffer. Cost is trivial.

### Step 1.3: Destroy in destroyImages

Edit `ohao/render/rt/path_tracer.cpp` `PathTracer::destroyImages()`. Near the existing AOV destroy calls, add:

```cpp
    if (m_motionVectorView)    { vkDestroyImageView(m_device, m_motionVectorView, nullptr); m_motionVectorView = VK_NULL_HANDLE; }
    if (m_motionVectorImage)   { vkDestroyImage(m_device, m_motionVectorImage, nullptr);    m_motionVectorImage = VK_NULL_HANDLE; }
    if (m_motionVectorMemory)  { vkFreeMemory(m_device, m_motionVectorMemory, nullptr);     m_motionVectorMemory = VK_NULL_HANDLE; }
```

### Step 1.4: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-mv
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t1_cornell.png 4 2>&1 | tail -3
```

Expected: clean build, cornell renders cleanly. No visible change — image exists but is neither written to nor read from yet. No Vulkan validation errors.

### Step 1.5: Commit

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): motion vector AOV storage image (RG16F, Sub-plan 3.A)

PathTracer gains m_motionVectorImage/Memory/View — RG16F storage image
created in createImages, destroyed in destroyImages. Accessor
getMotionVectorAOV() exposes the view. Descriptor binding + shader
write come in the next commits.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: Descriptor binding 19 + descriptor write

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 2.1: Extend bindings array

Edit `ohao/render/rt/path_tracer.cpp` `createDescriptorResources()`. Find `VkDescriptorSetLayoutBinding bindings[N]` — grep for the existing declaration (after Feature 1.1 it was sized 19, containing bindings 0..18).

Grow to `[20]` and add entry 19:

```cpp
    // Binding 19: motion vector AOV (RG16F storage image) — Sub-plan 3.A
    bindings[19].binding         = 19;
    bindings[19].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[19].descriptorCount = 1;
    bindings[19].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

If there is a companion `VkDescriptorBindingFlags bindingFlags[19]` array, grow it to `[20]` and leave index 19 at default (0). Update the layout's `bindingCount` from 19 to 20.

### Step 2.2: Grow the descriptor pool

Still in `createDescriptorResources`. Find the `VkDescriptorPoolSize poolSizes[]` table (grep for `STORAGE_IMAGE` near the pool). Increment the `STORAGE_IMAGE` `descriptorCount` by 1.

Previous count covered accumBuffer + output + albedoAOV + normalAOV + history(×4 — 2 surface ping + 2 shading ping) = 8. New count = 9.

### Step 2.3: Extend descriptor writes in render()

In `PathTracer::render()`, find the descriptor-writes build-up (grep for `VkWriteDescriptorSet writes[` near a section that populates bindings 11, 12, 13, …). Grow the `writes[]` array by 1 (so it can hold 20 entries total if indices are contiguous; or grow the dynamic `writeCount`-driven version — match existing style).

Add the write for binding 19 BEFORE `vkUpdateDescriptorSets`:

```cpp
    // Binding 19: motion vector AOV
    VkDescriptorImageInfo motionVectorInfo{};
    motionVectorInfo.imageView   = m_motionVectorView;
    motionVectorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 19;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &motionVectorInfo;
    writeCount++;
```

### Step 2.4: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-mv
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t2_cornell.png 4 2>&1 | tail -3
```

Expected: clean build, cornell renders. No validation errors. The binding exists but no shader declares or writes to it yet — Vulkan will warn only if a shader declares the binding and the descriptor set is missing it. Currently no shader references binding 19, so nothing warns.

### Step 2.5: Commit

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): descriptor binding 19 for motion vector AOV

Binding 19 = STORAGE_IMAGE, raygen stage only. Pool STORAGE_IMAGE
count +1. Descriptor write populated in render(). Shader use lands
in the next commit.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 3: Virtual accessor on `IRTRendererProfile` + `VulkanRenderer::getMotionVectorAOV`

**Files:**
- Modify: `ohao/render/rt/rt_profile_renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`

### Step 3.1: IRTRendererProfile virtual + RTProfileRendererBase override

Edit `ohao/render/rt/rt_profile_renderer.hpp`. Find `class IRTRendererProfile` (or interface). Near the existing `VkImage getNormalAOV() const = 0;` / `VkImageView` accessors, add:

```cpp
    virtual VkImageView getMotionVectorAOV() const = 0;
```

In the concrete `RTProfileRendererBase` (or `RTRealtimeRenderer` / `RTOfflineRenderer` if that's the concrete class), add the override delegating to `m_pathTracer`:

```cpp
    VkImageView getMotionVectorAOV() const override { return m_pathTracer.getMotionVectorAOV(); }
```

Mirror the existing `getAlbedoAOV` / `getNormalAOV` override pattern — if they return `VkImage`, follow that; if they return `VkImageView`, use `VkImageView`. Check the existing signatures.

### Step 3.2: Add VulkanRenderer accessor

Edit `ohao/gpu/vulkan/renderer.hpp`. Near the existing AOV accessors (search `getAlbedoAOV` / `getNormalAOV`), add:

```cpp
// Returns the motion vector AOV image view from the active RT profile.
// Nullopt (VK_NULL_HANDLE) if no RT profile is active.
VkImageView getMotionVectorAOV() const;
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Near the existing accessor implementations, add:

```cpp
VkImageView VulkanRenderer::getMotionVectorAOV() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getMotionVectorAOV();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getMotionVectorAOV();
    }
    return VK_NULL_HANDLE;
}
```

Adjust the exact conditional shape to match how the existing AOV accessors dispatch across profiles — grep for `getAlbedoAOV` implementation in renderer.cpp to see the pattern.

### Step 3.3: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-mv
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. If `IRTRendererProfile` has multiple concrete subclasses, ensure each overrides `getMotionVectorAOV()` — otherwise the abstract class complaint will fire.

### Step 3.4: Commit

```bash
git add ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): expose motion vector AOV via VulkanRenderer

IRTRendererProfile::getMotionVectorAOV() pure virtual delegates to
m_pathTracer. VulkanRenderer::getMotionVectorAOV() dispatches to the
active profile. NRD/DLSS integrations bind the view via this accessor
once they land in Sub-plans 3.D + 4 + 5.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 4: Shader — first-hit MV compute + imageStore

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen` (mirror of pt_raygen)
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 4.1: Declare binding 19 in pt_raygen.rgen

Edit `shaders/rt/pt_raygen.rgen`. Near the existing AOV bindings (search `binding = 6` or `albedoAOV`), add after them:

```glsl
layout(set = 0, binding = 19, rg16f) uniform image2D motionVector;
```

### Step 4.2: Compute + write MV on first-hit

In `main()`, find where `firstHitPos` is captured — from Feature 1.1 the existing code is:

```glsl
        // Store first hit for temporal reprojection
        if (bounce == 0u) {
            firstHitPos = payload.hitPos;
            firstHitDist = payload.hitDist;
        }
```

After this `if` block (same scope, same indentation), add:

```glsl
        // Sub-plan 3.A: motion vector on first-hit
        if (bounce == 0u) {
            vec2 motion = vec2(0.0);
            // First frame: prev viewProj is identity or stale — force zero
            if (firstHitDist > 0.0 && historyFrameCount > 0u) {
                // Derive currViewProj in-shader (spec §4.3). Cost: 2 mat4 inverts
                // once per raygen thread on bounce 0 — removed by the CameraUBO
                // refactor (out-of-scope for 3.A).
                mat4 currViewProj = inverse(pc.invProj) * inverse(pc.invView);

                vec4 currClip = currViewProj  * vec4(firstHitPos, 1.0);
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

Note: `historyFrameCount` is already declared earlier in `main()` from `pc.control.y`. Verify it exists at this point in the shader — grep for `historyFrameCount` before dropping in the edit.

The `imageStore` writes a `vec4` even for a 2-channel image; the GPU writes only the R and G channels (the extra 0.0 values are ignored).

### Step 4.3: Mirror to pt_raygen_offline.rgen

`pt_raygen_offline.rgen` is a verbatim copy of `pt_raygen.rgen` except for the top 4-line comment. Apply steps 4.1 + 4.2 identically.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head -10
```
Expected: only the top comment block differs.

### Step 4.4: Mirror to pt_raygen_realtime.rgen

Apply both edits to `pt_raygen_realtime.rgen` as well. Realtime also benefits from MV (realtime NRD in Sub-plan 4 consumes it) — no reason to skip.

### Step 4.5: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-mv
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile (glslc accepts `rg16f` and `inverse()` without issue). Full app builds.

### Step 4.6: Smoke test — rendering unchanged

```bash
./build/cornell_box /tmp/t4_cornell.png 4 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4_helmet.png 16 --denoise=none 2>&1 | tail -3
```

Expected: both produce images. Beauty output is pixel-identical to pre-3.A (MV is pure output — doesn't feed back into the beauty pipeline). Spot-check visually if you want. No Vulkan validation errors.

### Step 4.7: Commit

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): motion vector compute + write on first-hit

All three raygen shaders declare binding 19 and compute per-pixel 2D
screen-space motion (pixel-delta units) on bounce 0. Derives
currViewProj in-shader from invProj + invView; projects firstHitPos
through current and prev VP. First frame (historyFrameCount == 0)
writes zero. Clip-space w guards prevent divide-by-zero.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 5: `--dump-mv` CLI + visual verification

**Files:**
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 5.1: Add `--dump-mv=<path>` arg to env_demo.cpp

Edit `examples/env_demo.cpp`. In the existing arg-parse loop (starts around line 35 iterating `argv[5..]`), add:

```cpp
    std::string dumpMvPath;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--dump-mv=", 0) == 0) {
            dumpMvPath = arg.substr(10);
        }
        // ... existing arg handling ...
    }
```

### Step 5.2: Readback + encode + save MV debug PNG

After `renderer.render()` returns (or after `renderer.getPixels()` is called — either is fine), AFTER the main beauty-PNG save, add:

```cpp
    if (!dumpMvPath.empty()) {
        VkImageView mvView = renderer.getMotionVectorAOV();
        if (mvView != VK_NULL_HANDLE) {
            // Read back the RG16F MV buffer via staging buffer. The renderer
            // already has a general readback helper for RGBA32F AOVs
            // (readbackHDRBuffers). For this debug path we add a targeted
            // readback: the renderer exposes the image + we copy it via a
            // one-shot command buffer.
            //
            // To keep the change minimal, use the existing pattern:
            // (W, H) from renderer; allocate a staging buffer of 4 bytes per
            // pixel (RG16 = 4 bytes); vkCmdCopyImageToBuffer; map + read.
            //
            // Simpler: since this is a debug-only path, do the readback
            // inline with a helper in VulkanRenderer::readbackMotionVector.
            // Add that method now.
            std::vector<uint16_t> mvRaw;  // 2 halfs per pixel
            uint32_t mw = 0, mh = 0;
            if (!renderer.readbackMotionVector(mvRaw, mw, mh)) {
                std::cerr << "[MV dump] readback failed\n";
            } else {
                // Encode to RGB8: +X → red, +Y → green, neutral = 128
                std::vector<uint8_t> rgb(mw * mh * 3, 128);
                auto half2float = [](uint16_t h) -> float {
                    // IEEE 754 half-precision to float
                    uint32_t sign = (h >> 15) & 0x1;
                    uint32_t exp  = (h >> 10) & 0x1f;
                    uint32_t mant = h & 0x3ff;
                    uint32_t f;
                    if (exp == 0) {
                        if (mant == 0) { f = sign << 31; }
                        else {
                            exp = 1;
                            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
                            mant &= 0x3ff;
                            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                        }
                    } else if (exp == 0x1f) {
                        f = (sign << 31) | (0xff << 23) | (mant << 13);
                    } else {
                        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                    }
                    float out;
                    std::memcpy(&out, &f, 4);
                    return out;
                };
                const float scale = 0.02f;  // visibility scale — 50px motion → full saturation
                for (uint32_t i = 0; i < mw * mh; i++) {
                    float dx = half2float(mvRaw[i * 2 + 0]);
                    float dy = half2float(mvRaw[i * 2 + 1]);
                    int r = static_cast<int>(128.0f + std::max(-1.0f, std::min(1.0f, dx * scale)) * 127.0f);
                    int g = static_cast<int>(128.0f + std::max(-1.0f, std::min(1.0f, dy * scale)) * 127.0f);
                    rgb[i * 3 + 0] = static_cast<uint8_t>(r);
                    rgb[i * 3 + 1] = static_cast<uint8_t>(g);
                    rgb[i * 3 + 2] = 128;
                }
                stbi_write_png(dumpMvPath.c_str(), mw, mh, 3, rgb.data(), mw * 3);
                std::cout << "Saved MV debug: " << dumpMvPath << std::endl;
            }
        } else {
            std::cerr << "[MV dump] no RT profile active — cannot read MV AOV\n";
        }
    }
```

Add include at top if not already present:
```cpp
#include <cstring>
#include <algorithm>
```

### Step 5.3: Add `readbackMotionVector` helper to VulkanRenderer

Edit `ohao/gpu/vulkan/renderer.hpp`. Near `readbackHDRBuffers`, add:

```cpp
// Debug: readback the motion vector AOV as raw uint16_t pairs (RG16F interleaved).
// One 2-half pair per pixel; total size = 2 * width * height uint16_t values.
// Returns false if no RT profile is active or readback fails.
bool readbackMotionVector(std::vector<uint16_t>& mvRaw, uint32_t& width, uint32_t& height);
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Implement, following the `readbackHDRBuffers` pattern:

```cpp
bool VulkanRenderer::readbackMotionVector(std::vector<uint16_t>& mvRaw, uint32_t& width, uint32_t& height) {
    VkImageView mvView = getMotionVectorAOV();
    if (mvView == VK_NULL_HANDLE) return false;

    // We need the VkImage (not just the view) for vkCmdCopyImageToBuffer.
    // The simplest path: expose a VkImage getter alongside the view, or
    // do the readback inside the PathTracer. For a debug-only helper,
    // add a getter.
    VkImage mvImage = VK_NULL_HANDLE;
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        mvImage = m_rtOfflineRenderer->getMotionVectorImage();
    } else if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        mvImage = m_rtRealtimeRenderer->getMotionVectorImage();
    }
    if (mvImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RG16 = 4 bytes
    mvRaw.resize(static_cast<size_t>(width) * height * 2);

    // Staging buffer
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_commandPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition MV image to TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = mvImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition MV image back to GENERAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(mvRaw.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}
```

And add the `getMotionVectorImage()` getter to `IRTRendererProfile` + override. Mirror the `getMotionVectorAOV()` pattern from Task 3. Add the pure virtual to the interface and the delegating override in `RTProfileRendererBase`. `PathTracer` needs a public `getMotionVectorImage() const { return m_motionVectorImage; }` — add that too (the `m_motionVectorImage` field itself already exists from Task 1).

### Step 5.4: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-mv
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

### Step 5.5: Static camera test — expect near-zero MV

Render the same scene twice with identical camera, dump the MV:

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_static.png 16 --denoise=none --dump-mv=/tmp/t5_mv_static.png 2>&1 | tail -3
```

Inspect `/tmp/t5_mv_static.png` — should be near-uniform gray `(128, 128, 128)`. Tiny fluctuations from sub-pixel camera jitter (if enabled) are fine. Save this image to `renders/`:

```bash
cp /tmp/t5_mv_static.png /home/frankyin/Desktop/Github/ohao_engine/renders/mv_static.png
```

### Step 5.6: Camera pan test — expect uniform red tint

The existing examples don't have multi-frame runs with moving cameras out of the box. Modify `env_demo.cpp` temporarily for this test: between two consecutive `renderer.render()` calls, translate the camera by a small world-space X offset, then re-render and dump the MV on the second frame.

Pragmatic alternative: add a brief test inside env_demo.cpp under an `--mv-test` CLI flag that does:

```cpp
// Optional: --mv-test pans the camera between two renders and dumps frame 2's MV
if (std::string(argv[5]) == "--mv-test") {   // only when explicit
    renderer.render();                        // Frame 1 — establish prevViewProj
    auto& cam = renderer.getCamera();
    cam.setPosition(cam.getPosition() + glm::vec3(0.5f, 0.0f, 0.0f));  // pan +X
    renderer.render();                        // Frame 2 — should compute MV
    // continue to dump-mv save logic
}
```

This is inline test code — not for permanent shipping. Save the result:

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_pan.png 16 --dump-mv=/tmp/t5_mv_pan.png --mv-test 2>&1 | tail -3
cp /tmp/t5_mv_pan.png /home/frankyin/Desktop/Github/ohao_engine/renders/mv_pan_right.png
```

Visually inspect. Expected: uniform reddish tint (all pixels shifted in +X screen space as camera pans right → scene appears to move left from the camera's POV → negative MV.x, so `dx * scale * 127` produces a value below 128 → darker red on all pixels; or flip scale sign if you want "camera moves right = pixel moves left = red tint").

(Choose the encoding sign convention that feels natural — the spec doesn't mandate. Document the final choice in the verification log.)

If the test instrumentation is too fiddly, an acceptable simpler alternative: keep only the static-camera test in Task 5 and treat the pan test as manual-visual validation when you have multi-frame rendering later.

### Step 5.7: Append to verification log

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. APPEND:

```markdown
## 2026-04-18: Camera motion vectors (Sub-plan 3.A) validation

Path tracer now writes per-pixel 2D MV AOV (RG16F @ binding 19) on
first-hit. Camera motion only; skeletal is 3.F.

Verification:
- **Static camera:** MV dump is uniform gray (128, 128, 128) — zero motion.
  Saved: `renders/mv_static.png`.
- **Camera pan +X:** MV dump shows uniform directional tint.
  Saved: `renders/mv_pan_right.png`.
- **Regression:** beauty output at `--denoise=none` bit-identical to
  pre-3.A — MV is pure output AOV.

Known limitation: first frame outputs zero MV (prevViewProj is stale).
This is documented behavior. MV-driven temporal reprojection lands in
Sub-plan 3.C.

Sub-plan 3.A complete. Next: 3.B (depth + roughness AOVs).
```

### Step 5.8: Commit

```bash
git add examples/env_demo.cpp \
        ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        ohao/render/rt/path_tracer.hpp \
        ohao/render/rt/rt_profile_renderer.hpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): --dump-mv + static/pan verification for Sub-plan 3.A

env_demo grows --dump-mv=<path> that reads back the MV AOV and saves
as RGB debug PNG (encoded +X→red, +Y→green, neutral=128).
Static-camera dump is uniform gray; camera-pan dump shows uniform
directional tint. VulkanRenderer gains readbackMotionVector helper +
getMotionVectorImage accessor chain. Beauty output unchanged.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 `m_motionVectorImage/Memory/View` in PathTracer + accessor | Task 1 |
| §4.2 Descriptor binding 19 + pool + write | Task 2 |
| §4.3 Shader: binding decl + first-hit compute + imageStore + first-frame guard + clip-w guard | Task 4 |
| §4.4 First-frame prevViewProj handling | Task 4 (via `historyFrameCount > 0u` check) |
| §5.1 VulkanRenderer::getMotionVectorAOV accessor | Task 3 |
| §5.2 No changes to existing AOVs | All tasks — preserved |
| §6 Verification (static + pan + regression) | Task 5 |

**Placeholder scan:** No `<FILL>` / `<TBD>`. Task 5's "choose encoding sign convention" has a concrete instruction + alternative and fills in the final choice during execution.

**Type consistency:**
- `m_motionVectorImage/Memory/View` names identical across Task 1, 3, 5.
- `getMotionVectorAOV` / `getMotionVectorImage` naming: the former returns `VkImageView` (for descriptor binding in external systems like NRD); the latter returns `VkImage` (for Vulkan transfer operations like the readback). Separate getters is the right split — documented in Task 5.
- Binding 19 used consistently in Task 2 (descriptor) + Task 4 (shader).
- `pc.invView` / `pc.invProj` / `pc.prevViewProj` / `pc.params.xy` all exist in the post-Feature-1.1 push-constant shape — confirmed by grep of `pt_raygen.rgen` at plan-writing time.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-18-denoiser-subplan3a-motion-vectors.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Same pattern that shipped Sub-plans 1 and 2.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
