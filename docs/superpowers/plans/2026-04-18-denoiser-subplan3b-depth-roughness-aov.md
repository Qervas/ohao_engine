# Denoiser Sub-plan 3.B: Depth + Roughness AOVs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Path tracer emits two more first-hit AOVs on top of the existing beauty/albedo/normal/motion-vector outputs: `depthAOV` (R32F, linear view-space Z, NRD "ViewZ" convention) at binding 20 and `roughnessAOV` (R8 UNORM, perceptual roughness in `[0, 1]`) at binding 21. Output-only — no beauty/accumulation feedback.

**Architecture:** Two new `PathTracer`-owned storage images alongside the existing AOVs. Raygen first-hit block computes view-space Z via `inverse(pc.invView)` (same CameraUBO-refactor TODO as Sub-plan 3.A), unpacks roughness from the existing `payload.attenuation.x` encoding used by the BSDF sampler, writes both with `imageStore`. Miss rays write sentinel values (depth=1e30, roughness=1.0). Accessor chain mirrors MV 3.A pattern: `IRTRendererProfile` virtuals + delegating `RTProfileRendererBase` overrides + `VulkanRenderer` dispatchers. Debug readback helpers + `--dump-depth=` / `--dump-roughness=` CLI on env_demo for visual verification.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · `R32_SFLOAT` + `R8_UNORM` storage images · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-18-denoiser-subplan3b-depth-roughness-aov-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Add `m_depthAOVImage/Memory/View` + `m_roughnessAOVImage/Memory/View`; 4 getters (`getDepthAOV/Image`, `getRoughnessAOV/Image`). |
| `ohao/render/rt/path_tracer.cpp` | Create/destroy in `createImages`/`destroyImages`; bindings 20+21 in layout; pool STORAGE_IMAGE +2; descriptor writes; layout transitions in AOV barrier group. |
| `ohao/render/rt/rt_profile_renderer.hpp` | 4 pure virtuals + delegating overrides. |
| `ohao/gpu/vulkan/renderer.hpp` | 4 accessor decls + 2 readback helper decls. |
| `ohao/gpu/vulkan/renderer.cpp` | Accessor impls + readback helper impls. |
| `shaders/rt/pt_raygen.rgen` | Binding 20+21 decls + first-hit compute/imageStore. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Binding decls + compute/write (descriptor compat + realtime useful too). |
| `examples/env_demo.cpp` | `--dump-depth=<path>` + `--dump-roughness=<path>` CLI flags; encode/save PNGs. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.B entry. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-depthrough -b denoiser-3b-depth-roughness HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-depthrough`.

If the fresh worktree's `build/_deps/glm-src/` is empty (known bootstrap artifact):
```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/glm-src/. \
      /home/frankyin/Desktop/Github/ohao-depthrough/build/_deps/glm-src/
```

For OptiX support unchanged:
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Create / destroy depth + roughness storage images

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 1.1: Add private image handles to header

Edit `ohao/render/rt/path_tracer.hpp`. Find the block of AOV handles (search `m_motionVectorImage`). After it, add:

```cpp
    // Feature 3.B: view-space depth AOV (R32F)
    VkImage        m_depthAOVImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthAOVMemory = VK_NULL_HANDLE;
    VkImageView    m_depthAOVView = VK_NULL_HANDLE;

    // Feature 3.B: perceptual roughness AOV (R8 UNORM)
    VkImage        m_roughnessAOVImage = VK_NULL_HANDLE;
    VkDeviceMemory m_roughnessAOVMemory = VK_NULL_HANDLE;
    VkImageView    m_roughnessAOVView = VK_NULL_HANDLE;
```

Public accessors — near `getMotionVectorAOV`:

```cpp
    VkImageView getDepthAOV()         const { return m_depthAOVView; }
    VkImage     getDepthAOVImage()    const { return m_depthAOVImage; }
    VkImageView getRoughnessAOV()     const { return m_roughnessAOVView; }
    VkImage     getRoughnessAOVImage() const { return m_roughnessAOVImage; }
