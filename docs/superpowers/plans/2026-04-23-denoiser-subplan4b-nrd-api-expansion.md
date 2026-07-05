# Denoiser Sub-plan 4.B: NRD API Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add NRD's required packed `normal+roughness` AOV at binding 26, expand `NrdDenoiser` API with per-frame `setCommonSettings` + `setInputImages`, and verify NRD accepts our settings via probe log. No compute dispatch yet — that's 4.C.

**Architecture:** Two tasks. T1 adds binding 26 (RGBA8 packed normal+roughness) to the PathTracer, mirrors the 3.B/3.C.6 AOV pattern exactly; raygen writes NRD-canonical octahedral-encoded normal + roughness at first hit. T2 expands `NrdDenoiser` public API with two new structs (`NrdCameraInputs`, `NrdInputImages`) and two methods; implementation calls `nrd::SetCommonSettings` per-frame and stores input image views on Impl for 4.C's dispatch to consume.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · NVIDIA NRD v4.17 · C++17.

**Reference spec:** `docs/superpowers/specs/2026-04-23-denoiser-subplan4b-nrd-api-expansion-design.md`

---

## File Structure

**New:** none.

**Modified:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | +3 fields (`m_normalRoughnessImage/Memory/View`) + 2 getters. |
| `ohao/render/rt/path_tracer.cpp` | createImages/destroyImages blocks; binding 26 in layout; pool STORAGE_IMAGE 15→16; descriptor write; AOV barriers `[9]→[10]`; update NRD probe with setCommonSettings call. |
| `ohao/render/rt/rt_profile_renderer.hpp` | +2 pure virtuals + 2 delegators. |
| `ohao/gpu/vulkan/renderer.hpp` | +2 accessor decls. |
| `ohao/gpu/vulkan/renderer.cpp` | +2 dispatchers. |
| `shaders/rt/pt_raygen.rgen` | Binding 26 decl + `octEncode` helper + first-hit imageStore + miss zero-init. |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror. |
| `shaders/rt/pt_raygen_realtime.rgen` | Same edits. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` | Add `NrdCameraInputs` + `NrdInputImages` structs + 2 public methods. |
| `ohao/render/rt/denoise/nrd_denoise.cpp` | Implement `setCommonSettings` (call `nrd::SetCommonSettings`) + `setInputImages` (store on Impl). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 4.B entry. |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-4b-nrd-api -b denoiser-4b-nrd-api-expansion HEAD
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api

# Bootstrap build if needed:
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -5
# If empty build/_deps:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

OptiX (optional, unchanged from 4.A):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Normal+roughness AOV pipeline (binding 26)

This is a carbon-copy of the 3.B / 3.C.6 AOV pattern. The implementer should reference those prior commits on the `animation` branch for the exact idiom — this task mirrors them for a new RGBA8 packed AOV.

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`
- Modify: `ohao/render/rt/path_tracer.cpp`
- Modify: `ohao/render/rt/rt_profile_renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen`
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 1.1: Add PathTracer fields + getters

Edit `ohao/render/rt/path_tracer.hpp`. Find the `m_specColorView` block (from 3.C.6). Immediately after, add:

```cpp
    // Feature 4.B: NRD REBLUR IN_NORMAL_ROUGHNESS — oct-encoded normal (RG) + roughness (B)
    VkImage        m_normalRoughnessImage = VK_NULL_HANDLE;
    VkDeviceMemory m_normalRoughnessMemory = VK_NULL_HANDLE;
    VkImageView    m_normalRoughnessView = VK_NULL_HANDLE;
```

Public accessors — near `getSpecColorAOV`:

```cpp
    VkImageView getNormalRoughnessAOV()      const { return m_normalRoughnessView; }
    VkImage     getNormalRoughnessAOVImage() const { return m_normalRoughnessImage; }
```

### Step 1.2: Create/destroy in createImages/destroyImages

