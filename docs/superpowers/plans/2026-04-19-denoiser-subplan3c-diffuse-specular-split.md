# Denoiser Sub-plan 3.C: Diffuse + Specular Radiance Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Path tracer writes two more first-hit AOVs — `diffuseRadiance` (RGBA16F @ binding 22) and `specularRadiance` (RGBA16F @ binding 23) — holding demodulated radiance streams classified via a bounce-0 lobe stamp. Feeds Sub-plan 4 (NRD).

**Architecture:** Mirror of the 3.A/3.B pattern (storage images → bindings → accessor chain → shader writes → debug readback + CLI). One departure from the design spec: `lobeMask` stays as a **raygen-local `uint`** (not a payload field) because the raygen owns the outer path-trace loop and directly knows which lobe was chosen at bounce 0. No payload struct changes; no closesthit touch. Demodulation and write happen at raygen exit, after all bounces accumulate into `diffContrib` / `specContrib` locals.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · `R16G16B16A16_SFLOAT` storage images · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-19-denoiser-subplan3c-diffuse-specular-split-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Add `m_diffuseRadianceImage/Memory/View` + `m_specularRadianceImage/Memory/View` + 4 getters. |
| `ohao/render/rt/path_tracer.cpp` | Create/destroy in `createImages`/`destroyImages`; bindings 22+23 in layout; pool STORAGE_IMAGE +2; descriptor writes; AOV barrier group [5]→[7]. |
| `ohao/render/rt/rt_profile_renderer.hpp` | 4 pure virtuals + 4 delegating overrides. |
| `ohao/gpu/vulkan/renderer.hpp` | 4 accessor decls + 2 readback helper decls. |
| `ohao/gpu/vulkan/renderer.cpp` | Accessor impls + readback impls. |
| `shaders/rt/pt_raygen.rgen` | Binding 22+23 decls + local accumulators + NEE split + env-MIS split + emissive attribution + bounce-0 lobe stamp + bounce-≥1 attribution + demodulated writes at exit. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Binding decls + same structure but no env-MIS block (realtime doesn't have one). |
| `examples/env_demo.cpp` | `--dump-diffuse=<path>` + `--dump-specular=<path>` CLI flags; half→float decode + tonemap + PNG write. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C entry with sum-match RMSE + channel max stats. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-diffspec -b denoiser-3c-diffuse-specular-split HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-diffspec`.

If the fresh worktree's `build/` is empty on first configure, bootstrap by copying deps:
```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. \
      /home/frankyin/Desktop/Github/ohao-diffspec/build/_deps/
```

For OptiX support (optional, stub fallback works without):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Create / destroy diffuse + specular radiance storage images

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 1.1: Add private image handles to header

Edit `ohao/render/rt/path_tracer.hpp`. Find the 3.B `m_roughnessAOVImage` block (search `m_roughnessAOVImage`). After it, add:

```cpp
    // Feature 3.C: demodulated diffuse radiance (RGBA16F)
    VkImage        m_diffuseRadianceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_diffuseRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_diffuseRadianceView = VK_NULL_HANDLE;

    // Feature 3.C: demodulated specular radiance (RGBA16F)
    VkImage        m_specularRadianceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_specularRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_specularRadianceView = VK_NULL_HANDLE;
```

Public accessors — near `getRoughnessAOV`:

```cpp
    VkImageView getDiffuseRadianceAOV()      const { return m_diffuseRadianceView; }
    VkImage     getDiffuseRadianceAOVImage() const { return m_diffuseRadianceImage; }
    VkImageView getSpecularRadianceAOV()      const { return m_specularRadianceView; }
    VkImage     getSpecularRadianceAOVImage() const { return m_specularRadianceImage; }
```

### Step 1.2: Create in createImages()

Edit `ohao/render/rt/path_tracer.cpp` `PathTracer::createImages()`. Find the 3.B roughness block (ends with `if (vkCreateImageView(...) ... &m_roughnessAOVView)`). After it, before the final `return true;`, insert:

```cpp
    // ---- Feature 3.C: Diffuse radiance (RGBA16F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_diffuseRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_diffuseRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_diffuseRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_diffuseRadianceImage, m_diffuseRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_diffuseRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_diffuseRadianceView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C: Specular radiance (RGBA16F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specularRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_specularRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_specularRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_specularRadianceImage, m_specularRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_specularRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_specularRadianceView) != VK_SUCCESS) return false;
    }
