# Denoiser Sub-plan 3.C.6: Demod AOV Exposure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raygen stops pre-dividing radiance by albedo/F0 at exit. Two new AOVs at bindings 24 (`VK_FORMAT_R8G8B8A8_UNORM` diffuse albedo, `isMetal ? 0 : albedo`) and 25 (`VK_FORMAT_R8G8B8A8_UNORM` specular color, `isMetal ? albedo : vec3(0.04)`) expose the demod factors so NRD remodulates downstream. Removes the 100×-amplification-on-dark-channels bug in the existing demod division.

**Architecture:** Same 5-task shape as 3.B / 3.C (storage images → bindings → accessor chain → shader → readback+verify). Key design departure from earlier sub-plans: T4 both ADDS new AOV writes AND REMOVES the existing demod division at raygen exit — radiance semantics flip from demodulated to raw within a single shader commit. Beauty path stays untouched; only the diffuse/specular AOV semantics change.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · `R8G8B8A8_UNORM` storage images · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-19-denoiser-subplan3c6-demod-aov-exposure-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Add `m_diffAlbedoImage/Memory/View` + `m_specColorImage/Memory/View`; 4 getters. |
| `ohao/render/rt/path_tracer.cpp` | Create/destroy in `createImages`/`destroyImages`; bindings 24+25 in layout; pool STORAGE_IMAGE +2 (13→15); descriptor writes; AOV barrier group [7]→[9]. |
| `ohao/render/rt/rt_profile_renderer.hpp` | 4 pure virtuals + 4 delegating overrides. |
| `ohao/gpu/vulkan/renderer.hpp` | 4 accessor decls + 2 readback helper decls. |
| `ohao/gpu/vulkan/renderer.cpp` | Accessor impls + readback impls. |
| `shaders/rt/pt_raygen.rgen` | Binding decls 24+25 + top-of-main zero-init + bounce-0 capture imageStore + replace demod block with raw writes at exit. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Same edits. |
| `examples/env_demo.cpp` | `--dump-diff-albedo=<path>` + `--dump-spec-color=<path>` CLI flags + 2 new dump blocks. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C.6 entry. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-3c6-demod-aovs -b denoiser-3c6-demod-aov-exposure HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-3c6-demod-aovs`.

If the fresh worktree's `build/` is empty, configure + bootstrap from master if needed:
```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -10
# If build/_deps/glm-src/ is empty:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

For OptiX (optional):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Create / destroy diff-albedo + spec-color storage images

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 1.1: Add private image handles to header

Edit `ohao/render/rt/path_tracer.hpp`. Find the 3.C specular radiance block (search `m_specularRadianceImage`). After it, add:

```cpp
    // Feature 3.C.6: diffuse albedo AOV for NRD remodulate (RGBA8 UNORM)
    VkImage        m_diffAlbedoImage = VK_NULL_HANDLE;
    VkDeviceMemory m_diffAlbedoMemory = VK_NULL_HANDLE;
    VkImageView    m_diffAlbedoView = VK_NULL_HANDLE;

    // Feature 3.C.6: specular color / F0 AOV for NRD remodulate (RGBA8 UNORM)
    VkImage        m_specColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_specColorMemory = VK_NULL_HANDLE;
    VkImageView    m_specColorView = VK_NULL_HANDLE;
```

Public accessors — near `getSpecularRadianceAOV`:

```cpp
    VkImageView getDiffAlbedoAOV()      const { return m_diffAlbedoView; }
    VkImage     getDiffAlbedoAOVImage() const { return m_diffAlbedoImage; }
    VkImageView getSpecColorAOV()       const { return m_specColorView; }
    VkImage     getSpecColorAOVImage()  const { return m_specColorImage; }
```

### Step 1.2: Create in createImages()

Edit `ohao/render/rt/path_tracer.cpp` `PathTracer::createImages()`. Find the 3.C specular radiance block (ends with `if (vkCreateImageView(...) ... &m_specularRadianceView)`). After it, before the final `return true;`, insert:

```cpp
    // ---- Feature 3.C.6: Diffuse albedo AOV (RGBA8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_diffAlbedoImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_diffAlbedoImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_diffAlbedoMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_diffAlbedoImage, m_diffAlbedoMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_diffAlbedoImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_diffAlbedoView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C.6: Specular color / F0 AOV (RGBA8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specColorImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_specColorImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_specColorMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_specColorImage, m_specColorMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_specColorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_specColorView) != VK_SUCCESS) return false;
    }
```