Edit `ohao/render/rt/path_tracer.cpp`. Find the 3.C.6 spec-color block (ends with `if (vkCreateImageView(...) ... &m_specColorView)`). Immediately after, before final `return true;` in `createImages()`, insert:

```cpp
    // ---- Feature 4.B: Normal+roughness packed AOV (RGBA8 UNORM) for NRD REBLUR ----
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

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_normalRoughnessImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_normalRoughnessImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_normalRoughnessMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_normalRoughnessImage, m_normalRoughnessMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_normalRoughnessImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_normalRoughnessView) != VK_SUCCESS) return false;
    }
```

In `destroyImages()`, near the 3.C.6 `m_specColor*` destroy block, add:

```cpp
    if (m_normalRoughnessView)   { vkDestroyImageView(m_device, m_normalRoughnessView, nullptr);  m_normalRoughnessView = VK_NULL_HANDLE; }
    if (m_normalRoughnessImage)  { vkDestroyImage(m_device, m_normalRoughnessImage, nullptr);     m_normalRoughnessImage = VK_NULL_HANDLE; }
    if (m_normalRoughnessMemory) { vkFreeMemory(m_device, m_normalRoughnessMemory, nullptr);      m_normalRoughnessMemory = VK_NULL_HANDLE; }
```

### Step 1.3: Descriptor bindings 26 + pool + writes + AOV barriers

In `createDescriptorResources()`, find the bindings array (currently size 26 after 3.C.6). Grow to `[27]`; update `bindingFlags[27]`, `flagsInfo.bindingCount = 27`, `layoutInfo.bindingCount = 27`.

After binding 25 entries:

```cpp
    // Binding 26: normal+roughness packed AOV (RGBA8 UNORM) — Sub-plan 4.B
    bindings[26].binding         = 26;
    bindings[26].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[26].descriptorCount = 1;
    bindings[26].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Pool — bump `STORAGE_IMAGE` descriptorCount by 1 (from 15 to 16), extend running comment:

```cpp
    // +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec radiance (3.C), +2 albedo/specColor (3.C.6), +1 normalRoughness (4.B)
```

Descriptor writes in `render()` — find the 3.C.6 `specColorInfo` write. Grow `writes[]` array by 1 (from 26 to 27). After the specColor write:

```cpp
    // Binding 26: normal+roughness — Sub-plan 4.B
    VkDescriptorImageInfo normalRoughnessInfo{};
    normalRoughnessInfo.imageView   = m_normalRoughnessView;
    normalRoughnessInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 26;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &normalRoughnessInfo;
    writeCount++;
```

AOV barrier block — grow `aovBarriers[9]` → `[10]`. After the 3.C.6 specColor barrier (index 8):

```cpp
    // Sub-plan 4.B: normal+roughness AOV barrier
    aovBarriers[9].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aovBarriers[9].srcAccessMask = 0;
    aovBarriers[9].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aovBarriers[9].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    aovBarriers[9].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aovBarriers[9].image = m_normalRoughnessImage;
    aovBarriers[9].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
```

Update `vkCmdPipelineBarrier` count from 9 to 10.

### Step 1.4: Accessor chain

Edit `ohao/render/rt/rt_profile_renderer.hpp`. After the 3.C.6 spec-color virtuals, add:

```cpp
    virtual VkImageView getNormalRoughnessAOV()      const = 0;
    virtual VkImage     getNormalRoughnessAOVImage() const = 0;
```

In `RTProfileRendererBase`, after 3.C.6 delegators:

```cpp
    VkImageView getNormalRoughnessAOV()      const override { return m_pathTracer.getNormalRoughnessAOV(); }
    VkImage     getNormalRoughnessAOVImage() const override { return m_pathTracer.getNormalRoughnessAOVImage(); }