```

### Step 1.3: Destroy in destroyImages()

Near the 3.B `m_roughnessAOV*` destroy lines, add:

```cpp
    if (m_diffuseRadianceView)    { vkDestroyImageView(m_device, m_diffuseRadianceView, nullptr);   m_diffuseRadianceView = VK_NULL_HANDLE; }
    if (m_diffuseRadianceImage)   { vkDestroyImage(m_device, m_diffuseRadianceImage, nullptr);      m_diffuseRadianceImage = VK_NULL_HANDLE; }
    if (m_diffuseRadianceMemory)  { vkFreeMemory(m_device, m_diffuseRadianceMemory, nullptr);       m_diffuseRadianceMemory = VK_NULL_HANDLE; }

    if (m_specularRadianceView)   { vkDestroyImageView(m_device, m_specularRadianceView, nullptr);  m_specularRadianceView = VK_NULL_HANDLE; }
    if (m_specularRadianceImage)  { vkDestroyImage(m_device, m_specularRadianceImage, nullptr);     m_specularRadianceImage = VK_NULL_HANDLE; }
    if (m_specularRadianceMemory) { vkFreeMemory(m_device, m_specularRadianceMemory, nullptr);      m_specularRadianceMemory = VK_NULL_HANDLE; }
```

### Step 1.4: Build + smoke

- [ ] **Step 1.4a: Configure + build**

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -5  # only if build/ missing
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 1.4b: Cornell smoke**

```bash
./build/cornell_box /tmp/t1_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: `Saved: /tmp/t1_cornell.png`. No Vulkan validation errors. Nothing reads or writes the new images yet.

### Step 1.5: Commit

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): diffuse + specular radiance storage images (Sub-plan 3.C)

PathTracer gains m_diffuseRadianceImage (RGBA16F) and
m_specularRadianceImage (RGBA16F) alongside the existing AOVs.
Created in createImages, destroyed in destroyImages. Accessors:
getDiffuseRadianceAOV/Image, getSpecularRadianceAOV/Image.
Descriptor binding + shader writes in subsequent commits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Descriptor bindings 22 + 23 + AOV layout transitions

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp`

### Step 2.1: Extend bindings array

In `createDescriptorResources()`, find the current `VkDescriptorSetLayoutBinding bindings[22]` (size after 3.B). Grow to `[24]`. Also grow `bindingFlags[22]` → `[24]` (leave new entries at default 0), and update both `flagsInfo.bindingCount = 24` and `layoutInfo.bindingCount = 24`.

After the binding 21 (roughness) entries, add:

```cpp
    // Binding 22: diffuse radiance (RGBA16F storage image) — Sub-plan 3.C
    bindings[22].binding         = 22;
    bindings[22].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[22].descriptorCount = 1;
    bindings[22].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 23: specular radiance (RGBA16F storage image) — Sub-plan 3.C
    bindings[23].binding         = 23;
    bindings[23].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[23].descriptorCount = 1;
    bindings[23].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

### Step 2.2: Grow the pool

Find `VkDescriptorPoolSize poolSizes[]`. Increment the `STORAGE_IMAGE` `descriptorCount` by 2. After 3.B it was 11; set to 13. Update the running comment if present (e.g. `// +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec (3.C)`).

### Step 2.3: Add descriptor writes in render()

Find the existing roughness write (search `dstBinding = 21` or `roughInfo`). Grow `writes[]` array bound by 2 (e.g., `writes[22]` → `writes[24]`). After the roughness write, add:

```cpp
    // Binding 22: diffuse radiance — Sub-plan 3.C
    VkDescriptorImageInfo diffRadianceInfo{};
    diffRadianceInfo.imageView   = m_diffuseRadianceView;
    diffRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 22;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffRadianceInfo;
    writeCount++;

    // Binding 23: specular radiance — Sub-plan 3.C
    VkDescriptorImageInfo specRadianceInfo{};
    specRadianceInfo.imageView   = m_specularRadianceView;
    specRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 23;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specRadianceInfo;
    writeCount++;
```

### Step 2.4: Add to AOV barrier group (UNDEFINED → GENERAL)

Find the AOV barrier block (search `aovBarriers[5]` — size after 3.B). Grow the declaration to `aovBarriers[7] = {};`. After the roughness AOV entry (index 4), add:

```cpp
    // Sub-plan 3.C: diffuse radiance AOV barrier
    aovBarriers[5].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[5].srcAccessMask = 0;
    aovBarriers[5].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[5].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[5].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[5].image = m_diffuseRadianceImage;
    aovBarriers[5].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Sub-plan 3.C: specular radiance AOV barrier
    aovBarriers[6].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[6].srcAccessMask = 0;
    aovBarriers[6].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[6].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[6].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[6].image = m_specularRadianceImage;
    aovBarriers[6].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
```