### Step 1.3: Destroy in destroyImages()

Near the 3.C `m_specularRadiance*` destroy lines, add:

```cpp
    if (m_diffAlbedoView)    { vkDestroyImageView(m_device, m_diffAlbedoView, nullptr);   m_diffAlbedoView = VK_NULL_HANDLE; }
    if (m_diffAlbedoImage)   { vkDestroyImage(m_device, m_diffAlbedoImage, nullptr);      m_diffAlbedoImage = VK_NULL_HANDLE; }
    if (m_diffAlbedoMemory)  { vkFreeMemory(m_device, m_diffAlbedoMemory, nullptr);       m_diffAlbedoMemory = VK_NULL_HANDLE; }

    if (m_specColorView)     { vkDestroyImageView(m_device, m_specColorView, nullptr);   m_specColorView = VK_NULL_HANDLE; }
    if (m_specColorImage)    { vkDestroyImage(m_device, m_specColorImage, nullptr);      m_specColorImage = VK_NULL_HANDLE; }
    if (m_specColorMemory)   { vkFreeMemory(m_device, m_specColorMemory, nullptr);       m_specColorMemory = VK_NULL_HANDLE; }
```

### Step 1.4: Build + smoke

- [ ] **Step 1.4a: Configure + build**

```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 1.4b: Cornell smoke**

```bash
./build/cornell_box /tmp/t1_3c6_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: `Saved: /tmp/t1_3c6_cornell.png`. No validation errors.

### Step 1.5: Commit

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): diff-albedo + spec-color AOV storage images (Sub-plan 3.C.6)

PathTracer gains m_diffAlbedoImage (RGBA8) and m_specColorImage (RGBA8)
alongside the existing AOVs. Created in createImages, destroyed in
destroyImages. Accessors: getDiffAlbedoAOV/Image, getSpecColorAOV/Image.
Descriptor binding + shader writes in subsequent commits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: Descriptor bindings 24 + 25 + AOV layout transitions

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 2.1: Extend bindings array

In `createDescriptorResources()`, find the current `VkDescriptorSetLayoutBinding bindings[24]` (size after 3.C). Grow to `[26]`. Also grow `bindingFlags[24]` → `[26]` (leave new entries at default 0), and update both `flagsInfo.bindingCount = 26` and `layoutInfo.bindingCount = 26`.

After the binding 23 (specular radiance) entries, add:

```cpp
    // Binding 24: diffuse albedo AOV (RGBA8 UNORM storage image) — Sub-plan 3.C.6
    bindings[24].binding         = 24;
    bindings[24].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[24].descriptorCount = 1;
    bindings[24].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 25: specular color AOV (RGBA8 UNORM storage image) — Sub-plan 3.C.6
    bindings[25].binding         = 25;
    bindings[25].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[25].descriptorCount = 1;
    bindings[25].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

### Step 2.2: Grow the pool

Find `VkDescriptorPoolSize poolSizes[]`. Increment the `STORAGE_IMAGE` `descriptorCount` by 2. After 3.C it was 13; set to 15. Extend the running comment (e.g., `// +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec (3.C), +2 albedo/specColor (3.C.6)`).

### Step 2.3: Add descriptor writes in render()

Find the existing specular radiance write (search `dstBinding = 23` or `specRadianceInfo`). Grow `writes[]` array bound by 2 (e.g., `writes[24]` → `writes[26]`). After the specular radiance write, add:

```cpp
    // Binding 24: diffuse albedo — Sub-plan 3.C.6
    VkDescriptorImageInfo diffAlbedoInfo{};
    diffAlbedoInfo.imageView   = m_diffAlbedoView;
    diffAlbedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 24;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffAlbedoInfo;
    writeCount++;

    // Binding 25: specular color — Sub-plan 3.C.6
    VkDescriptorImageInfo specColorInfo{};
    specColorInfo.imageView   = m_specColorView;
    specColorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 25;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specColorInfo;
    writeCount++;
```

### Step 2.4: Add to AOV barrier group (UNDEFINED → GENERAL)

Find the AOV barrier block (search `aovBarriers[7]` — size after 3.C). Grow declaration to `aovBarriers[9] = {};`. After the specular radiance entry (index 6), add:

```cpp
    // Sub-plan 3.C.6: diffuse albedo AOV barrier
    aovBarriers[7].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[7].srcAccessMask = 0;
    aovBarriers[7].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[7].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[7].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[7].image = m_diffAlbedoImage;
    aovBarriers[7].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Sub-plan 3.C.6: specular color AOV barrier
    aovBarriers[8].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[8].srcAccessMask = 0;
    aovBarriers[8].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[8].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[8].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[8].image = m_specColorImage;
    aovBarriers[8].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
```

Update `vkCmdPipelineBarrier`'s barrier count from 7 → 9:
```cpp
vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
    0, 0, nullptr, 0, nullptr, 9, aovBarriers);
```

### Step 2.5: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t2_3c6_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: clean build, no validation errors. Shaders still don't write bindings 24/25; images remain zero after barriers.

### Step 2.6: Commit

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): descriptor bindings 24+25 for diff-albedo + spec-color AOVs

Bindings 24 (diffuse albedo RGBA8) + 25 (specular color RGBA8), both
STORAGE_IMAGE raygen-only. Pool STORAGE_IMAGE count +2 (13→15). Both
images join the AOV layout barrier group (UNDEFINED→GENERAL), count
7→9. Shader writes come in Task 4.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Accessor chain (IRTRendererProfile + VulkanRenderer)

**Files:**
- Modify: `ohao/render/rt/rt_profile_renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`

### Step 3.1: IRTRendererProfile virtuals + base overrides

Edit `ohao/render/rt/rt_profile_renderer.hpp`. After the 3.C specular-radiance virtuals (search `getSpecularRadianceAOV`), add:

```cpp
    virtual VkImageView getDiffAlbedoAOV()      const = 0;
    virtual VkImage     getDiffAlbedoAOVImage() const = 0;
    virtual VkImageView getSpecColorAOV()       const = 0;
    virtual VkImage     getSpecColorAOVImage()  const = 0;
```

In `RTProfileRendererBase`, after the 3.C delegators, add:

```cpp
    VkImageView getDiffAlbedoAOV()      const override { return m_pathTracer.getDiffAlbedoAOV(); }
    VkImage     getDiffAlbedoAOVImage() const override { return m_pathTracer.getDiffAlbedoAOVImage(); }
    VkImageView getSpecColorAOV()       const override { return m_pathTracer.getSpecColorAOV(); }
    VkImage     getSpecColorAOVImage()  const override { return m_pathTracer.getSpecColorAOVImage(); }
```

### Step 3.2: VulkanRenderer accessors

Edit `ohao/gpu/vulkan/renderer.hpp`. After the 3.C `getSpecularRadianceAOVImage` decl, add:

```cpp
    VkImageView getDiffAlbedoAOV()      const;
    VkImage     getDiffAlbedoAOVImage() const;
    VkImageView getSpecColorAOV()       const;
    VkImage     getSpecColorAOVImage()  const;
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Mirror the 3.C split idiom. After `getSpecularRadianceAOV()`:

```cpp
VkImageView VulkanRenderer::getDiffAlbedoAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getDiffAlbedoAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getSpecColorAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getSpecColorAOV();
    }
    return VK_NULL_HANDLE;
}
```

After `getSpecularRadianceAOVImage()`:

```cpp
VkImage VulkanRenderer::getDiffAlbedoAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDiffAlbedoAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDiffAlbedoAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getSpecColorAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getSpecColorAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getSpecColorAOVImage();
    }
    return VK_NULL_HANDLE;
}
```

### Step 3.3: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

### Step 3.4: Commit

```bash
git add ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp \
        ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): expose diff-albedo + spec-color AOVs via VulkanRenderer

IRTRendererProfile gains 4 pure virtuals (VkImageView + VkImage getters
for both AOVs). RTProfileRendererBase delegates to m_pathTracer.
VulkanRenderer dispatches to the active profile.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Shader — binding decls + zero-init + capture imageStore + replace demod block with raw writes

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen`
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 4.1: Declare bindings 24 + 25 in `pt_raygen.rgen`

Right after the `binding = 23, rgba32f ... specularRadiance;` line (from 3.C.5), add:

```glsl
layout(set = 0, binding = 24, rgba8) uniform image2D diffuseAlbedoAOV;
layout(set = 0, binding = 25, rgba8) uniform image2D specColorAOV;
```

### Step 4.2: Zero-init the new AOVs at pixel entry

Find the top of `main()` where `diffContrib` / `specContrib` / `firstHitDiffAlbedo` / `firstHitSpecColor` / `lobeMask` are declared (the Sub-plan 3.C comment block around line 118). Immediately AFTER those local declarations, add:

```glsl
    // Sub-plan 3.C.6: zero-init demod AOVs; bounce-0 first-hit overwrites on hit.
    imageStore(diffuseAlbedoAOV, pixel, vec4(0.0));
    imageStore(specColorAOV,     pixel, vec4(0.0));