```

Edit `ohao/gpu/vulkan/renderer.hpp`. After the 3.C.6 `getSpecColorAOVImage` decl:

```cpp
    VkImageView getNormalRoughnessAOV()      const;
    VkImage     getNormalRoughnessAOVImage() const;
```

Edit `ohao/gpu/vulkan/renderer.cpp`. Mirror the 3.C.6 split idiom (view uses helper, image uses explicit check).

After `getSpecColorAOV()`:

```cpp
VkImageView VulkanRenderer::getNormalRoughnessAOV() const {
    if (const auto* renderer = getRTRenderer(m_renderMode)) {
        return renderer->getNormalRoughnessAOV();
    }
    return VK_NULL_HANDLE;
}
```

After `getSpecColorAOVImage()`:

```cpp
VkImage VulkanRenderer::getNormalRoughnessAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getNormalRoughnessAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getNormalRoughnessAOVImage();
    }
    return VK_NULL_HANDLE;
}
```

### Step 1.5: Raygen — binding decl + octEncode helper + imageStore

Edit `shaders/rt/pt_raygen.rgen`. After the binding 25 (specColor) decl, add:

```glsl
layout(set = 0, binding = 26, rgba8) uniform image2D normalRoughnessAOV;
```

Near the top-of-file helpers (where `cosineHemisphere`, `ACES` are), add:

```glsl
// Sub-plan 4.B: octahedral encoding for NRD REBLUR IN_NORMAL_ROUGHNESS
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 enc = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
    return enc * 0.5 + 0.5;
}
```

Zero-init block at pixel entry (near other 3.C.6 zero-inits):

```glsl
    // Sub-plan 4.B: zero-init normal+roughness AOV for miss pixels
    imageStore(normalRoughnessAOV, pixel, vec4(0.0));
```

First-hit write — find the Sub-plan 3.C.6 demod AOV writes block (around the line `imageStore(diffuseAlbedoAOV, pixel, vec4(firstHitDiffAlbedo, 1.0));`). AFTER both 3.C.6 writes, INSIDE the same `if (bounce == 0u) { ... }` block:

```glsl
            // Sub-plan 4.B: pack oct-encoded normal (RG) + roughness (B) for NRD REBLUR
            imageStore(normalRoughnessAOV, pixel, vec4(octEncode(N), roughness, 0.0));
```

### Step 1.6: Mirror 1.5 to offline + realtime raygens

`pt_raygen_offline.rgen` is verbatim mirror of `pt_raygen.rgen` except top comment. Apply 1.5 identically.

`pt_raygen_realtime.rgen` — same edits at analogous sites. Realtime has the same Stage A/B/C structure, same first-hit capture site.

Verify:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```
Expected: only top-comment differences.

### Step 1.7: Build + beauty regression smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api
cmake --build build --target shaders -j8 2>&1 | tail -5
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t1_4b_cornell.png 64 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t1_4b_helmet.png 64 --denoise=none 2>&1 | tail -3
```

Expected: clean build, no validation errors. Beauty bit-identical to pre-4.B (new AOV write is additive, doesn't touch radiance).

### Step 1.8: Commit T1

```bash
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp \
        ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): normal+roughness packed AOV at binding 26 (Sub-plan 4.B T1)

Adds RGBA8 storage image + descriptor + barriers for oct-encoded
normal (RG) + roughness (B) at binding 26. All 3 raygens declare
the binding, zero-init on miss, and write the packed value at
first-hit inside Stage A's capture block alongside the 3.C.6
demod AOVs.

Accessor chain: PathTracer -> IRTRendererProfile -> VulkanRenderer.
Pool STORAGE_IMAGE 15->16. AOV barrier group 9->10.

Matches NRD REBLUR IN_NORMAL_ROUGHNESS format (oct-encoded RG +
roughness B + materialID=0 A). NRD consumption lands in T2
(setInputImages) and 4.C (compute dispatch).

Beauty path untouched.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: NrdDenoiser API expansion + probe update + verification