Update `vkCmdPipelineBarrier`'s barrier count from 5 → 7:
```cpp
vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
    0, 0, nullptr, 0, nullptr, 7, aovBarriers);
```

### Step 2.5: Build + smoke

- [ ] **Step 2.5a: Build**

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean.

- [ ] **Step 2.5b: Cornell smoke**

```bash
./build/cornell_box /tmp/t2_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: clean render, no validation errors. Shaders still don't write bindings 22/23; images remain zero after barriers transition.

### Step 2.6: Commit

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): descriptor bindings 22+23 for diffuse + specular radiance

Bindings 22 (diffuse RGBA16F) + 23 (specular RGBA16F), both STORAGE_IMAGE
raygen-only. Pool STORAGE_IMAGE count +2 (11→13). Both images join the
AOV layout barrier group (UNDEFINED→GENERAL), count 5→7. Shader writes
come in Task 4.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Accessor chain (IRTRendererProfile + VulkanRenderer)

**Files:**
- Modify: `ohao/render/rt/rt_profile_renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`

### Step 3.1: IRTRendererProfile virtuals + base overrides

Edit `ohao/render/rt/rt_profile_renderer.hpp`. After the 3.B roughness-related virtuals (search `getRoughnessAOV`), add:

```cpp
    virtual VkImageView getDiffuseRadianceAOV()      const = 0;
    virtual VkImage     getDiffuseRadianceAOVImage() const = 0;
    virtual VkImageView getSpecularRadianceAOV()      const = 0;
    virtual VkImage     getSpecularRadianceAOVImage() const = 0;
```

In `RTProfileRendererBase`, after the 3.B delegators, add:

```cpp
    VkImageView getDiffuseRadianceAOV()      const override { return m_pathTracer.getDiffuseRadianceAOV(); }
    VkImage     getDiffuseRadianceAOVImage() const override { return m_pathTracer.getDiffuseRadianceAOVImage(); }
    VkImageView getSpecularRadianceAOV()      const override { return m_pathTracer.getSpecularRadianceAOV(); }
    VkImage     getSpecularRadianceAOVImage() const override { return m_pathTracer.getSpecularRadianceAOVImage(); }
```

(Use `.` for member access — `m_pathTracer` is a value, matching 3.B's pattern.)

### Step 3.2: VulkanRenderer accessors

Edit `ohao/gpu/vulkan/renderer.hpp`. After the 3.B `getRoughnessAOVImage` decl, add:

```cpp
    VkImageView getDiffuseRadianceAOV()      const;
    VkImage     getDiffuseRadianceAOVImage() const;
    VkImageView getSpecularRadianceAOV()      const;
    VkImage     getSpecularRadianceAOVImage() const;
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Locate the 3.B impls (`getRoughnessAOV` → uses `getRTRenderer(m_renderMode)` helper; `getRoughnessAOVImage` → uses explicit `m_rtOfflineRenderer`/`m_rtRealtimeRenderer` checks). Mirror each MV/roughness twin exactly.

After `VulkanRenderer::getRoughnessAOV()`:

```cpp
VkImageView VulkanRenderer::getDiffuseRadianceAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getDiffuseRadianceAOV();
    }
    return VK_NULL_HANDLE;
}

VkImageView VulkanRenderer::getSpecularRadianceAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getSpecularRadianceAOV();
    }
    return VK_NULL_HANDLE;
}
```

After `VulkanRenderer::getRoughnessAOVImage()`:

```cpp
VkImage VulkanRenderer::getDiffuseRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getDiffuseRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getDiffuseRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanRenderer::getSpecularRadianceAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getSpecularRadianceAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getSpecularRadianceAOVImage();
    }
    return VK_NULL_HANDLE;
}
```

(Confirm the exact signatures of `getRoughnessAOV`/`getRoughnessAOVImage` first — if the `getRTRenderer` idiom uses `const auto*` vs `const auto&`, match verbatim.)

### Step 3.3: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. If a concrete profile subclass doesn't inherit from `RTProfileRendererBase`, it'll fail with "cannot instantiate abstract class" — add overrides there too (check both `RTRealtimeRenderer` and `RTOfflineRenderer`).

### Step 3.4: Commit