```

### Step 1.2: Create in createImages()

Edit `ohao/render/rt/path_tracer.cpp` `PathTracer::createImages()`. Find the final `return true;` and insert two new blocks right BEFORE it:

```cpp
    // ---- Feature 3.B: Depth AOV (R32F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_depthAOVImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_depthAOVImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_depthAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_depthAOVImage, m_depthAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthAOVImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthAOVView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.B: Roughness AOV (R8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_roughnessAOVImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_roughnessAOVImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_roughnessAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_roughnessAOVImage, m_roughnessAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_roughnessAOVImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_roughnessAOVView) != VK_SUCCESS) return false;
    }
```

### Step 1.3: Destroy in destroyImages()

Near the MV destroy calls, add:

```cpp
    if (m_depthAOVView)      { vkDestroyImageView(m_device, m_depthAOVView, nullptr);   m_depthAOVView = VK_NULL_HANDLE; }
    if (m_depthAOVImage)     { vkDestroyImage(m_device, m_depthAOVImage, nullptr);      m_depthAOVImage = VK_NULL_HANDLE; }
    if (m_depthAOVMemory)    { vkFreeMemory(m_device, m_depthAOVMemory, nullptr);       m_depthAOVMemory = VK_NULL_HANDLE; }

    if (m_roughnessAOVView)  { vkDestroyImageView(m_device, m_roughnessAOVView, nullptr); m_roughnessAOVView = VK_NULL_HANDLE; }
    if (m_roughnessAOVImage) { vkDestroyImage(m_device, m_roughnessAOVImage, nullptr);    m_roughnessAOVImage = VK_NULL_HANDLE; }
    if (m_roughnessAOVMemory){ vkFreeMemory(m_device, m_roughnessAOVMemory, nullptr);     m_roughnessAOVMemory = VK_NULL_HANDLE; }
```

### Step 1.4: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-depthrough
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t1_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: clean build. Cornell renders cleanly — nothing reads or writes the new images yet. No Vulkan validation errors.

### Step 1.5: Commit

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): depth + roughness AOV storage images (Sub-plan 3.B)

PathTracer gains m_depthAOVImage (R32F) and m_roughnessAOVImage (R8)
alongside the existing AOVs. Created in createImages, destroyed in
destroyImages. Accessors: getDepthAOV/Image, getRoughnessAOV/Image.
Descriptor binding + shader writes in subsequent commits.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: Descriptor bindings 20 + 21 + layout transitions

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 2.1: Extend bindings array

In `createDescriptorResources()`, find `VkDescriptorSetLayoutBinding bindings[20]` (size after Sub-plan 3.A). Grow to `[22]`. Add:

```cpp
    // Binding 20: depth AOV (R32F storage image) — Sub-plan 3.B
    bindings[20].binding         = 20;
    bindings[20].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[20].descriptorCount = 1;
    bindings[20].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 21: roughness AOV (R8 UNORM storage image) — Sub-plan 3.B
    bindings[21].binding         = 21;
    bindings[21].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[21].descriptorCount = 1;
    bindings[21].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Grow `bindingFlags[20]` → `[22]` if present (leave new entries at default 0). Update `layoutInfo.bindingCount = 22` and `flagsInfo.bindingCount = 22` similarly.

### Step 2.2: Grow the pool

Find `VkDescriptorPoolSize poolSizes[]`. Increment the `STORAGE_IMAGE` `descriptorCount` by 2 (previously grew by 1 for MV in 3.A).

### Step 2.3: Add descriptor writes in render()

Find where the MV descriptor write was added (search `dstBinding = 19` or `motionVectorInfo`). Grow `writes[]` array bound by 2. After the MV write, add:

```cpp
    // Binding 20: depth AOV
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = m_depthAOVView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 20;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &depthInfo;
    writeCount++;

    // Binding 21: roughness AOV
    VkDescriptorImageInfo roughInfo{};
    roughInfo.imageView   = m_roughnessAOVView;
    roughInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 21;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &roughInfo;
    writeCount++;
```

### Step 2.4: Add to AOV barrier group (UNDEFINED → GENERAL)