**Files:**
- Modify: `ohao/render/rt/denoise/nrd_denoise.hpp`
- Modify: `ohao/render/rt/denoise/nrd_denoise.cpp`
- Modify: `ohao/render/rt/path_tracer.cpp` (update existing probe)
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 2.1: Expand nrd_denoise.hpp

Edit `ohao/render/rt/denoise/nrd_denoise.hpp`. Add two structs above the `NrdDenoiser` class:

```cpp
#include <array>

namespace ohao {

/// Per-frame camera state NRD needs in its CommonSettings.
/// Matrices are row-major glm::mat4-compatible layout.
struct NrdCameraInputs {
    std::array<float, 16> viewMatrix      {};
    std::array<float, 16> viewMatrixPrev  {};
    std::array<float, 16> projMatrix      {};
    std::array<float, 2>  jitter          {};   // sub-pixel offset this frame
    std::array<float, 2>  jitterPrev      {};   // previous frame's jitter
    std::array<float, 3>  motionVectorScale {1.0f, 1.0f, 0.0f};
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
};

/// Vulkan image views NRD will read during compute dispatch (4.C).
/// 4.B stores them on Impl but does not yet bind via UserPool.
struct NrdInputImages {
    VkImageView viewZ                  = VK_NULL_HANDLE;
    VkImageView motionVector           = VK_NULL_HANDLE;
    VkImageView normalRoughness        = VK_NULL_HANDLE;
    VkImageView diffRadianceHitDist    = VK_NULL_HANDLE;
    VkImageView specRadianceHitDist    = VK_NULL_HANDLE;
    VkImageView diffAlbedo             = VK_NULL_HANDLE;
    VkImageView specColor              = VK_NULL_HANDLE;
    // Outputs — 4.C creates these and passes them in:
    VkImageView outDiffRadianceHitDist = VK_NULL_HANDLE;
    VkImageView outSpecRadianceHitDist = VK_NULL_HANDLE;
};

class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&)            = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);
    void shutdown();

    /// Per-frame: push camera state into NRD's internal CommonSettings.
    /// Must be called before the 4.C compute dispatch. Returns true on success.
    bool setCommonSettings(const NrdCameraInputs& inputs);

    /// Per-frame: record the Vulkan image views NRD will consume/produce.
    /// 4.B stores them; 4.C binds them via NRD's UserPool during dispatch.
    void setInputImages(const NrdInputImages& images);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

Confirm the existing `initialize`/`shutdown` signatures from 4.A are preserved (they are — we're only adding).

### Step 2.2: Expand nrd_denoise.cpp

Edit `ohao/render/rt/denoise/nrd_denoise.cpp`. Inside the `#ifdef OHAO_NRD_ENABLED` block, expand `Impl`:

```cpp
struct NrdDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    nrd::Instance*   instance       = nullptr;

    // Sub-plan 4.B: per-frame image views stored for 4.C's dispatch.
    NrdInputImages   inputs {};
};
```

Add the two new method implementations after `shutdown()`:

```cpp
bool NrdDenoiser::setCommonSettings(const NrdCameraInputs& in) {
    if (!m_impl->instance) {
        return false;
    }

    nrd::CommonSettings s = {};
    std::memcpy(s.viewToClipMatrix,      in.projMatrix.data(),     sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrix,     in.viewMatrix.data(),     sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrixPrev, in.viewMatrixPrev.data(), sizeof(float) * 16);
    std::memcpy(s.motionVectorScale,     in.motionVectorScale.data(), sizeof(float) * 3);
    std::memcpy(s.cameraJitter,          in.jitter.data(),         sizeof(float) * 2);
    std::memcpy(s.cameraJitterPrev,      in.jitterPrev.data(),     sizeof(float) * 2);
    s.frameIndex = in.frameIndex;
    s.isMotionVectorInWorldSpace = in.isMotionVectorInWorldSpace;

    nrd::Result r = nrd::SetCommonSettings(*m_impl->instance, s);
    if (r != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] SetCommonSettings failed: " << int(r) << std::endl;
        return false;
    }
    return true;
}

void NrdDenoiser::setInputImages(const NrdInputImages& images) {
    m_impl->inputs = images;
}
```

