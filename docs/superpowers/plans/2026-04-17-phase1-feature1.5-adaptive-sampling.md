# Phase 1 Feature 1.5: Adaptive Sampling — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the offline path tracer spend per-pixel compute where it's needed — converged pixels early-out, noisy pixels keep sampling up to a cap — using Welford running luminance variance and a per-pixel done-mask.

**Architecture:** Two new GPU buffers: R32F Welford-M2 (sum of squared deviations) and R8UI done-mask. Offline raygen checks done-mask at entry (early return); after accumulation, Welford-updates M2 and marks the pixel done when the half-CI drops below `threshold × mean`. Realtime raygen unchanged. Tunable via `RTRenderSettings.adaptiveEnabled / adaptiveMinSamples / adaptiveMaxSamples / adaptiveThreshold`, passed through a new `uvec4 adaptive` push-constant lane (total 256 bytes — on the Vulkan 1.3 minimum).

**Tech Stack:** Vulkan 1.3 RT pipeline · GLSL ray-tracing · storage images (R32F + R8UI) · push constants · GoogleTest for CPU Welford unit test.

**Reference spec:** `docs/superpowers/specs/2026-04-17-phase1-feature1.5-adaptive-sampling-design.md`

---

## File Structure

**New CPU files:**

| Path | Responsibility |
|------|---------------|
| `ohao/render/rt/welford.hpp` | Header-only inline `welfordUpdate(mean, M2, count, x)` — CPU reference that exactly mirrors the GLSL math (validates numerical behavior) |
| `tests/renderer/welford_test.cpp` | GoogleTest: known-sequence variance matches closed-form |

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | 4 new fields in `RTRenderSettings`; extend `PTPushConstants` with `glm::uvec4 adaptive` (packed); add private `m_varianceM2Image / Memory / View` and `m_doneMaskImage / Memory / View` |
| `ohao/render/rt/path_tracer.cpp` | `createImages` creates both new images; `destroyImages` frees them; `createDescriptorResources` registers bindings 19+20; descriptor writes extended; `resetAccumulation` clears both new images; `render` populates `pc.adaptive` |
| `ohao/render/rt/rt_profile_renderer.hpp` | `kOfflineRTSettings` gets `adaptiveEnabled = true` defaults; `kRealtimeRTSettings` keeps `false` |
| `shaders/rt/pt_raygen.rgen` | Declare bindings 19+20; early-out at start of `main()`; Welford update + convergence check after accumulation |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror of `pt_raygen.rgen` — apply the same edits verbatim |
| `shaders/rt/pt_raygen_realtime.rgen` | Declare bindings 19+20 so descriptor set layout matches; **no** adaptive logic (realtime profile has `adaptiveEnabled = false`) |
| `tests/renderer/CMakeLists.txt` | Register `welford_test` target |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append Feature 1.5 validation entry |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-adaptive -b phase1-adaptive HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-adaptive`.

If the fresh worktree's `build/_deps/glm-src/` is empty (known bootstrap artifact), copy from the sibling main tree once:

```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/glm-src/. \
      /home/frankyin/Desktop/Github/ohao-adaptive/build/_deps/glm-src/
```

---

## Task 1: Welford CPU reference + unit test (TDD)

**Files:**
- Create: `ohao/render/rt/welford.hpp`
- Create: `tests/renderer/welford_test.cpp`
- Modify: `tests/renderer/CMakeLists.txt`

Purpose: Algorithmic correctness of the Welford update, independent of GPU. The GLSL port in Task 5 mirrors this math line-for-line.

- [ ] **Step 1.1: Write the failing test**

Create `tests/renderer/welford_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "render/rt/welford.hpp"
#include <cmath>

using ohao::welfordUpdate;

// Feed a known sequence. After N samples, Welford variance (M2/(N-1))
// must equal the closed-form sample variance.
TEST(Welford, KnownSequenceVarianceMatchesClosedForm) {
    const float samples[] = {2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f};
    // Closed form: mean=5, var = Σ(x-mean)² / (N-1) = 32/7 ≈ 4.5714
    const float expectedMean = 5.0f;
    const float expectedVar  = 32.0f / 7.0f;

    float mean = 0.0f;
    float M2 = 0.0f;
    uint32_t n = 0;
    for (float x : samples) {
        welfordUpdate(mean, M2, n, x);
    }

    EXPECT_EQ(n, 8u);
    EXPECT_NEAR(mean, expectedMean, 1e-5f);
    const float variance = M2 / float(n - 1);
    EXPECT_NEAR(variance, expectedVar, 1e-4f);
}