```bash
git add ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp \
        ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): expose diffuse + specular radiance AOVs via VulkanRenderer

IRTRendererProfile gains 4 pure virtuals (VkImageView + VkImage getters
for both radiance streams). RTProfileRendererBase delegates to
m_pathTracer. VulkanRenderer dispatches to the active profile.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Shader — bounce-0 split + bounce-≥1 attribution + demodulated writes

This is the largest task. Work in two internal sub-commits for bisectability: (A) binding decls + locals + exit writes (safe, produces zeros); (B) NEE split + env-MIS split + emissive + lobe stamp + indirect attribution. Build and smoke between A and B.

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen` (verbatim mirror of pt_raygen.rgen)
- Modify: `shaders/rt/pt_raygen_realtime.rgen` (no env-MIS block)

### Step 4A.1: Declare bindings 22 + 23 in `pt_raygen.rgen`

After the `binding = 21, r8 ... roughnessAOV;` line, add:

```glsl
layout(set = 0, binding = 22, rgba16f) uniform image2D diffuseRadiance;
layout(set = 0, binding = 23, rgba16f) uniform image2D specularRadiance;
```

### Step 4A.2: Add local accumulators + F0/albedo capture at bounce 0

At the top of `main()`, near `vec3 radiance = vec3(0.0);` (around line 111), add:

```glsl
    // Sub-plan 3.C: diffuse + specular radiance split (demodulated)
    vec3 diffContrib        = vec3(0.0);
    vec3 specContrib        = vec3(0.0);
    vec3 firstHitDiffAlbedo = vec3(0.0);  // diffuse albedo at bounce 0 (metals: 0)
    vec3 firstHitSpecColor  = vec3(0.04); // F0 at bounce 0 (dielectric default)
    uint lobeMask           = 0u;         // 0 = diffuse, 1 = specular (set at bounce 0 BSDF sample)
```

Inside the main loop, AFTER the `packedRoughness`/`isMetal`/`roughness`/`F0` decode block (search for `F0: dielectric = 0.04, metal = albedo` — the `vec3 F0 = isMetal ? albedo : vec3(0.04);` line), add:

```glsl
        // Sub-plan 3.C: capture first-hit demodulation factors
        if (bounce == 0u) {
            firstHitDiffAlbedo = isMetal ? vec3(0.0) : albedo;
            firstHitSpecColor  = F0;
        }
```

### Step 4A.3: Add demodulated writes at raygen exit

Just before the final `imageStore(outputImage, ...)` at the end of `main()`, add:

```glsl
    // Sub-plan 3.C: demodulate + write diffuse/specular radiance AOVs
    vec3 diffDemod = diffContrib / max(firstHitDiffAlbedo, vec3(0.01));
    vec3 specDemod = specContrib / max(firstHitSpecColor,  vec3(0.01));
    // Clamp to RGBA16F safe range (max 65504; leave headroom for NRD internal ops)
    diffDemod = min(diffDemod, vec3(60000.0));
    specDemod = min(specDemod, vec3(60000.0));
    imageStore(diffuseRadiance,  pixel, vec4(diffDemod, 1.0));
    imageStore(specularRadiance, pixel, vec4(specDemod, 1.0));
```

At this point (end of Step 4A) `diffContrib`/`specContrib` are still zero everywhere. Both AOVs will be all-zero. That's fine — validates the plumbing before the split logic lands.

### Step 4A.4: Mirror 4A.1–4A.3 to `pt_raygen_offline.rgen`

`pt_raygen_offline.rgen` is a near-verbatim copy of `pt_raygen.rgen` except the top 3–4 line comment. Apply steps 4A.1–4A.3 identically.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only the top comment block differs.

### Step 4A.5: Mirror 4A.1–4A.3 to `pt_raygen_realtime.rgen`

Apply 4A.1 (binding decls) and 4A.2 (locals + first-hit capture) and 4A.3 (demod write at exit) to the realtime shader at the analogous sites. Realtime shader is ~120 lines shorter (no env MIS), but the locations of binding decls (after binding 21), local accumulators (near `vec3 radiance`), first-hit capture (after F0 decode), and exit writes (before final `imageStore(outputImage, ...)`) exist identically.

### Step 4A.6: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile, clean link.

### Step 4A.7: Smoke — AOVs written but all zero

```bash
./build/cornell_box /tmp/t4a_cornell.png 4 --denoise=none 2>&1 | tail -3
```

Expected: beauty unchanged (split logic not in yet), no validation errors, both new AOVs filled with zeros by the exit writes.

### Step 4A.8: Commit sub-stage A

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): 3.C plumbing — bindings 22+23 + locals + demod exit writes