`#include <cstring>` and `#include <iostream>` near the top of the file if not already present.

**Verify NRD v4.17 `CommonSettings` struct field names** before writing — exact names may differ slightly (`viewToClipMatrix` vs `viewToClip`, `worldToViewMatrix` vs `worldToView`, etc.). Check via:

```bash
grep -A 20 "struct CommonSettings" build/_deps/nrd-src/Include/NRDDescs.h 2>/dev/null | head -60
# or:
find build/_deps/nrd-src/Include -name "*.h" -exec grep -l "CommonSettings" {} \;
```

Adjust the `std::memcpy` targets + scalar fields to match the resolved names.

### Step 2.3: Update PathTracer probe to exercise setCommonSettings

Edit `ohao/render/rt/path_tracer.cpp`. Find the existing 4.A probe block (the `#ifdef OHAO_NRD_ENABLED` block added in `PathTracer::init`). Replace the existing simple probe with:

```cpp
#ifdef OHAO_NRD_ENABLED
    {
        NrdDenoiser nrdProbe;
        if (nrdProbe.initialize(m_device, m_physicalDevice, m_width, m_height)) {
            std::cout << "[NRD] initialized for " << m_width << "x" << m_height << std::endl;

            // Sub-plan 4.B: exercise setCommonSettings with identity matrices
            NrdCameraInputs dummyInputs {};
            for (int i = 0; i < 4; ++i) {
                dummyInputs.viewMatrix[i * 4 + i]     = 1.0f;
                dummyInputs.viewMatrixPrev[i * 4 + i] = 1.0f;
                dummyInputs.projMatrix[i * 4 + i]     = 1.0f;
            }
            dummyInputs.motionVectorScale = {1.0f, 1.0f, 0.0f};

            if (nrdProbe.setCommonSettings(dummyInputs)) {
                std::cout << "[NRD probe] 4.B CommonSettings accepted" << std::endl;
            } else {
                std::cerr << "[NRD probe] 4.B CommonSettings FAILED" << std::endl;
            }

            nrdProbe.shutdown();
        } else {
            std::cerr << "[NRD probe] initialize FAILED" << std::endl;
        }
    }
#endif
```

Match the surrounding coding style (the existing probe uses `std::cout`/`std::cerr`, so keep that).

### Step 2.4: Build + run smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t2_4b_cornell.png 4 --denoise=none 2>&1 | grep -E "NRD|Saved"
```

Expected stdout (the probe fires for each PathTracer instance — RTRealtime + RTOffline):

```
[NRD] initialized for WxH
[NRD probe] 4.B CommonSettings accepted
[NRD] initialized for WxH
[NRD probe] 4.B CommonSettings accepted
Saved: /tmp/t2_4b_cornell.png
```

No validation errors.

### Step 2.5: Verify OFF build still works

```bash
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api
rm -rf build-nonrd
cmake -B build-nonrd -S . -DOHAO_NRD=OFF 2>&1 | tail -5
cmake --build build-nonrd -j8 2>&1 | tail -5
./build-nonrd/cornell_box /tmp/t2_4b_cornell_nonrd.png 4 --denoise=none 2>&1 | grep -E "NRD|Saved"
```

Expected: no `[NRD]` logs (probe ifdef'd out), `Saved: ...` fires. Clean.

### Step 2.6: Append verification log

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-23: NRD API expansion — normal+roughness AOV + settings pump (Sub-plan 4.B)

T1: Added packed normal+roughness AOV at binding 26 (RGBA8 UNORM). All 3
raygens write oct-encoded normal (RG) + roughness (B) + 0 in A at Stage A
first-hit, matching NRD REBLUR's IN_NORMAL_ROUGHNESS canonical format.
Descriptor pool STORAGE_IMAGE 15→16. AOV barrier group 9→10. Beauty
path untouched.

T2: Expanded NrdDenoiser public API with `NrdCameraInputs` + `NrdInputImages`
structs and `setCommonSettings` + `setInputImages` methods. `setCommonSettings`
calls `nrd::SetCommonSettings` on the NRD instance with view/proj/jitter/frame
index fields populated. `setInputImages` stores VkImageView handles on Impl
for 4.C's dispatch to consume.

Verification on probe run (identity matrices, dummy jitter):
```
[NRD] initialized for 1920x1080
[NRD probe] 4.B CommonSettings accepted
```

- Beauty output unchanged from pre-4.B (AOV write is additive; radiance
  untouched).
- No validation errors on `OHAO_NRD=ON` or `OHAO_NRD=OFF` builds.

Next: Sub-plan 4.C (first REBLUR compute dispatch — the denoise actually runs).
```