Find the AOV barrier block (from Sub-plan 3.A's layout fix commit — search `aovBarriers[3]` and grep for `m_motionVectorImage`). Grow from 3 barriers to 5. Add entries for depth + roughness:

```cpp
    // Sub-plan 3.B: depth AOV barrier
    aovBarriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[3].srcAccessMask = 0;
    aovBarriers[3].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[3].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[3].image = m_depthAOVImage;
    aovBarriers[3].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Sub-plan 3.B: roughness AOV barrier
    aovBarriers[4].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[4].srcAccessMask = 0;
    aovBarriers[4].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[4].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[4].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[4].image = m_roughnessAOVImage;
    aovBarriers[4].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
```

Update the `vkCmdPipelineBarrier` call to pass 5 instead of 3:
```cpp
vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
    0, 0, nullptr, 0, nullptr, 5, aovBarriers);
```

Also change the declaration to `VkImageMemoryBarrier aovBarriers[5] = {};`.

### Step 2.5: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-depthrough
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t2_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: clean build, no validation errors. (No shader writes to 20/21 yet.)

### Step 2.6: Commit

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): descriptor bindings 20+21 for depth + roughness AOVs

Bindings 20 (depth R32F) + 21 (roughness R8 UNORM), both STORAGE_IMAGE
raygen-only. Pool STORAGE_IMAGE count +2. Both images join the AOV
layout barrier group (UNDEFINED→GENERAL) alongside MV from 3.A.
Shader writes come in Task 4.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 3: Accessor chain (IRTRendererProfile + VulkanRenderer)

**Files:**
- Modify: `ohao/render/rt/rt_profile_renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`

### Step 3.1: IRTRendererProfile virtuals + base overrides

Edit `ohao/render/rt/rt_profile_renderer.hpp`. Near the existing `virtual VkImageView getMotionVectorAOV() const = 0;` + `virtual VkImage getMotionVectorImage() const = 0;` pair, add:

```cpp
    virtual VkImageView getDepthAOV()         const = 0;
    virtual VkImage     getDepthAOVImage()    const = 0;
    virtual VkImageView getRoughnessAOV()     const = 0;
    virtual VkImage     getRoughnessAOVImage() const = 0;
```

In `RTProfileRendererBase` (or equivalent concrete class delegating to `m_pathTracer`), add matching overrides near the existing MV overrides:

```cpp
    VkImageView getDepthAOV()         const override { return m_pathTracer.getDepthAOV(); }
    VkImage     getDepthAOVImage()    const override { return m_pathTracer.getDepthAOVImage(); }
    VkImageView getRoughnessAOV()     const override { return m_pathTracer.getRoughnessAOV(); }
    VkImage     getRoughnessAOVImage() const override { return m_pathTracer.getRoughnessAOVImage(); }
```

### Step 3.2: VulkanRenderer accessors

Edit `ohao/gpu/vulkan/renderer.hpp`. Near `getMotionVectorAOV` / `getMotionVectorImage`, add:

```cpp
    VkImageView getDepthAOV()         const;
    VkImage     getDepthAOVImage()    const;
    VkImageView getRoughnessAOV()     const;
    VkImage     getRoughnessAOVImage() const;
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Near the existing `VulkanRenderer::getMotionVectorAOV()` / `getMotionVectorImage()` impls, add:

```cpp
VkImageView VulkanRenderer::getDepthAOV() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDepthAOV();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDepthAOV();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getDepthAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDepthAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDepthAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getRoughnessAOV() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getRoughnessAOV();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getRoughnessAOV();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getRoughnessAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getRoughnessAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getRoughnessAOVImage();
    }
    return VK_NULL_HANDLE;
}
```

### Step 3.3: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-depthrough
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. If any concrete `IRTRendererProfile` subclass doesn't inherit from `RTProfileRendererBase`, it'll fail with "cannot instantiate abstract class" — add overrides there too.

### Step 3.4: Commit

```bash
git add ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp \
        ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): expose depth + roughness AOVs via VulkanRenderer