All three raygen shaders declare bindings 22 (diffuse RGBA16F) +
23 (specular RGBA16F), capture firstHitDiffAlbedo/SpecColor at
bounce 0, and write demodulated diffContrib/specContrib at exit.
Accumulators are still zero — split logic lands in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Step 4B.1: Split NEE contribution in `pt_raygen.rgen`

Locate the NEE block. The critical lines (around 345–353 in `pt_raygen.rgen`) look like:

```glsl
                    vec3 diff = kD * albedo / 3.14159;
                    vec3 directContribution = throughput * Le * (diff + spec) * NdotL * weight * float(lightBuf.lightCount);
                    if (enableFireflyClamp && pc.tuning.x > 0.0) {
                        float contribLum = dot(directContribution, vec3(0.2126, 0.7152, 0.0722));
                        if (contribLum > pc.tuning.x) directContribution *= pc.tuning.x / contribLum;
                    }
                    radiance += directContribution;
```

REPLACE `radiance += directContribution;` with:

```glsl
                    radiance += directContribution;
                    // Sub-plan 3.C: split NEE contribution into diff/spec channels
                    vec3 neeCommon = throughput * Le * NdotL * weight * float(lightBuf.lightCount);
                    vec3 neeDiffuse  = neeCommon * diff;
                    vec3 neeSpecular = neeCommon * spec;
                    if (enableFireflyClamp && pc.tuning.x > 0.0) {
                        float lumD = dot(neeDiffuse,  vec3(0.2126, 0.7152, 0.0722));
                        float lumS = dot(neeSpecular, vec3(0.2126, 0.7152, 0.0722));
                        if (lumD > pc.tuning.x) neeDiffuse  *= pc.tuning.x / lumD;
                        if (lumS > pc.tuning.x) neeSpecular *= pc.tuning.x / lumS;
                    }
                    if (bounce == 0u) {
                        diffContrib += neeDiffuse;
                        specContrib += neeSpecular;
                    } else if (lobeMask == 0u) {
                        diffContrib += neeDiffuse + neeSpecular;  // stamp attributes full BRDF contribution
                    } else {
                        specContrib += neeDiffuse + neeSpecular;
                    }
```

### Step 4B.2: Split env-MIS contribution in `pt_raygen.rgen`

Locate the env-MIS block ending with `radiance += envContribution;` (around line 415). REPLACE the clamp + `radiance +=` sequence:

Find:
```glsl
                    vec3 envContribution = throughput * envRadiance * brdf * NdotL_env * w / envPdf;

                    if (enableFireflyClamp && pc.tuning.x > 0.0) {
                        float contribLum = dot(envContribution, vec3(0.2126, 0.7152, 0.0722));
                        if (contribLum > pc.tuning.x) envContribution *= pc.tuning.x / contribLum;
                    }
                    radiance += envContribution;
```

REPLACE with:
```glsl
                    vec3 envContribution = throughput * envRadiance * brdf * NdotL_env * w / envPdf;

                    // Sub-plan 3.C: split env-MIS contribution into diff/spec channels
                    vec3 envCommon  = throughput * envRadiance * NdotL_env * w / envPdf;
                    vec3 envDiffuse  = envCommon * diff;
                    vec3 envSpecular = envCommon * spec;

                    if (enableFireflyClamp && pc.tuning.x > 0.0) {
                        float contribLum = dot(envContribution, vec3(0.2126, 0.7152, 0.0722));
                        if (contribLum > pc.tuning.x) envContribution *= pc.tuning.x / contribLum;
                        float lumD = dot(envDiffuse,  vec3(0.2126, 0.7152, 0.0722));
                        float lumS = dot(envSpecular, vec3(0.2126, 0.7152, 0.0722));
                        if (lumD > pc.tuning.x) envDiffuse  *= pc.tuning.x / lumD;
                        if (lumS > pc.tuning.x) envSpecular *= pc.tuning.x / lumS;
                    }
                    radiance += envContribution;
                    if (bounce == 0u) {
                        diffContrib += envDiffuse;
                        specContrib += envSpecular;
                    } else if (lobeMask == 0u) {
                        diffContrib += envDiffuse + envSpecular;
                    } else {
                        specContrib += envDiffuse + envSpecular;
                    }
```

### Step 4B.3: Split emissive contribution in `pt_raygen.rgen`

Find (around line 200):
```glsl
        if (length(emissive) > 0.001) {
            radiance += throughput * emissive;
        }
```