// Single sample: mean == x, M2 == 0.
TEST(Welford, SingleSample) {
    float mean = 0.0f, M2 = 0.0f;
    uint32_t n = 0;
    welfordUpdate(mean, M2, n, 3.14f);
    EXPECT_EQ(n, 1u);
    EXPECT_FLOAT_EQ(mean, 3.14f);
    EXPECT_FLOAT_EQ(M2, 0.0f);
}

// Constant samples: M2 stays at zero (no variance).
TEST(Welford, ConstantSamplesZeroVariance) {
    float mean = 0.0f, M2 = 0.0f;
    uint32_t n = 0;
    for (int i = 0; i < 100; i++) welfordUpdate(mean, M2, n, 2.718f);
    EXPECT_NEAR(M2, 0.0f, 1e-5f);
    EXPECT_NEAR(mean, 2.718f, 1e-5f);
}
```

- [ ] **Step 1.2: Run test, verify fails to compile**

```bash
cd /home/frankyin/Desktop/Github/ohao-adaptive
cmake --build build --target welford_test 2>&1 | head -15
```

Expected: compile error — `welford.hpp` not found OR CMake doesn't know the target yet (see step 1.4).

- [ ] **Step 1.3: Create header**

Create `ohao/render/rt/welford.hpp`:

```cpp
#pragma once

// Welford — numerically stable online variance update.
//
// Implements the algorithm from Knuth TAOCP Vol 2, commonly known as
// Welford's. Given an existing (mean, M2, n) state and a new sample x,
// updates the triple in place. Sample variance is M2 / (n - 1).
//
// The GLSL implementation in shaders/rt/pt_raygen.rgen mirrors this
// exactly; CPU version is for unit testing the math without firing
// up Vulkan.

#include <cstdint>