```

This ensures miss-at-bounce-0 pixels retain zero in both AOVs.

### Step 4.3: Add capture imageStore at bounce 0 in `pt_raygen.rgen`

Find the existing bounce-0 F0 capture block (search `Sub-plan 3.C: capture first-hit demodulation factors`). It currently reads:

```glsl
        // Sub-plan 3.C: capture first-hit demodulation factors
        if (bounce == 0u) {
            firstHitDiffAlbedo = isMetal ? vec3(0.0) : albedo;
            firstHitSpecColor  = F0;
        }
```

REPLACE with (adds 2 imageStore calls inside the same `if`):

```glsl
        // Sub-plan 3.C: capture first-hit demodulation factors
        // Sub-plan 3.C.6: also expose them as AOVs for NRD remodulate
        if (bounce == 0u) {
            firstHitDiffAlbedo = isMetal ? vec3(0.0) : albedo;
            firstHitSpecColor  = F0;
            imageStore(diffuseAlbedoAOV, pixel, vec4(firstHitDiffAlbedo, 1.0));
            imageStore(specColorAOV,     pixel, vec4(firstHitSpecColor,  1.0));
        }
```

### Step 4.4: Replace demod block with raw writes at raygen exit in `pt_raygen.rgen`

Find the existing Sub-plan 3.C demod block (search `Sub-plan 3.C: demodulate + write diffuse/specular radiance AOVs`). It currently reads:

```glsl
    // Sub-plan 3.C: demodulate + write diffuse/specular radiance AOVs
    vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
    vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
    imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
    imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

REPLACE with:

```glsl
    // Sub-plan 3.C.6: write RAW radiance; NRD remodulates using bindings 24/25.
    imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));
    imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
```

### Step 4.5: Mirror 4.1-4.4 edits to `pt_raygen_offline.rgen`

`pt_raygen_offline.rgen` is a verbatim copy of pt_raygen.rgen except for the top comment. Apply the same 4 edits.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only the top comment block differs.

### Step 4.6: Mirror 4.1-4.4 edits to `pt_raygen_realtime.rgen`

Apply the same 4 edits at the analogous sites. Realtime's structure is parallel — the same landmarks (binding decls after 23, local declarations block, bounce-0 F0 capture, raygen exit demod block) all exist.

### Step 4.7: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile (`rgba8` is a valid storage-image qualifier on Vulkan 1.3). Clean full-app link.

### Step 4.8: Regression smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t4_3c6_cornell.png 16 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4_3c6_helmet.png 16 --denoise=none 2>&1 | tail -3
```

Expected: both produce images. Beauty should match pre-3.C.6 (only AOV semantics change — `radiance` accumulation math is unchanged).

### Step 4.9: Commit

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): raw radiance + demod AOVs at bindings 24+25 (Sub-plan 3.C.6)

All three raygen shaders declare bindings 24 (rgba8 diffuseAlbedoAOV)
+ 25 (rgba8 specColorAOV), zero-init both at pixel entry, and write
firstHitDiffAlbedo + firstHitSpecColor on bounce 0 hits.

The Sub-plan 3.C demod division at raygen exit is removed — bindings
22 + 23 now hold RAW diffContrib + specContrib radiance. Downstream
NRD (Sub-plan 4) will remodulate via:
    beauty ≈ denoisedDiff × diffAlbedo + denoisedSpec × specColor

Beauty path unchanged (radiance accumulation unmodified).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Readback helpers + CLI dumps + verification

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 5.1: Add readback helpers to VulkanRenderer

Edit `ohao/gpu/vulkan/renderer.hpp`. After the 3.C.5 `readbackSpecularRadiance` decl, add:

```cpp
    // Debug: readback RGBA8 diffuse albedo AOV (4 bytes per pixel).
    bool readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA8 specular color AOV (4 bytes per pixel).
    bool readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Find the 3.C.5 `readbackSpecularRadiance` impl — mirror the same staging-buffer pattern. After it, add:

```cpp
bool VulkanRenderer::readbackDiffAlbedoAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getDiffAlbedoAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

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
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackSpecColorAOV(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getSpecColorAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

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
    std::memcpy(data.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}
```

### Step 5.2: Add CLI flags + dump blocks to env_demo.cpp

Edit `examples/env_demo.cpp`. In the existing CLI parse loop (search `dump-specular`), add two more branches:

```cpp
    std::string dumpDiffAlbedoPath;
    std::string dumpSpecColorPath;
    // ... inside the existing for loop, add: ...
        else if (arg.rfind("--dump-diff-albedo=", 0) == 0) {
            dumpDiffAlbedoPath = arg.substr(19);
        } else if (arg.rfind("--dump-spec-color=", 0) == 0) {
            dumpSpecColorPath = arg.substr(18);
        }
```

After the existing `--dump-specular` block, add two new blocks. Since RGBA8 data is already in `[0, 255]` per channel, no tonemap / decode needed — just extract RGB (drop alpha) and write PNG:

```cpp
    auto dumpRGBA8ToRGB = [&](const std::string& path, const std::vector<uint8_t>& rgba,
                               uint32_t w, uint32_t h) {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        for (uint32_t i = 0; i < w * h; i++) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
        stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
        std::cout << "Saved " << path << std::endl;
    };

    if (!dumpDiffAlbedoPath.empty()) {
        std::vector<uint8_t> rgba;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDiffAlbedoAOV(rgba, dw, dh)) {
            std::cerr << "[Diff albedo dump] readback failed\n";
        } else {
            dumpRGBA8ToRGB(dumpDiffAlbedoPath, rgba, dw, dh);
        }
    }

    if (!dumpSpecColorPath.empty()) {
        std::vector<uint8_t> rgba;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackSpecColorAOV(rgba, sw, sh)) {
            std::cerr << "[Spec color dump] readback failed\n";
        } else {
            dumpRGBA8ToRGB(dumpSpecColorPath, rgba, sw, sh);
        }
    }
```

Place the `dumpRGBA8ToRGB` lambda near the top of main() alongside the existing `half2float` + `dumpRGBA32FStream` helpers.

### Step 5.3: Build + smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-3c6-demod-aovs
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

### Step 5.4: Helmet scene full dump

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_3c6_helmet.png 64 --denoise=none \
    --dump-diffuse=/tmp/t5_3c6_diffuse.png \
    --dump-specular=/tmp/t5_3c6_specular.png \
    --dump-diff-albedo=/tmp/t5_3c6_diff_albedo.png \
    --dump-spec-color=/tmp/t5_3c6_spec_color.png 2>&1 | tail -8
```

Expected stdout:
```
Saved /tmp/t5_3c6_diffuse.png (max channel = X.X)
Saved /tmp/t5_3c6_specular.png (max channel = Y.Y)
Saved /tmp/t5_3c6_diff_albedo.png
Saved /tmp/t5_3c6_spec_color.png
```

Record the X.X / Y.Y max channel values — they should be SIGNIFICANTLY LOWER than 3.C.5's (no demod amplification).

### Step 5.5: Visual spot-check via Read tool

Read all 4 PNGs:

- `/tmp/t5_3c6_diff_albedo.png`: matte plates show the helmet's base color; visor + metal body regions are **near-black** (metals have zero diffuse albedo); sky = black (miss zero-init).
- `/tmp/t5_3c6_spec_color.png`: matte plates near-black (0.04 dielectric F0 ≈ 10/255); metal visor shows metal albedo; sky = black.
- `/tmp/t5_3c6_diffuse.png`: helmet emissive panels visible; matte plates with ambient grain. SIGNIFICANTLY DIMMER than 3.C.5 (no 100× amplification on dark albedo channels).
- `/tmp/t5_3c6_specular.png`: visor env reflection visible. Also dimmer than 3.C.5 (no amplification).

If any AOV is uniformly black/white or mangled, investigate.

Copy to renders/ (check gitignore first):
```bash
mkdir -p renders
cp /tmp/t5_3c6_diff_albedo.png  renders/diff_albedo_helmet.png
cp /tmp/t5_3c6_spec_color.png   renders/spec_color_helmet.png
cp /tmp/t5_3c6_diffuse.png      renders/diffuse_helmet_raw.png
cp /tmp/t5_3c6_specular.png     renders/specular_helmet_raw.png
```

Don't `git add` the PNGs (renders/ is gitignored).

### Step 5.6: Regression smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t5_3c6_cornell.png 16 --denoise=none 2>&1 | tail -3
```