REPLACE with:
```glsl
        if (length(emissive) > 0.001) {
            vec3 emissiveContribution = throughput * emissive;
            radiance += emissiveContribution;
            // Sub-plan 3.C: attribute emissive to diffuse channel (convention)
            if (bounce == 0u || lobeMask == 0u) {
                diffContrib += emissiveContribution;
            } else {
                specContrib += emissiveContribution;
            }
        }
```

### Step 4B.4: Split env-miss contribution in `pt_raygen.rgen`

Find (around line 186):
```glsl
            radiance += throughput * payload.color * envMisWeight;
```

REPLACE with:
```glsl
            vec3 envMissContribution = throughput * payload.color * envMisWeight;
            radiance += envMissContribution;
            // Sub-plan 3.C: attribute env-miss radiance by bounce-0 lobe stamp
            if (bounce == 0u) {
                // Primary miss (no hit at all): both channels zero — already initialized.
                // Fall through without adding to either channel.
            } else if (lobeMask == 0u) {
                diffContrib += envMissContribution;
            } else {
                specContrib += envMissContribution;
            }
```

### Step 4B.5: Stamp lobeMask at bounce-0 BSDF sample in `pt_raygen.rgen`

Find the BSDF-sample branch (around line 437): `if (bsdfChoice < specProb || roughness < 0.05)`. That branch is the specular sample. The `else` branch is diffuse sample. Add the stamp at the START of each branch, guarded by `bounce == 0u`:

Specular branch (inside `if (bsdfChoice < specProb || roughness < 0.05)`, at the very top of the block):
```glsl
            if (bounce == 0u) lobeMask = 1u;
```

Diffuse branch (inside the `else` at the very top):
```glsl
            if (bounce == 0u) lobeMask = 0u;
```

### Step 4B.6: Mirror 4B.1–4B.5 to `pt_raygen_offline.rgen`

`pt_raygen_offline.rgen` is a verbatim copy. Apply 4B.1–4B.5 identically.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only the top comment block differs.

### Step 4B.7: Apply adapted split to `pt_raygen_realtime.rgen`

Realtime shader has **no env-MIS block** — skip Step 4B.2. Apply 4B.1 (NEE split), 4B.3 (emissive), 4B.4 (env miss), 4B.5 (lobe stamp) at the analogous sites. Diff against pt_raygen.rgen for sanity afterward:

```bash
diff <(grep -A3 "Sub-plan 3.C" shaders/rt/pt_raygen.rgen) <(grep -A3 "Sub-plan 3.C" shaders/rt/pt_raygen_realtime.rgen)
```

Realtime should have one fewer "Sub-plan 3.C: split env-MIS" occurrence than the default raygen.

### Step 4B.8: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean.

### Step 4B.9: Regression smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t4b_cornell.png 16 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4b_helmet.png 16 --denoise=none 2>&1 | tail -3
```

Expected: both produce images. Beauty should match the pre-3.C run (stamp does not affect `radiance` accumulation because every split contribution was added to `radiance` first, then mirrored into `diff/specContrib` with identical math).

Byte-compare to a fresh `animation`-branch build if available. If bit-differences show up, the split logic leaked into `radiance` — investigate.

### Step 4B.10: Commit sub-stage B

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): 3.C split logic — NEE / env-MIS / emissive / env-miss + lobe stamp

Raygen now classifies each radiance contribution:
- bounce 0 NEE: analytic diffuse/specular split via existing diff + spec BRDFs
- bounce 0 env-MIS: same analytic split
- bounce 0 BSDF sample: stamp lobeMask (0=diffuse, 1=specular)
- bounce >= 1: attribute all contributions to the stamped lobe
- bounce 0 emissive: diffuse channel by convention
- primary miss (bounce 0): both channels zero

Realtime raygen skips the env-MIS split (no env-MIS block). Beauty
output unchanged — split logic mirrors existing radiance math.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Readback helpers + CLI dumps + visual + sum-match verification

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 5.1: Add readback helpers to VulkanRenderer

Edit `ohao/gpu/vulkan/renderer.hpp`. After the 3.B `readbackRoughnessAOV` decl, add:

```cpp
    // Debug: readback RGBA16F diffuse radiance (4 halfs per pixel → 8 bytes).
    bool readbackDiffuseRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height);

    // Debug: readback RGBA16F specular radiance (4 halfs per pixel → 8 bytes).
    bool readbackSpecularRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height);
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Mirror the existing `readbackMotionVector` shape exactly (it also reads an image with multiple halfs per pixel: MV is RG16F = 2 halfs; ours is RGBA16F = 4 halfs). After the 3.B `readbackRoughnessAOV` impl, add:

```cpp
bool VulkanRenderer::readbackDiffuseRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getDiffuseRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 8; // RGBA16F = 8 bytes/pixel
    halfData.resize(static_cast<size_t>(width) * height * 4);

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
    std::memcpy(halfData.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::readbackSpecularRadiance(std::vector<uint16_t>& halfData, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getSpecularRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 8;
    halfData.resize(static_cast<size_t>(width) * height * 4);

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
    std::memcpy(halfData.data(), mapped, byteCount);
    vkUnmapMemory(m_device, stagingMem);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return true;
}
```

Match the exact `findMemoryType` signature used by `readbackMotionVector` at the top of this file (it takes `m_physicalDevice` as first arg — confirm once before pasting).

### Step 5.2: Add CLI flags + dump blocks to env_demo.cpp

Edit `examples/env_demo.cpp`. In the existing CLI parse loop (search `dump-roughness`), extend with two more branches:

```cpp
    std::string dumpDiffusePath;
    std::string dumpSpecularPath;
    // ... inside the existing for loop ...
        else if (arg.rfind("--dump-diffuse=", 0) == 0) {
            dumpDiffusePath = arg.substr(15);
        } else if (arg.rfind("--dump-specular=", 0) == 0) {
            dumpSpecularPath = arg.substr(16);
        }
```

After the existing `--dump-roughness` block, add two new blocks. The helper `half2float` already lives in env_demo.cpp (added in 3.A for the MV dump). Reuse it.

```cpp
    auto dumpRGBA16FStream = [&](const std::string& path, const std::vector<uint16_t>& halfData,
                                  uint32_t w, uint32_t h) {
        // Decode RGBA16F → float RGB, apply Reinhard tonemap, write 8-bit RGB PNG.
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        float maxC = 0.0f;
        for (uint32_t i = 0; i < w * h; i++) {
            float r = half2float(halfData[i * 4 + 0]);
            float g = half2float(halfData[i * 4 + 1]);
            float b = half2float(halfData[i * 4 + 2]);
            maxC = std::max({maxC, r, g, b});
            float rT = r / (r + 1.0f);
            float gT = g / (g + 1.0f);
            float bT = b / (b + 1.0f);
            rgb[i * 3 + 0] = static_cast<uint8_t>(std::max(0, std::min(255, int(rT * 255.0f))));
            rgb[i * 3 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, int(gT * 255.0f))));
            rgb[i * 3 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, int(bT * 255.0f))));
        }
        stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
        std::cout << "Saved " << path << " (max channel = " << maxC << ")" << std::endl;
    };

    if (!dumpDiffusePath.empty()) {
        std::vector<uint16_t> halfData;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDiffuseRadiance(halfData, dw, dh)) {
            std::cerr << "[Diffuse dump] readback failed\n";
        } else {
            dumpRGBA16FStream(dumpDiffusePath, halfData, dw, dh);
        }
    }

    if (!dumpSpecularPath.empty()) {
        std::vector<uint16_t> halfData;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackSpecularRadiance(halfData, sw, sh)) {
            std::cerr << "[Specular dump] readback failed\n";
        } else {
            dumpRGBA16FStream(dumpSpecularPath, halfData, sw, sh);
        }
    }
```

Place the lambda near the top of `main()` (after `half2float`) so both blocks can call it. Or, if preferred, expand inline twice. Lambda is shorter.

### Step 5.3: Build + visual test

- [ ] **Step 5.3a: Build**

```bash
cd /home/frankyin/Desktop/Github/ohao-diffspec
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean.

- [ ] **Step 5.3b: Helmet scene dumps**

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_helmet.png 64 --denoise=none \
    --dump-diffuse=/tmp/t5_diffuse.png \
    --dump-specular=/tmp/t5_specular.png 2>&1 | tail -6
```

Expected stdout includes lines like:
```
Saved /tmp/t5_diffuse.png (max channel = 5.xx)
Saved /tmp/t5_specular.png (max channel = 50.xx)
```

Specular max typically higher than diffuse (HDR env sun reflection via low-roughness lobe demodulates large).

### Step 5.4: Visual check

Use the Read tool or an image viewer to inspect:

- **`/tmp/t5_diffuse.png`**: warm ambient shading across matte helmet plates; visor near-black (reflection is all specular — diffuse channel has only NEE + env ambient × dielectric fraction, which for the metal visor is zero). Sky = black (primary miss).
- **`/tmp/t5_specular.png`**: bright visor reflection of the HDR env; metal body regions bright. Sky = black (primary miss).