### Step 2.7: Commit T2

```bash
cd /home/frankyin/Desktop/Github/ohao-4b-nrd-api
git add ohao/render/rt/denoise/nrd_denoise.hpp \
        ohao/render/rt/denoise/nrd_denoise.cpp \
        ohao/render/rt/path_tracer.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "feat(rt): NrdDenoiser setCommonSettings + setInputImages (Sub-plan 4.B T2)

Expands NrdDenoiser public API with two new structs:
- NrdCameraInputs: view/proj matrices, jitter, frame index, MV scale.
- NrdInputImages: the 7 Vulkan image views NRD consumes + 2 output views
  (outputs populated by 4.C).

Two new methods:
- setCommonSettings(NrdCameraInputs&): calls nrd::SetCommonSettings on
  the NRD instance per frame.
- setInputImages(NrdInputImages&): stores views on Impl for 4.C's
  dispatch to consume via NRD UserPool.

PathTracer probe extended to exercise setCommonSettings with identity
matrices; log '[NRD probe] 4.B CommonSettings accepted' confirms
NRD accepts our settings.

No compute dispatch yet — that's 4.C. Beauty path untouched.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 PathTracer AOV image/view + binding + descriptor + pool + barrier | T1 Steps 1.1–1.3 |
| §4.1 Accessor chain (PathTracer → profile → VulkanRenderer) | T1 Step 1.4 |
| §4.1 Raygen binding decl + octEncode + imageStore (3 raygens) | T1 Steps 1.5–1.6 |
| §4.2 NrdCameraInputs + NrdInputImages structs | T2 Step 2.1 |
| §4.2 setCommonSettings/setInputImages API + impl | T2 Steps 2.1–2.2 |
| §4.3 Probe expansion with CommonSettings exercise | T2 Step 2.3 |
| §6.1 Beauty bit-identical | T1 Step 1.7, T2 Step 2.4 |
| §6.3 Probe log confirms SetCommonSettings success | T2 Step 2.4 |
| §6.5 OFF build still clean | T2 Step 2.5 |
| §8 Success criteria 1–8 | T1 + T2 collectively |

**Placeholder scan:** no TBDs or vague steps. CommonSettings field-name resolution at T2 Step 2.2 is a legitimate investigation step against the live NRD source tree, not a placeholder-to-fix.

**Type consistency:**
- `m_normalRoughnessImage/Memory/View` used across T1 Steps 1.1, 1.2, 1.3.
- `getNormalRoughnessAOV`/`Image` used consistently across 3 layers in T1 Step 1.4.
- Binding 26, pool 16, barriers [10] consistent.
- `NrdCameraInputs`/`NrdInputImages` struct names used consistently between hpp, cpp, probe.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-23-denoiser-subplan4b-nrd-api-expansion.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Same pattern that shipped 3.x + 4.A cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