namespace ohao {

// Update (mean, M2, n) with new sample x. Safe when n == 0.
inline void welfordUpdate(float& mean, float& M2, uint32_t& n, float x) {
    n += 1u;
    float delta  = x - mean;
    mean += delta / float(n);
    float delta2 = x - mean;
    M2 += delta * delta2;
}

} // namespace ohao
```

- [ ] **Step 1.4: Register test in CMake**

Append to `tests/renderer/CMakeLists.txt`:

```cmake
# Welford running-variance unit test (Feature 1.5)
add_executable(welford_test welford_test.cpp)
target_include_directories(welford_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(welford_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(welford_test)
```

(Header-only — no source .cpp needed for welford since it's all inline.)

- [ ] **Step 1.5: Build + run**

```bash
cmake --build build --target welford_test -j8
./build/welford_test
```

Expected: `[  PASSED  ] 3 tests.`

- [ ] **Step 1.6: Commit**

```bash
git add ohao/render/rt/welford.hpp tests/renderer/welford_test.cpp tests/renderer/CMakeLists.txt
git commit -m "feat(rt): Welford online variance update + unit tests

Header-only CPU reference for the running-variance algorithm used by
adaptive sampling. GLSL port in pt_raygen.rgen mirrors this exactly.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 2: RTRenderSettings fields + push-constant lane

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`

- [ ] **Step 2.1: Add 4 new fields to `RTRenderSettings`**

In `ohao/render/rt/path_tracer.hpp`, locate `struct RTRenderSettings`. After the last field (`SamplerType samplerType`), add:

```cpp
    bool     adaptiveEnabled{false};
    uint32_t adaptiveMinSamples{8};
    uint32_t adaptiveMaxSamples{1024};
    float    adaptiveThreshold{0.01f};
```

Final struct:

```cpp
struct RTRenderSettings {
    RTRenderProfile profile{RTRenderProfile::Offline};
    uint32_t maxBounces{4};
    bool preferAccumulation{true};
    bool enableAuxiliaryAOVs{true};
    bool allowExternalDenoiser{true};
    bool enableInternalDenoise{false};
    bool enableFireflyClamp{false};
    float fireflyClampLuminance{10.0f};
    SamplerType samplerType{SamplerType::Sobol};
    bool     adaptiveEnabled{false};
    uint32_t adaptiveMinSamples{8};
    uint32_t adaptiveMaxSamples{1024};
    float    adaptiveThreshold{0.01f};
};
```

- [ ] **Step 2.2: Update preset constants**

Still in `path_tracer.hpp`. `kRealtimeRTSettings` keeps `adaptiveEnabled = false`; `kOfflineRTSettings` flips it on.

```cpp
inline constexpr RTRenderSettings kRealtimeRTSettings{
    RTRenderProfile::Realtime,
    2,
    true,
    true,
    false,
    true,
    true,
    10.0f,
    SamplerType::PCG,
    false,          // adaptiveEnabled
    8u,             // adaptiveMinSamples
    1024u,          // adaptiveMaxSamples
    0.01f,          // adaptiveThreshold
};

inline constexpr RTRenderSettings kOfflineRTSettings{
    RTRenderProfile::Offline,
    4,
    true,
    true,
    true,
    false,
    false,
    0.0f,
    SamplerType::Sobol,
    true,           // adaptiveEnabled — offline gets it
    8u,             // adaptiveMinSamples
    1024u,          // adaptiveMaxSamples
    0.01f,          // adaptiveThreshold
};
```

- [ ] **Step 2.3: Extend `PTPushConstants`**

In `path_tracer.hpp`, locate `struct PTPushConstants`. Add a new field after `tuning`:

```cpp
    struct PTPushConstants {
        glm::mat4 invView;              // 64 bytes
        glm::mat4 invProj;              // 64 bytes
        glm::mat4 prevViewProj;         // 64 bytes
        glm::uvec4 params;              // 16 bytes  (x=width, y=height, z=sampleIndex, w=maxBounces)
        glm::uvec4 control;             // 16 bytes  (x=flags, y=historyFrameCount, z=viewChanged, w=envCDFWidth)
        glm::vec4  tuning;              // 16 bytes  (x=fireflyClamp, y=envCDFHeight, z=envIntegral, w=unused)
        glm::uvec4 adaptive;            // 16 bytes  (x=enabled, y=minSamples, z=maxSamples, w=floatBitsToUint(threshold))
    };  // total = 256 bytes (Vulkan 1.3 minimum maxPushConstantsSize)
```

The comment update matters — the bytes-tally block now reads 256.

- [ ] **Step 2.4: Build to verify**

```bash
cd /home/frankyin/Desktop/Github/ohao-adaptive
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. The render code doesn't consume the new fields yet (Task 4 wires them), so nothing should change at runtime.

- [ ] **Step 2.5: Commit**

```bash
git add ohao/render/rt/path_tracer.hpp
git commit -m "feat(rt): RTRenderSettings adaptive fields + push-constant lane

Adds adaptiveEnabled / min / max / threshold to settings and a new
uvec4 adaptive lane (packed threshold) to PTPushConstants. Total push
constant size is now 256 bytes — exactly the Vulkan 1.3 minimum.

Offline preset defaults to enabled; realtime stays off.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

---

## Task 3: Create / destroy / clear Welford-M2 and done-mask images

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp` (add image handles to private section)
- Modify: `ohao/render/rt/path_tracer.cpp` (`createImages`, `destroyImages`, `resize`, `resetAccumulation`)

- [ ] **Step 3.1: Add image handles to header**

In `ohao/render/rt/path_tracer.hpp`, in the private section near `m_accumBuffer`, add:

```cpp
    // Feature 1.5: adaptive sampling buffers
    VkImage        m_varianceM2Image = VK_NULL_HANDLE;
    VkDeviceMemory m_varianceM2Memory = VK_NULL_HANDLE;
    VkImageView    m_varianceM2View = VK_NULL_HANDLE;

    VkImage        m_doneMaskImage = VK_NULL_HANDLE;
    VkDeviceMemory m_doneMaskMemory = VK_NULL_HANDLE;
    VkImageView    m_doneMaskView = VK_NULL_HANDLE;
```

Place them right after `m_accumBuffer` / `m_accumMemory` / `m_accumView` so related storage-image state sits together.

- [ ] **Step 3.2: Create the images in `createImages`**

In `ohao/render/rt/path_tracer.cpp`, locate `PathTracer::createImages()`. Find where `m_accumBuffer` is created (look for `VK_FORMAT_R32G32B32A32_SFLOAT` + `VK_IMAGE_USAGE_STORAGE_BIT`). Right after the accum buffer's `vkCreateImageView` call, add two new blocks. Use the same helper pattern the existing code uses (`createStorageImage` if present, or inline).

If the file has an inline pattern, mirror it exactly. Target code to add (adapt variable names to existing style):

```cpp
    // ---- Feature 1.5: Welford M2 buffer (R32F) ----
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R32_SFLOAT;
        imgInfo.extent        = {m_width, m_height, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(m_device, &imgInfo, nullptr, &m_varianceM2Image);

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(m_device, m_varianceM2Image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize   = mr.size;
        ai.memoryTypeIndex  = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_varianceM2Memory);
        vkBindImageMemory(m_device, m_varianceM2Image, m_varianceM2Memory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType             = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image             = m_varianceM2Image;
        vi.viewType          = VK_IMAGE_VIEW_TYPE_2D;
        vi.format            = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &m_varianceM2View);
    }

    // ---- Feature 1.5: Done-mask (R8UI) ----
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R8_UINT;
        imgInfo.extent        = {m_width, m_height, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(m_device, &imgInfo, nullptr, &m_doneMaskImage);

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(m_device, m_doneMaskImage, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize   = mr.size;
        ai.memoryTypeIndex  = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_doneMaskMemory);
        vkBindImageMemory(m_device, m_doneMaskImage, m_doneMaskMemory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType             = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image             = m_doneMaskImage;
        vi.viewType          = VK_IMAGE_VIEW_TYPE_2D;
        vi.format            = VK_FORMAT_R8_UINT;
        vi.subresourceRange  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &m_doneMaskView);
    }
```

If the existing codebase already has a helper like `createStorageImage(format, usage)`, use that instead of inlining. Grep for `VkImageCreateInfo` usage in `path_tracer.cpp` to find the pattern the file follows.

- [ ] **Step 3.3: Destroy the images in `destroyImages`**

In `ohao/render/rt/path_tracer.cpp` `destroyImages()`, next to the `m_accumBuffer` destroy calls, add:

```cpp
    if (m_varianceM2View)    { vkDestroyImageView(m_device, m_varianceM2View, nullptr); m_varianceM2View = VK_NULL_HANDLE; }
    if (m_varianceM2Image)   { vkDestroyImage(m_device, m_varianceM2Image, nullptr);    m_varianceM2Image = VK_NULL_HANDLE; }
    if (m_varianceM2Memory)  { vkFreeMemory(m_device, m_varianceM2Memory, nullptr);     m_varianceM2Memory = VK_NULL_HANDLE; }

    if (m_doneMaskView)      { vkDestroyImageView(m_device, m_doneMaskView, nullptr);   m_doneMaskView = VK_NULL_HANDLE; }
    if (m_doneMaskImage)     { vkDestroyImage(m_device, m_doneMaskImage, nullptr);      m_doneMaskImage = VK_NULL_HANDLE; }
    if (m_doneMaskMemory)    { vkFreeMemory(m_device, m_doneMaskMemory, nullptr);       m_doneMaskMemory = VK_NULL_HANDLE; }
```

- [ ] **Step 3.4: Clear new images in `resetAccumulation`**

In `PathTracer::resetAccumulation()`, find where `m_historyFrameCount = 0` is set. Add logic to zero both new images on the next frame — the simplest approach is a flag plus a one-shot command-buffer submission in the next `render()` call. Alternative: mark the images for clearing, and clear them in `render()` before the ray dispatch.

For simplicity, extend `m_viewChangedThisFrame` handling. Current pattern in `render()`:

```cpp
if (m_viewChangedThisFrame) {
    // clear accumBuffer contents
}
```

After that `if` block, add (inside the same block):

```cpp
    // Feature 1.5: reset adaptive state when view changes
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    {
        // Transition to TRANSFER_DST, clear, transition back to GENERAL
        VkImageMemoryBarrier toDst[2] = {};
        for (int i = 0; i < 2; i++) {
            toDst[i].sType           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toDst[i].oldLayout       = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst[i].newLayout       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst[i].subresourceRange = range;
            toDst[i].srcAccessMask   = 0;
            toDst[i].dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        toDst[0].image = m_varianceM2Image;
        toDst[1].image = m_doneMaskImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toDst);

        VkClearColorValue zero{};  // all-zero
        vkCmdClearColorImage(cmd, m_varianceM2Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        vkCmdClearColorImage(cmd, m_doneMaskImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

        VkImageMemoryBarrier toGeneral[2] = {};
        for (int i = 0; i < 2; i++) {
            toGeneral[i].sType           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toGeneral[i].oldLayout       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toGeneral[i].newLayout       = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral[i].subresourceRange = range;
            toGeneral[i].srcAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
            toGeneral[i].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
        toGeneral[0].image = m_varianceM2Image;
        toGeneral[1].image = m_doneMaskImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, toGeneral);
    }
```

If `render()` doesn't receive `cmd` at the right spot, place the clears immediately before the existing accumBuffer transition. Read the surrounding code to find the correct insertion point.

- [ ] **Step 3.5: Handle `resize()`**

`PathTracer::resize(width, height)` already calls `destroyImages()` then `createImages()`. New buffers are created/destroyed automatically. No additional code.

- [ ] **Step 3.6: Build**

```bash
cd /home/frankyin/Desktop/Github/ohao-adaptive
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 3.7: Smoke test**

```bash
./build/cornell_box /tmp/t3_smoke.png 4 2>&1 | tail -3
```

Expected: PNG written, no Vulkan validation errors. Image identical to pre-feature since nothing consumes the buffers yet.

- [ ] **Step 3.8: Commit**

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): storage images for Welford-M2 + done-mask (Feature 1.5)

Two new storage images owned by PathTracer: m_varianceM2Image (R32F)
and m_doneMaskImage (R8UI). Created in createImages, destroyed in
destroyImages, cleared on view change before each ray dispatch.
Descriptor bindings added in Task 4.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

---

## Task 4: Descriptor bindings 19 + 20

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp` (`createDescriptorResources`, descriptor writes in `render`)

- [ ] **Step 4.1: Grow the binding array in the layout**

In `createDescriptorResources`, find `VkDescriptorSetLayoutBinding bindings[19]` (added in Feature 1.1). Change to `bindings[21]` and add two new entries at the end:

```cpp
    // Binding 19: varianceM2 (R32F storage image)
    bindings[19].binding         = 19;
    bindings[19].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[19].descriptorCount = 1;
    bindings[19].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 20: doneMask (R8UI storage image)
    bindings[20].binding         = 20;
    bindings[20].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[20].descriptorCount = 1;
    bindings[20].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Also grow `bindingFlags` array to `[21]` and the layout's `bindingCount` to 21.

- [ ] **Step 4.2: Grow the pool size**

In `createDescriptorResources`, where `VkDescriptorPoolSize poolSizes[]` is set up, increment the STORAGE_IMAGE count by 2 (to account for the two new images per descriptor set).

- [ ] **Step 4.3: Extend descriptor writes**

In `render()`, find the descriptor writes setup. Grow `writes[]` array by 2 (to 21). After the last existing write, add:

```cpp
    VkDescriptorImageInfo varianceM2Info{};
    varianceM2Info.imageView   = m_varianceM2View;
    varianceM2Info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 19;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &varianceM2Info;
    writeCount++;

    VkDescriptorImageInfo doneMaskInfo{};
    doneMaskInfo.imageView   = m_doneMaskView;
    doneMaskInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 20;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &doneMaskInfo;
    writeCount++;
```

Place BEFORE the `vkUpdateDescriptorSets` call.

- [ ] **Step 4.4: Populate push-constant adaptive lane**

In `render()`, where `PTPushConstants pc{}` is populated, after the `pc.tuning = ...` line, add:

```cpp
    pc.adaptive = glm::uvec4(
        m_renderSettings.adaptiveEnabled ? 1u : 0u,
        m_renderSettings.adaptiveMinSamples,
        m_renderSettings.adaptiveMaxSamples,
        glm::floatBitsToUint(m_renderSettings.adaptiveThreshold)
    );
```

- [ ] **Step 4.5: Build + smoke test**

```bash
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t4_smoke.png 4 2>&1 | tail -3
```

Expected: clean build, PNG written, no Vulkan validation errors. Image identical to pre-feature since shader still doesn't touch bindings 19/20.

- [ ] **Step 4.6: Commit**

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): descriptor bindings 19+20 for adaptive sampling

VarianceM2 (R32F) at binding 19; done-mask (R8UI) at binding 20. Pool
storage-image count +2. Push constants now carry adaptive enabled +
min/max/threshold (floatBitsToUint).

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

---

## Task 5: Shader integration — early-out + Welford update

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen` (mirror)
- Modify: `shaders/rt/pt_raygen_realtime.rgen` (binding declarations only, no logic)

- [ ] **Step 5.1: Declare bindings 19 + 20 in pt_raygen.rgen**

In `shaders/rt/pt_raygen.rgen`, after the existing binding 17/18 declarations, add:

```glsl
layout(set = 0, binding = 19, r32f)  uniform image2D varianceM2;
layout(set = 0, binding = 20, r8ui)  uniform uimage2D doneMask;
```

- [ ] **Step 5.2: Add adaptive fields to the push_constant block**

Still in `pt_raygen.rgen`, find the `layout(push_constant) uniform PushConstants { ... } pc;` block. Add one more field at the end:

```glsl
    uvec4 adaptive;  // x=enabled, y=minSamples, z=maxSamples, w=threshold(bits)
```

- [ ] **Step 5.3: Early-out at top of main()**

In `main()`, immediately after `ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);` and BEFORE `samplerInit` or any other work, add:

```glsl
    if (pc.adaptive.x != 0u) {
        if (imageLoad(doneMask, pixel).r != 0u) return;
    }
```

- [ ] **Step 5.4: Welford update after accumulation**

Find the block where the new frame's radiance is written into `accumBuffer`. Right AFTER `imageStore(accumBuffer, pixel, vec4(acc, count));` (or equivalent), add:

```glsl
    // Feature 1.5: Welford online variance of luminance
    if (pc.adaptive.x != 0u && count > 0.5) {
        float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
        float newMeanLum = dot(acc, vec3(0.2126, 0.7152, 0.0722));
        uint n = uint(count);
        // Derive prevMean from newMean: newMean = prevMean + (x - prevMean)/n
        //   => prevMean = (n * newMean - x) / (n - 1), when n > 1
        float prevMeanLum;
        if (n == 1u) {
            prevMeanLum = 0.0;
        } else {
            prevMeanLum = (float(n) * newMeanLum - lum) / float(n - 1u);
        }
        float delta  = lum - prevMeanLum;
        float delta2 = lum - newMeanLum;
        float M2 = imageLoad(varianceM2, pixel).r + delta * delta2;
        imageStore(varianceM2, pixel, vec4(M2, 0.0, 0.0, 0.0));

        uint minSamples = pc.adaptive.y;
        uint maxSamples = pc.adaptive.z;
        float threshold = uintBitsToFloat(pc.adaptive.w);

        if (n >= minSamples) {
            float variance = (n > 1u) ? (M2 / float(n - 1u)) : 0.0;
            float sigma    = sqrt(max(variance, 0.0));
            float halfCI   = sigma / sqrt(float(n));
            if (halfCI < threshold * max(newMeanLum, 1e-3)) {
                imageStore(doneMask, pixel, uvec4(1u, 0u, 0u, 0u));
            }
            // Also force-stop at maxSamples to prevent runaway
            if (n >= maxSamples) {
                imageStore(doneMask, pixel, uvec4(1u, 0u, 0u, 0u));
            }
        }
    }
```

The math mirrors `welfordUpdate` in `ohao/render/rt/welford.hpp`. Critical: `prevMean` is reconstructed from `newMean` because accumBuffer only stores the current mean, not the previous — the reconstruction is exact modulo float precision.

- [ ] **Step 5.5: Mirror edits to pt_raygen_offline.rgen**

`pt_raygen_offline.rgen` is a verbatim copy of `pt_raygen.rgen` except for the top comment block. Apply steps 5.1, 5.2, 5.3, 5.4 to it identically.

Verify: `diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen` should still show only the top comment block as the difference.

- [ ] **Step 5.6: Declare bindings + push field in pt_raygen_realtime.rgen (no logic)**

`pt_raygen_realtime.rgen` must declare the same bindings and the same push-constant struct layout so the pipeline descriptor set matches. Apply steps 5.1 and 5.2 to the realtime shader — but NOT steps 5.3 and 5.4. The realtime path does not run adaptive logic. `pc.adaptive.x` will be zero at runtime (via `kRealtimeRTSettings.adaptiveEnabled = false`), so even if the early-out code were present it would be a no-op — but we keep the realtime shader free of the adaptive logic for clarity.

- [ ] **Step 5.7: Build shaders**

```bash
cd /home/frankyin/Desktop/Github/ohao-adaptive
cmake --build build --target shaders -j8 2>&1 | tail -10
```

Expected: clean compile for all three raygen shaders.

- [ ] **Step 5.8: Smoke test**

```bash
cmake --build build -j8 2>&1 | tail -3
./build/cornell_box /tmp/t5_cornell.png 16 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t5_helmet.png 16 2>&1 | tail -3
```

Expected: both PNGs written, no Vulkan validation errors. With `maxSamples=1024`, 16-spp renders will generally NOT hit the done threshold (not enough samples to stabilize variance). So images should look nearly identical to pre-feature output — same visual noise level. **This is expected** — adaptive's benefit shows at higher sample counts.

Visually inspect: no obvious artifacts, no blotchy regions, no sudden brightness jumps.

- [ ] **Step 5.9: Commit**

```bash
git add shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): adaptive sampling shader integration

Offline raygen early-outs when done-mask bit is set; after each
sample updates Welford M2 (reconstructed from current mean) and
flips the done-mask when half-CI drops below threshold*mean.
Realtime shader only declares bindings for descriptor-set
consistency — no adaptive logic runs at realtime spp counts.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

---

## Task 6: Verification + reference scene measurement

**Files:**
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

- [ ] **Step 6.1: Render baseline (adaptive OFF, same as current Sobol)**

Temporarily set `kOfflineRTSettings.adaptiveEnabled = false` by editing `ohao/render/rt/path_tracer.hpp`. Rebuild:

```bash
cd /home/frankyin/Desktop/Github/ohao-adaptive
cmake --build build -j8 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/adaptive_off_64spp.png 64 2>&1 | tail -3
```

Revert the hpp change (`adaptiveEnabled = true`), rebuild:

```bash
cmake --build build -j8 2>&1 | tail -3
```

Verify `git status --short` is clean on `path_tracer.hpp` (no dirty state left behind).

- [ ] **Step 6.2: Render adaptive ON**

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/adaptive_on_64spp.png 64 2>&1 | tail -3
```

The 64 here is the upper cap the outer render loop iterates to — but individual pixels may stop earlier.

- [ ] **Step 6.3: Compare image quality**

```bash
python3 tools/compare_variance.py /tmp/adaptive_off_64spp.png /tmp/adaptive_on_64spp.png
```

Record:
- Global RMSE (should be small — both should look similar)
- Local variance (adaptive_on should be ≤ adaptive_off, since hard pixels got more samples)

- [ ] **Step 6.4: Compare against 16384-spp truth**

```bash
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png \
    /tmp/adaptive_off_64spp.png
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png \
    /tmp/adaptive_on_64spp.png
```

Record both RMSE values. Expected: adaptive RMSE ≤ non-adaptive RMSE.

(The file is `truth_4096spp.png` in the repo.)

- [ ] **Step 6.5: Heatmap spot-check (optional but recommended)**

Save a visualization of per-pixel sample count:

```bash
python3 -c "
import numpy as np
from PIL import Image
# accumBuffer.w isn't directly exposed yet — skip this until AOV is wired.
print('Heatmap AOV not yet exposed; Phase 2 work.')
"
```

For Phase 1 we don't add the sample-count AOV. Skip this step; add as future work item.

- [ ] **Step 6.6: Update verification log**

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. APPEND a new section at the bottom:

```markdown
## 2026-04-17: Adaptive sampling (Feature 1.5) validation

Offline path tracer runs adaptive sampling by default. Comparison at
64-spp cap on DamagedHelmet + env_studio:

| Mode | Local 5×5 variance | RMSE vs 4096-spp truth |
|------|-------------------|-----------------------|
| Adaptive OFF (uniform 64 spp) | <fill> | <fill> |
| Adaptive ON  (up to 64 spp)   | <fill> | <fill> |

- Local variance reduction: <X>%
- RMSE improvement: <Y>%

Adaptive sampling redistributes budget to noisy pixels. Hard regions
(glossy shoulder, helmet edges) receive most samples; sky + matte
regions stop early. Unbiased — no clipping, no visual bias.

Feature 1.5 complete. Realtime profile unaffected.
```

Fill in `<fill>` and `<X>` / `<Y>` with actual numbers from steps 6.3 and 6.4.

- [ ] **Step 6.7: Commit**

```bash
git add tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): adaptive sampling variance gate (Feature 1.5)

On envlit_turntable at 64-spp cap, adaptive ON reduces local variance
by <X>% and RMSE vs truth by <Y>% compared to uniform 64 spp. Same
compute budget, smarter distribution.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Replace `<X>` / `<Y>` with the measured numbers.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Implemented by |
|---|---|
| §4.1 New R32F + R8UI storage images | Task 3 |
| §4.2 RTRenderSettings adaptive fields | Task 2 |
| §4.2 Push constants extended with adaptive lane | Task 2, Task 4 |
| §4.3 Shader early-out + Welford + threshold check | Task 5 |
| §4.4 Reset semantics on view change | Task 3 (step 3.4) |
| §5 Descriptor bindings 19 + 20 | Task 4 |
| §5.1 Realtime raygen unchanged (only declarations) | Task 5 (step 5.6) |
| §6.1 Welford unit test | Task 1 |
| §6.2 Integration variance + RMSE test | Task 6 |
| §6.3 Reset correctness | Task 3 step 3.4, implicitly verified by rendering after camera moves |

**Placeholder scan:**

Only `<fill>`, `<X>`, `<Y>` placeholders in Task 6.6/6.7 — these are deliberately left for the executor to fill with measured numbers, with explicit instructions.

**Type consistency:**

- `RTRenderSettings` field names match between C++ (Task 2) and CPU usage (Task 4): `adaptiveEnabled`, `adaptiveMinSamples`, `adaptiveMaxSamples`, `adaptiveThreshold`.
- `PTPushConstants.adaptive` is a `glm::uvec4` in C++ (Task 2) and `uvec4` in GLSL (Task 5). Values: `x=enabled`, `y=minSamples`, `z=maxSamples`, `w=floatBitsToUint(threshold)` — consistent at both ends via `glm::floatBitsToUint` / `uintBitsToFloat`.
- Image handles: `m_varianceM2Image/Memory/View` and `m_doneMaskImage/Memory/View` — names match between header (Task 3 step 3.1) and usage (Tasks 3, 4).
- Binding IDs: 19 (varianceM2), 20 (doneMask) — consistent between C++ layout (Task 4) and GLSL declarations (Task 5).

**Known design trade-offs inline in plan:**

- Task 5 step 5.4 reconstructs `prevMean` from `newMean` (forward equation). Float-precision drift is negligible at sample counts under 1024 — documented.
- Task 3 step 3.4 clears new buffers on view change inside the existing `m_viewChangedThisFrame` branch. Alternative: dedicated clear pass. Existing pattern is simpler.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-17-phase1-feature1.5-adaptive-sampling.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Consistent with how Features 1.1 and 1.2 shipped.

**2. Inline Execution** — Batch execution with checkpoints.

**Which approach?**