IRTRendererProfile gains 4 pure virtuals (VkImageView + VkImage getters
for both AOVs). RTProfileRendererBase delegates to m_pathTracer.
VulkanRenderer dispatches to the active profile.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 4: Shader — first-hit depth + roughness compute

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen` (mirror)
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 4.1: Declare bindings 20 + 21 in pt_raygen.rgen

Edit `shaders/rt/pt_raygen.rgen`. Right after the binding 19 declaration (motion vector from 3.A), add:

```glsl
layout(set = 0, binding = 20, r32f) uniform image2D depthAOV;
layout(set = 0, binding = 21, r8)   uniform image2D roughnessAOV;
```

### Step 4.2: Compute + write depth + roughness on first-hit

Find the Sub-plan 3.A motion vector block (grep `Sub-plan 3.A`). AFTER its `imageStore(motionVector, ...)` call, inside the same `if (bounce == 0u)` block, add:

```glsl
            // Sub-plan 3.B: depth (view-space Z) + roughness AOVs on first-hit
            float depth = 1e30;
            float rough = 1.0;
            if (firstHitDist > 0.0) {
                // View-space Z: right-handed → camera looks down -Z, so -viewPos.z = forward distance.
                // Reuses inverse(pc.invView) which Sub-plan 3.A already computes in this block —
                // reading `currViewProj`'s intermediate isn't exposed, so recompute the view inverse.
                mat4 viewMat = inverse(pc.invView);
                vec4 viewPos = viewMat * vec4(firstHitPos, 1.0);
                depth = -viewPos.z;

                // Roughness: unpack from payload.attenuation.x — encoding matches pt_closesthit.rchit.
                // Magic-number form: sign bit → metal flag, magnitude → roughness; values >= 10 are
                // shifted (historic encoding from Feature 1.1 era).
                float packed = payload.attenuation.x;
                rough = abs(packed);
                if (rough >= 10.0) rough -= 10.0;
                rough = clamp(rough, 0.01, 1.0);
            }
            imageStore(depthAOV,     pixel, vec4(depth, 0.0, 0.0, 0.0));
            imageStore(roughnessAOV, pixel, vec4(rough, 0.0, 0.0, 0.0));
```

**Placement note:** the new block must run BEFORE any NEE / env-MIS / BSDF-sampling code that might overwrite `payload.attenuation`. Since the MV block from 3.A runs right after `firstHitPos` capture and before all that logic, inserting the depth/roughness block there (same `if (bounce == 0u)` scope) is safe.

### Step 4.3: Mirror to pt_raygen_offline.rgen

`pt_raygen_offline.rgen` is a verbatim copy of pt_raygen.rgen except the top 3-4 line comment. Apply steps 4.1 + 4.2 identically.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only the top comment block differs.

### Step 4.4: Mirror to pt_raygen_realtime.rgen

Apply steps 4.1 + 4.2 to the realtime shader too. Realtime benefits from depth + roughness (NRD + DLSS RR will consume these).

### Step 4.5: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-depthrough
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile (`r32f` and `r8` are both valid storage-image qualifiers on Vulkan 1.3). Full app builds.

### Step 4.6: Regression smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t4_cornell.png 4 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4_helmet.png 16 --denoise=none 2>&1 | tail -3
```

Expected: both produce images. Beauty output bit-identical to pre-3.B — the new AOVs are pure output, no feedback into accumulation.

### Step 4.7: Commit

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): first-hit depth + roughness AOV writes

All three raygen shaders declare bindings 20 (r32f) + 21 (r8) and
compute view-space Z + unpacked roughness on bounce 0. Miss rays
write sentinels (depth=1e30, roughness=1.0). Mirrors 3.A placement
inside the existing first-hit if block.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 5: Readback helpers + CLI dumps + visual verification

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 5.1: Add readback helpers to VulkanRenderer

Edit `ohao/gpu/vulkan/renderer.hpp`. Near `readbackMotionVector`, add:

```cpp
    // Debug: readback the depth AOV as raw float buffer (1 float per pixel).
    bool readbackDepthAOV(std::vector<float>& depthData, uint32_t& width, uint32_t& height);

    // Debug: readback the roughness AOV as raw uint8 buffer (1 byte per pixel).
    bool readbackRoughnessAOV(std::vector<uint8_t>& roughData, uint32_t& width, uint32_t& height);
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Near `readbackMotionVector`, add:

```cpp
bool VulkanRenderer::readbackDepthAOV(std::vector<float>& depthData, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getDepthAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // R32F = 4 bytes
    depthData.resize(static_cast<size_t>(width) * height);

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

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(depthData.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackRoughnessAOV(std::vector<uint8_t>& roughData, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getRoughnessAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height; // R8 = 1 byte
    roughData.resize(static_cast<size_t>(width) * height);

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

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, byteCount, 0, &mapped);
    std::memcpy(roughData.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}
```

### Step 5.2: Add CLI flags to env_demo.cpp

Edit `examples/env_demo.cpp`. In the existing CLI parse loop (search `dump-mv`), extend:

```cpp
    std::string dumpDepthPath;
    std::string dumpRoughnessPath;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        // ... existing --denoise=, --dump-mv=, --pan-x= ...
        else if (arg.rfind("--dump-depth=", 0) == 0) {
            dumpDepthPath = arg.substr(13);
        } else if (arg.rfind("--dump-roughness=", 0) == 0) {
            dumpRoughnessPath = arg.substr(17);
        }
    }
```

After the existing MV dump block (search `--dump-mv` logic), add:

```cpp
    if (!dumpDepthPath.empty()) {
        std::vector<float> depthData;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDepthAOV(depthData, dw, dh)) {
            std::cerr << "[Depth dump] readback failed\n";
        } else {
            // Find max finite depth (exclude sentinels >= 1e20)
            float maxFinite = 0.0f;
            for (float d : depthData) if (d < 1e20f && d > maxFinite) maxFinite = d;
            if (maxFinite <= 0.0f) maxFinite = 1.0f;  // degenerate case

            // Encode as grayscale PNG: normalize finite range to [0, 255], sentinel → white
            std::vector<uint8_t> gray(static_cast<size_t>(dw) * dh, 255);
            for (uint32_t i = 0; i < dw * dh; i++) {
                float d = depthData[i];
                if (d < 1e20f) {
                    int v = static_cast<int>((d / maxFinite) * 255.0f);
                    gray[i] = static_cast<uint8_t>(std::max(0, std::min(255, v)));
                } // else sentinel → 255
            }
            stbi_write_png(dumpDepthPath.c_str(), dw, dh, 1, gray.data(), dw);
            std::cout << "Saved depth debug: " << dumpDepthPath
                      << " (max finite = " << maxFinite << ")" << std::endl;
        }
    }

    if (!dumpRoughnessPath.empty()) {
        std::vector<uint8_t> roughData;
        uint32_t rw = 0, rh = 0;
        if (!renderer.readbackRoughnessAOV(roughData, rw, rh)) {
            std::cerr << "[Roughness dump] readback failed\n";
        } else {
            // Direct grayscale PNG: R8 UNORM already in [0, 255]
            stbi_write_png(dumpRoughnessPath.c_str(), rw, rh, 1, roughData.data(), rw);
            std::cout << "Saved roughness debug: " << dumpRoughnessPath << std::endl;
        }
    }
```

Make sure `#include <algorithm>` is at the top (for `std::min`/`std::max`) — should already be present from the MV dump.

### Step 5.3: Build + visual tests

```bash
cd /home/frankyin/Desktop/Github/ohao-depthrough
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_helmet.png 16 --denoise=none \
    --dump-depth=/tmp/t5_depth.png \
    --dump-roughness=/tmp/t5_roughness.png 2>&1 | tail -5
```

Expected output lines include:
```
Saved depth debug: /tmp/t5_depth.png (max finite = <some value ~10-20 world units>)
Saved roughness debug: /tmp/t5_roughness.png
```

### Step 5.4: Visual check