Expected: beauty visually identical to pre-3.C.6 (only AOV semantics changed; `radiance` accumulation math is unchanged).

### Step 5.7: Append verification_log.md entry

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-19: Demod AOV exposure (Sub-plan 3.C.6)

Raygen stops pre-dividing radiance by albedo/F0 at exit. Two new AOVs
expose demod factors so NRD remodulates downstream:
- **Binding 24 (diffuse albedo)**: RGBA8 UNORM. `isMetal ? 0 : albedo`.
- **Binding 25 (specular color)**: RGBA8 UNORM. `isMetal ? albedo : vec3(0.04)`.

Bindings 22+23 now hold RAW `diffContrib` / `specContrib` radiance
(not demodulated). Removes the 100×-amplification-on-dark-channels
bug in the 3.C demod division.

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Diff albedo dump:** matte plates show helmet base color; metal
  regions near-black; sky black.
- **Spec color dump:** matte plates near-black (~10/255, dielectric F0);
  metal regions show metal albedo; sky black.
- **Diffuse radiance (raw):** max channel <fill from stdout>. Dimmer
  than 3.C.5 (was 93.2568; now significantly lower due to no amplification).
- **Specular radiance (raw):** max channel <fill from stdout>. Similarly
  dimmer than 3.C.5 (was 34076.5).
- **Regression:** beauty output unchanged (AOV semantics only —
  `radiance` accumulation is not demodulated).

Memory: +8 MB at 1080p for 2 new RGBA8 AOVs. Trivial.

Sub-plan 3.C.6 complete. Next: 3.C.7 (dual-ray bounce-0 split — first
visible offline quality improvement: cleaner specular at low spp).
```

Fill `<fill from stdout>` with actual max channel values from Step 5.4.

### Step 5.8: Commit

```bash
git add ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): --dump-diff-albedo + --dump-spec-color + visual verify (3.C.6)

Adds readbackDiffAlbedoAOV + readbackSpecColorAOV using the same
staging-buffer pattern as existing readbacks. env_demo grows
--dump-diff-albedo=<path> and --dump-spec-color=<path> flags
(RGBA8 direct passthrough to PNG; no tonemap/decode).

Verified on helmet + env_studio: diff_albedo shows base color on
matte, black on metal. spec_color shows near-black (0.04) on matte,
metal albedo on visor. Raw diffuse/specular radiance dumps now
dimmer than 3.C.5 (no 100× demod amplification).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 storage images + 4 getters | Task 1 |
| §4.2 descriptor bindings + pool + writes + layout barriers | Task 2 |
| §4.3 GLSL binding decls + zero-init + bounce-0 capture imageStore + remove demod division | Task 4 |
| §4.4 env_demo CLI flags + readback helpers | Task 5 |
| §4.5 accessor chain | Task 3 |
| §6 visual verification (diff_albedo / spec_color / raw-diff / raw-spec) + regression | Task 5 |
| §9 success criteria (all 10 items) | Tasks 1-5 collectively |

**Placeholder scan:** only `<fill from stdout>` + `X.X` / `Y.Y` in verification log template — intentional, filled at execution time.

**Type consistency:**
- `m_diffAlbedoImage/Memory/View` + `m_specColorImage/Memory/View` used consistently across Tasks 1, 2, 3, 5.
- `getDiffAlbedoAOV` / `getDiffAlbedoAOVImage` / `getSpecColorAOV` / `getSpecColorAOVImage` consistent across 3 layers.
- Bindings 24 (diffuse albedo) + 25 (specular color) consistent in Task 2 (descriptor) + Task 4 (shader decl).
- GLSL format `rgba8` ↔ `VK_FORMAT_R8G8B8A8_UNORM` match.
- Shader variable names `firstHitDiffAlbedo` / `firstHitSpecColor` reused from 3.C — unchanged.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-19-denoiser-subplan3c6-demod-aov-exposure.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Matches the pattern that shipped 3.A + 3.B + 3.C + 3.C.5 cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