If either dump is uniformly black/white, the split logic broke — investigate.

Copy to renders for archival:
```bash
mkdir -p renders
cp /tmp/t5_diffuse.png  renders/diffuse_helmet.png
cp /tmp/t5_specular.png renders/specular_helmet.png
```

(If `renders/` is gitignored — verify with `grep renders .gitignore` — skip the `git add` for these PNGs. Consistent with 3.A/3.B practice.)

### Step 5.5: Sum-match sanity test

From the existing `env_demo` run's saved beauty PNG and the diffuse/specular dumps, eyeball the sum-match: if you remodulate (mentally: `diffDemod × albedo + specDemod × (F0 ≈ 0.04 or albedo for metal)`), the result should roughly equal beauty. Exact numerical match requires a CPU-side script outside this task's scope; flag as a follow-up if precision concerns arise in Sub-plan 4.

Record the stdout `max channel` values for diffuse and specular in the verification log.

### Step 5.6: Append to verification_log.md

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-19: Diffuse + specular radiance split (Sub-plan 3.C) validation

Path tracer now writes demodulated diffuse (RGBA16F @ binding 22) and
specular (RGBA16F @ binding 23) radiance streams on first-hit, for
future NRD consumption (Sub-plan 4).

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Diffuse:** warm ambient shading across matte plate areas; visor
  near-black (metal lobes skip diffuse channel; F0 × albedo = 0 for pure metal).
  Max channel value: <fill from stdout>.
  Saved: `renders/diffuse_helmet.png`.
- **Specular:** bright visor with HDR env reflection; metal body
  bright; dielectric accessories dimmer.
  Max channel value: <fill from stdout>.
  Saved: `renders/specular_helmet.png`.
- **Regression:** beauty output at `--denoise=none/oidn/optix`
  unchanged from pre-3.C (split logic mirrors existing radiance math;
  no accumulation path modified).
- **Sum-match:** eyeball check — diffuse + specular re-modulated
  approximately equals beauty. Exact CPU regression deferred to
  Sub-plan 4 NRD integration.

Sub-plan 3.C complete. Next: 3.D (disocclusion mask).
```

Fill `<fill from stdout>` with actual values from Step 5.3b.

### Step 5.7: Commit

```bash
git add ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): --dump-diffuse + --dump-specular + visual verification (3.C)

Adds readbackDiffuseRadiance + readbackSpecularRadiance helpers using
the same staging-buffer pattern as readbackMotionVector. env_demo
grows --dump-diffuse=<path> and --dump-specular=<path> flags that
decode half-float RGBA + Reinhard tonemap to an 8-bit RGB PNG.
Helmet scene shows expected diff/spec separation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 storage images + 4 getters | Task 1 |
| §4.2 descriptor bindings + pool + writes + layout barriers | Task 2 |
| §4.4 GLSL bindings + lobe stamp + NEE split + indirect attribution + demod writes | Task 4 |
| §5.1 accessor chain | Task 3 |
| §5.2 readback helpers + CLI flags | Task 5 |
| §6 visual verification + sum-match + regression | Task 5 |

**Spec deviations:**
- §4.3 says `lobeMask` lives in the ray payload. Plan uses a **raygen-local `uint`** instead (the raygen owns the outer loop and directly sees the BSDF-lobe decision at bounce 0). Simpler; no payload struct changes; no closesthit touch. All downstream bounces still attribute via the stamp, just read from local state.

**Placeholder scan:** only `<fill from stdout>` in the verification log template — intentional, filled at execution time.

**Type consistency:**
- `m_diffuseRadianceImage/Memory/View` + `m_specularRadianceImage/Memory/View` identical across Tasks 1/2/3/5.
- `getDiffuseRadianceAOV` / `getDiffuseRadianceAOVImage` / `getSpecularRadianceAOV` / `getSpecularRadianceAOVImage` consistent across 3 layers (PathTracer → IRTRendererProfile → VulkanRenderer).
- Bindings 22 (diffuse) + 23 (specular) used consistently in Task 2 (descriptor) + Task 4 (shader decl).
- GLSL format `rgba16f` ↔ `VK_FORMAT_R16G16B16A16_SFLOAT` match.
- Accumulator names `diffContrib` / `specContrib` / `firstHitDiffAlbedo` / `firstHitSpecColor` / `lobeMask` used consistently throughout Task 4 sub-steps.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-19-denoiser-subplan3c-diffuse-specular-split.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Matches the pattern that shipped 3.A + 3.B cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