- `/tmp/t5_depth.png`: grayscale. Helmet surfaces darker than sky background (sky = white because sentinel 1e30 > 1e20 threshold → encoded as 255). Helmet front (closer) slightly darker than helmet back.

- `/tmp/t5_roughness.png`: grayscale. Visor near-black (glossy, roughness ≈ 0.05-0.1 → 13-26 on 255 scale). Helmet metal body gray-dark (medium roughness). Sky background = 255 (full white, miss sentinel).

If either dump is uniform black or uniform white, shader writes or readback have a bug — investigate.

Copy to renders:

```bash
mkdir -p /home/frankyin/Desktop/Github/ohao_engine/renders
cp /tmp/t5_depth.png     /home/frankyin/Desktop/Github/ohao_engine/renders/depth_helmet.png
cp /tmp/t5_roughness.png /home/frankyin/Desktop/Github/ohao_engine/renders/roughness_helmet.png
```

### Step 5.5: Append to verification_log.md

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. APPEND:

```markdown
## 2026-04-18: Depth + roughness AOVs (Sub-plan 3.B) validation

Path tracer now writes view-space depth (R32F @ binding 20) and
perceptual roughness (R8 UNORM @ binding 21) on first-hit.

Verification on DamagedHelmet + env_studio, 16 spp, --denoise=none:
- **Depth:** grayscale dump shows helmet near-surfaces darker, back
  surfaces lighter, sky (miss) fully white (sentinel 1e30).
  Max finite depth recorded: <fill from output> world units.
  Saved: `renders/depth_helmet.png`.
- **Roughness:** visor near-black (glossy metal), helmet body dark
  gray, dielectric accessories lighter, sky white (miss sentinel 1.0).
  Saved: `renders/roughness_helmet.png`.
- **Regression:** beauty output at `--denoise=none/oidn/optix`
  bit-identical to pre-3.B. AOVs are pure output.

Sub-plan 3.B complete. Next: 3.C (history refactor — MV-aware
temporal reprojection + diffuse/specular split).
```

Fill `<fill from output>` with the actual max-depth value printed by Step 5.3.

### Step 5.6: Commit

```bash
git add ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): --dump-depth + --dump-roughness + visual verification (3.B)

Adds readbackDepthAOV + readbackRoughnessAOV helpers using the same
staging-buffer pattern as readbackMotionVector. env_demo grows
--dump-depth=<path> and --dump-roughness=<path> flags that encode
the AOVs as grayscale PNGs. Helmet scene produces the expected
depth gradient and roughness silhouette.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 `m_depthAOVImage/Memory/View` + `m_roughnessAOVImage/Memory/View` + 4 getters | Task 1 |
| §4.2 Descriptor bindings 20 + 21 + pool + writes + layout transition | Task 2 |
| §4.3 Shader binding decls + first-hit compute/imageStore + miss sentinels | Task 4 |
| §5.1 IRTRendererProfile virtuals + RTProfileRendererBase overrides + VulkanRenderer dispatchers | Task 3 |
| §5.2 Debug readback helpers | Task 5 |
| §5.3 CLI dump flags | Task 5 |
| §6 Verification (depth + roughness grayscale + regression) | Task 5 |

**Placeholder scan:** Only `<fill from output>` in the verification log template — deliberately filled at execution time with measured depth max.

**Type consistency:**
- `m_depthAOVImage/Memory/View` + `m_roughnessAOVImage/Memory/View` names identical across Tasks 1, 3, 5.
- `getDepthAOV` (→VkImageView), `getDepthAOVImage` (→VkImage), `getRoughnessAOV`, `getRoughnessAOVImage` naming consistent across all three layers (PathTracer + IRTRendererProfile + VulkanRenderer).
- Bindings 20 (depth) + 21 (roughness) used consistently in Task 2 (descriptor) + Task 4 (shader).
- GLSL format qualifiers `r32f` + `r8` match C++ format enums `VK_FORMAT_R32_SFLOAT` + `VK_FORMAT_R8_UNORM`.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-18-denoiser-subplan3b-depth-roughness-aov.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Same pattern that shipped 3.A.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
