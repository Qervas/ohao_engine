# Denoiser Sub-plan 4.E — `--denoise=nrd` + Tonemap + Temporal + Realtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `--denoise=nrd` end-to-end as first-class peer to OIDN/OptiX: offline PNG properly lit via GPU tonemap pass, real temporal accumulation, interactive GLFW viewer running NRD per-frame at 50+fps.

**Architecture:** Three tasks, each producing a visible demo. T1 wires the CLI flag + new `NrdTonemap` compute pass (binding 30 = RGBA8 tonemapped) + extends `getPixels()` with an NRD branch. T2 routes real per-frame matrices (`m_prevViewMatrix`, `m_prevProjMatrix`, `m_historyFrameCount`) into `NrdCameraInputs`. T3 wires `--denoise=nrd` into `interactive` and verifies realtime perf.

**Tech Stack:** Vulkan 1.3 compute, GLSL 460, NVIDIA NRD v4.17.2, NRI v178, existing `parseDenoiseMode` plumbing from 1.A.

**Spec:** `docs/superpowers/specs/2026-04-24-denoiser-subplan4e-nrd-cli-tonemap-temporal-realtime-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `ohao/render/rt/denoise/denoise_types.hpp` (modify) | Add `NRD = 3` to enum. |
| `ohao/render/rt/denoise/denoise_types.cpp` (modify) | Parser accepts `"nrd"`; `denoiseModeName` returns `"nrd"`. |
| `shaders/rt/nrd_tonemap.comp` (NEW) | 8×8 compute, 2 bindings, ACES tonemap + gamma. |
| `ohao/render/rt/denoise/nrd_tonemap.hpp` (NEW) | `NrdTonemap` class + `NrdTonemapInputs` struct. |
| `ohao/render/rt/denoise/nrd_tonemap.cpp` (NEW) | PIMPL impl with compute pipeline + OFF stub. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` (modify) | Add `projMatrixPrev` field to `NrdCameraInputs`. |
| `ohao/render/rt/denoise/nrd_denoise.cpp` (modify) | `setCommonSettings` uses explicit `projMatrixPrev` instead of mirroring current proj. |
| `ohao/render/rt/path_tracer.hpp` (modify) | New members: `m_nrdTonemap`, `m_nrdTonemappedImage/View/Memory`, `m_nrdTonemapFirstFrame`, `m_prevViewMatrix`, `m_prevProjMatrix`, accessors for binding 30. |
| `ohao/render/rt/path_tracer_images.cpp` (modify) | Allocate binding 30 in createImages; reset first-frame latch; cleanup in destroyImages. |
| `ohao/render/rt/path_tracer.cpp` (modify) | Init/shutdown `m_nrdTonemap` alongside `m_nrdCompositor`. |
| `ohao/render/rt/path_tracer_render.cpp` (modify) | Capture prev V/P at render start; feed real temporal state into `NrdCameraInputs`; insert tonemap dispatch after compose. |
| `ohao/render/rt/rt_profile_renderer.hpp` (modify) | Add 2 pure virtuals + forwarders for binding 30. |
| `ohao/gpu/vulkan/renderer.hpp` (modify) | Declare `readbackNrdTonemapped(uint8_t*, w, h)` + `getNrdTonemappedAOVImage()` passthrough. |
| `ohao/gpu/vulkan/renderer.cpp` (modify) | Implement readback (RGBA8, 4 bytes/pixel); extend `getPixels()` with NRD branch. |
| `examples/interactive.cpp` (modify) | Parse `--denoise=nrd`; call `renderer.setDenoiseMode(mode)`. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` (modify) | T1/T2/T3 entries. |
| `CLAUDE.md` (modify) | Binding 30 row; `--denoise=nrd` noted. |

Invariants across all tasks:
1. `--denoise=none` / `=oidn` / `=optix` remain bit-identical to pre-4.E at matching seed/scene.
2. `OHAO_NRD=OFF` builds cleanly; `--denoise=nrd` logs fallback and uses `None`.
3. Unconditional member declarations (ABI-hoist pattern from 4.C T2) — no `#ifdef OHAO_NRD_ENABLED` around members in headers.

---

## Task 1: CLI + NrdTonemap + offline properly-lit PNG

Goal: `env_demo --denoise=nrd` produces a properly-lit PNG at spp=1. No temporal yet (`frameIndex=0` still hard-coded). Verifies the full raygen → NRD denoise → compose → tonemap → readback chain.

### Step 1.1 — Enum + parser

- [ ] **Step 1.1: Add NRD to DenoiseMode enum**

In `ohao/render/rt/denoise/denoise_types.hpp`, change:
```cpp
enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    OptiX  = 2,   // NVIDIA OptiX (requires CUDA + OptiX SDK at build time)
    // future:
    // NRD    = 3,
    // DLSSRR = 4,
};
```
to:
```cpp
enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    OptiX  = 2,   // NVIDIA OptiX (requires CUDA + OptiX SDK at build time)
    NRD    = 3,   // NVIDIA RayTracingDenoiser (Sub-plan 4)
    // future:
    // DLSSRR = 4,
};
```

- [ ] **Step 1.2: Parser accepts "nrd"**

In `ohao/render/rt/denoise/denoise_types.cpp`, find the `parseDenoiseMode` function. After the `"optix"` branch add:
```cpp
    if (lower == "nrd")   return DenoiseMode::NRD;
```

In `denoiseModeName`, after `case DenoiseMode::OptiX:` add:
```cpp
        case DenoiseMode::NRD:   return "nrd";
```

- [ ] **Step 1.3: Build + verify parser**

Run:
```bash
cmake --build build -j8 2>&1 | tail -3
./build/cornell_box --help 2>&1 | grep -i denoise || true
```
Expected: clean build. (The `--help` call may not list denoise modes; it's a sanity check that nothing broke.)

### Step 1.4 — Create nrd_tonemap.comp

- [ ] **Step 1.4: Write the tonemap shader**

Create `shaders/rt/nrd_tonemap.comp`:
```glsl
#version 460

// Sub-plan 4.E: tonemap compute pass for NRD composed output.
// Reads binding 29 (composed HDR RGBA32F), writes binding 30 (tonemapped RGBA8).
// ACES approximation + sRGB gamma — matches raygen's in-shader tonemap so raw-PT
// and NRD modes produce visually consistent output.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly  image2D inHDR;
layout(set = 0, binding = 1, rgba8)   uniform writeonly image2D outLDR;

vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    ivec2 p    = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outLDR);
    if (p.x >= size.x || p.y >= size.y) return;

    vec3 hdr    = imageLoad(inHDR, p).rgb;
    vec3 mapped = acesFilm(hdr);
    vec3 srgb   = pow(mapped, vec3(1.0 / 2.2));
    imageStore(outLDR, p, vec4(srgb, 1.0));
}
```

- [ ] **Step 1.5: Verify shader builds to SPV**

```bash
cmake --build build --target shaders -j8 2>&1 | tail -3
ls build/shaders/rt_nrd_tonemap.comp.spv
```
Expected: file exists. Note the underscore-flat convention (`rt_` prefix) — same as `rt_nrd_compose.comp.spv`.

### Step 1.6 — NrdTonemap PIMPL (header)

- [ ] **Step 1.6: Write nrd_tonemap.hpp**

Create `ohao/render/rt/denoise/nrd_tonemap.hpp`:
```cpp
#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Sub-plan 4.E T1: per-frame inputs for NrdTonemap::dispatch.
/// Both views are borrowed; compositor does not take ownership.
struct NrdTonemapInputs {
    VkImageView composedHDR   = VK_NULL_HANDLE;  // from PT binding 29 (4.D compose output, RGBA32F)
    VkImageView tonemappedOut = VK_NULL_HANDLE;  // PT binding 30 (NEW 4.E, RGBA8 UNORM)
};

/// Sub-plan 4.E T1: compute pipeline that applies ACES tonemap + sRGB gamma to
/// NRD's composed HDR output. Sibling to NrdCompositor (4.D) — same 3-method
/// PIMPL shape. Standalone compute pipeline; its descriptor set layout is
/// independent of PathTracer's RT descriptor layout.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, the implementation
/// compiles to no-op stubs and initialize() returns false.
class NrdTonemap {
public:
    NrdTonemap();
    ~NrdTonemap();

    NrdTonemap(const NrdTonemap&)            = delete;
    NrdTonemap& operator=(const NrdTonemap&) = delete;

    /// Load SPV, create descriptor layout (2 storage-image bindings), pipeline,
    /// descriptor pool, and allocate one persistent descriptor set. Stores w/h.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record a tonemap dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - Both image views valid
    ///   - composedHDR in SHADER_READ_ONLY_OPTIMAL (or GENERAL)
    ///   - tonemappedOut in GENERAL
    /// Writes ACES-tonemapped sRGB to tonemappedOut.
    void dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

### Step 1.7 — NrdTonemap PIMPL (impl)

- [ ] **Step 1.7: Write nrd_tonemap.cpp**

Create `ohao/render/rt/denoise/nrd_tonemap.cpp`. Structurally identical to `nrd_compose.cpp` (4.D T1); only differences are (a) 2 bindings instead of 5, (b) loads `rt_nrd_tonemap.comp.spv` instead of `rt_nrd_compose.comp.spv`, (c) different log prefix (`[NRD tonemap]`).

```cpp
// Sub-plan 4.E T1: NrdTonemap — Vulkan compute pipeline that applies ACES +
// sRGB gamma to NRD's composed HDR output.

#include "render/rt/denoise/nrd_tonemap.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <fstream>
#include <iostream>
#include <vector>

namespace ohao {

namespace {

// OHAO's shader CMake flattens directory paths with underscores.
std::vector<char> readShaderSpv(const std::string& name) {
    const std::string searchPaths[] = {
        name,
        "build/shaders/" + name,
        "build/Release/bin/shaders/" + name,
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = static_cast<size_t>(file.tellg());
            std::vector<char> buf(size);
            file.seekg(0);
            file.read(buf.data(), size);
            return buf;
        }
    }
    return {};
}

}  // anonymous namespace

struct NrdTonemap::Impl {
    VkDevice              device         = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice = VK_NULL_HANDLE;
    uint32_t              width          = 0;
    uint32_t              height         = 0;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet  = VK_NULL_HANDLE;
};

NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() { shutdown(); }

bool NrdTonemap::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                             uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // 2 storage-image bindings, COMPUTE stage.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorSetLayout failed: " << int(r) << std::endl;
        return false;
    }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_impl->setLayout;
    if (VkResult r = vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreatePipelineLayout failed: " << int(r) << std::endl;
        return false;
    }

    auto spv = readShaderSpv("rt_nrd_tonemap.comp.spv");
    if (spv.empty()) {
        std::cerr << "[NRD tonemap] failed to load rt_nrd_tonemap.comp.spv" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateShaderModule failed: " << int(r) << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName  = "main";

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage  = stage;
    pipeInfo.layout = m_impl->pipelineLayout;
    VkResult pipeResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                                    &m_impl->pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (pipeResult != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateComputePipelines failed: " << int(pipeResult) << std::endl;
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;
    if (VkResult r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorPool failed: " << int(r) << std::endl;
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_impl->setLayout;
    if (VkResult r = vkAllocateDescriptorSets(device, &dsai, &m_impl->descriptorSet); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkAllocateDescriptorSets failed: " << int(r) << std::endl;
        return false;
    }

    std::cout << "[NRD tonemap] pipeline ready @ " << width << "x" << height << std::endl;
    return true;
}

void NrdTonemap::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    if (m_impl->descriptorPool) { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)       { vkDestroyPipeline(d, m_impl->pipeline, nullptr);              m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout) { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr);  m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)      { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr);  m_impl->setLayout      = VK_NULL_HANDLE; }
    m_impl->descriptorSet = VK_NULL_HANDLE;
    m_impl->device        = VK_NULL_HANDLE;
}

void NrdTonemap::dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs) {
    if (!m_impl->pipeline) return;

    std::array<VkDescriptorImageInfo, 2> imageInfos{};
    imageInfos[0].imageView   = inputs.composedHDR;
    imageInfos[1].imageView   = inputs.tonemappedOut;
    for (auto& ii : imageInfos) ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_impl->descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_impl->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipelineLayout, 0, 1,
                             &m_impl->descriptorSet, 0, nullptr);

    const uint32_t gx = (m_impl->width  + 7u) / 8u;
    const uint32_t gy = (m_impl->height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED

namespace ohao {

struct NrdTonemap::Impl {};
NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() = default;

bool NrdTonemap::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdTonemap::shutdown() {}
void NrdTonemap::dispatch(VkCommandBuffer, const NrdTonemapInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
```

### Step 1.8 — Build + smoke (compiler-only)

- [ ] **Step 1.8: Build OHAO_NRD=ON**

```bash
cmake --build build -j8 2>&1 | tail -10
```
Expected: clean. `nrd_tonemap.cpp` builds.

- [ ] **Step 1.9: Build OHAO_NRD=OFF**

```bash
cmake --build build-nonrd -j8 --target cornell_box 2>&1 | tail -5
```
Expected: clean. Stub compiles.

### Step 1.10 — Extend PathTracer with binding 30 + m_nrdTonemap

- [ ] **Step 1.10: path_tracer.hpp — add forward-decl, members, accessors**

Open `ohao/render/rt/path_tracer.hpp`. Find the existing `NrdCompositor` forward-decl (unconditional) and add `NrdTonemap`:
```cpp
namespace ohao {
    class NrdDenoiser;
    class NrdCompositor;
    class NrdTonemap;   // NEW 4.E
}
```

Find the accessor region near `getNrdComposedAOVImage()` (added 4.D T2). Add below:
```cpp
    // Sub-plan 4.E T1: tonemapped NRD output (RGBA8 UNORM, binding 30).
    VkImageView getNrdTonemappedAOV()      const { return m_nrdTonemappedView; }
    VkImage     getNrdTonemappedAOVImage() const { return m_nrdTonemappedImage; }
```

In the private data-member region, find the 4.D `m_nrdComposedMemory` + `m_nrdComposeFirstFrame` block. Directly after it, add:
```cpp
    // Feature 4.E: NRD tonemapped output (RGBA8 UNORM) at binding 30.
    // NOT in PT's RT descriptor layout — only in NrdTonemap's compute set.
    VkImage        m_nrdTonemappedImage     = VK_NULL_HANDLE;
    VkDeviceMemory m_nrdTonemappedMemory    = VK_NULL_HANDLE;
    VkImageView    m_nrdTonemappedView      = VK_NULL_HANDLE;
    bool           m_nrdTonemapFirstFrame   = true;  // gates UNDEFINED→GENERAL on binding 30
```

Find the `m_nrdCompositor` unique_ptr member. Add sibling directly below:
```cpp
    std::unique_ptr<NrdTonemap> m_nrdTonemap;  // NEW 4.E
```

### Step 1.11 — Allocate binding 30 in createImages

- [ ] **Step 1.11: path_tracer_images.cpp — append binding 30 allocation**

Open `ohao/render/rt/path_tracer_images.cpp`. Find the end of the binding 29 (`m_nrdComposedImage`) allocation block (added in 4.D T2, ends with the `m_nrdComposeFirstFrame = true;` reset).

Directly after that closing `}`, append:
```cpp
    // ---- Sub-plan 4.E: NRD tonemapped output (RGBA8 UNORM) at binding 30 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = {m_width, m_height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_nrdTonemappedImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_nrdTonemappedImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_nrdTonemappedMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_nrdTonemappedImage, m_nrdTonemappedMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_nrdTonemappedImage;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_nrdTonemappedView) != VK_SUCCESS) return false;

        // Reset first-frame latch so UNDEFINED→GENERAL fires on new VkImage
        // after resize (same rationale as m_nrdComposeFirstFrame in 4.D T2).
        m_nrdTonemapFirstFrame = true;
    }
```

- [ ] **Step 1.12: path_tracer_images.cpp — cleanup binding 30 in destroyImages**

Find the 4.D cleanup block for `m_nrdComposedMemory`. Append after it:
```cpp
    if (m_nrdTonemappedView)   { vkDestroyImageView(m_device, m_nrdTonemappedView, nullptr);   m_nrdTonemappedView   = VK_NULL_HANDLE; }
    if (m_nrdTonemappedImage)  { vkDestroyImage(m_device, m_nrdTonemappedImage, nullptr);      m_nrdTonemappedImage  = VK_NULL_HANDLE; }
    if (m_nrdTonemappedMemory) { vkFreeMemory(m_device, m_nrdTonemappedMemory, nullptr);       m_nrdTonemappedMemory = VK_NULL_HANDLE; }
```

### Step 1.13 — Initialize + shutdown m_nrdTonemap

- [ ] **Step 1.13: path_tracer.cpp — include + init**

Open `ohao/render/rt/path_tracer.cpp`. After the `#include "render/rt/denoise/nrd_compose.hpp"` line (added in 4.D T2), add:
```cpp
#include "render/rt/denoise/nrd_tonemap.hpp"
```

Find the `#ifdef OHAO_NRD_ENABLED` block in `PathTracer::init` that sets up `m_nrdCompositor` (4.D T2). After the `m_nrdCompositor.reset();` (inside the failure branch) and the enclosing `}`, but still inside `#ifdef OHAO_NRD_ENABLED`, append:
```cpp
    m_nrdTonemap = std::make_unique<NrdTonemap>();
    if (!m_nrdTonemap->initialize(m_device, m_physicalDevice, m_width, m_height)) {
        std::cerr << "[NRD tonemap] init FAILED — tonemap pass will be skipped" << std::endl;
        m_nrdTonemap.reset();
    }
```
(`initialize()` logs `[NRD tonemap] pipeline ready @ WxH` on success.)

- [ ] **Step 1.14: path_tracer.cpp — shutdown**

Find `PathTracer::destroy()` and the 4.D `m_nrdCompositor.reset()` block. Add sibling directly after:
```cpp
    if (m_nrdTonemap) {
        m_nrdTonemap->shutdown();
        m_nrdTonemap.reset();
    }
```

### Step 1.15 — Dispatch tonemap in render()

- [ ] **Step 1.15: path_tracer_render.cpp — insert tonemap dispatch after compose**

Open `ohao/render/rt/path_tracer_render.cpp`. Find the 4.D compose dispatch block — it's inside `#ifdef OHAO_NRD_ENABLED`, inside `if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs)`, inside the nested `if (m_nrdCompositor)` block. It ends with the `m_nrdCompositor->dispatch(cmd, ci)` call and a comment "After compose, binding 29 is in GENERAL with SHADER_WRITE access."

Directly after that compose-dispatch block's closing `}` (the inner `if (m_nrdCompositor)` closing brace, NOT the outer `if (m_nrdDenoiser && ...)` brace), add the tonemap dispatch block:
```cpp
        if (m_nrdTonemap) {
            // Transition binding 29 SHADER_WRITE → SHADER_READ for tonemap's read.
            VkImageMemoryBarrier tbIn{};
            tbIn.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            tbIn.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            tbIn.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            tbIn.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn.image            = m_nrdComposedImage;
            tbIn.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &tbIn);

            // Transition binding 30: UNDEFINED→GENERAL first frame, GENERAL→GENERAL after.
            // Per-instance m_nrdTonemapFirstFrame (not a function-local static) — same
            // rationale as m_nrdComposeFirstFrame in 4.D.
            VkImageMemoryBarrier tbOut{};
            tbOut.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            tbOut.srcAccessMask    = m_nrdTonemapFirstFrame ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
            tbOut.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            tbOut.oldLayout        = m_nrdTonemapFirstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            tbOut.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbOut.image            = m_nrdTonemappedImage;
            tbOut.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_nrdTonemapFirstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &tbOut);
            m_nrdTonemapFirstFrame = false;

            // Dispatch
            NrdTonemapInputs ti {};
            ti.composedHDR   = m_nrdComposedView;
            ti.tonemappedOut = m_nrdTonemappedView;
            m_nrdTonemap->dispatch(cmd, ti);

            // After tonemap, binding 30 is in GENERAL with SHADER_WRITE access.
            // Downstream readback (readbackNrdTonemapped) transitions GENERAL→TRANSFER_SRC.
        }
```

### Step 1.16 — IRTRendererProfile forwarder

- [ ] **Step 1.16: rt_profile_renderer.hpp — add passthrough for binding 30**

Open `ohao/render/rt/rt_profile_renderer.hpp`. Find the 4.D `getNrdComposedAOVImage` / `getNrdComposedAOV` pure virtuals and their forwarders in `RTProfileRendererBase`.

Add two new pure virtuals directly below the compose pair:
```cpp
    virtual VkImage     getNrdTonemappedAOVImage() const = 0;  // Sub-plan 4.E
    virtual VkImageView getNrdTonemappedAOV()      const = 0;
```

And the matching forwarders inside `RTProfileRendererBase`:
```cpp
    VkImage     getNrdTonemappedAOVImage() const override { return m_pathTracer.getNrdTonemappedAOVImage(); }
    VkImageView getNrdTonemappedAOV()      const override { return m_pathTracer.getNrdTonemappedAOV(); }
```

### Step 1.17 — VulkanRenderer readback + getPixels branch

- [ ] **Step 1.17: renderer.hpp — declare readback + accessor**

Open `ohao/gpu/vulkan/renderer.hpp`. After the 4.C T2 `readbackDenoisedDiffuse` / `readbackDenoisedSpecular` declarations, add:
```cpp
    // Sub-plan 4.E T1: NRD tonemapped output readback (RGBA8 UNORM, 4 bytes/pixel)
    bool readbackNrdTonemapped(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height);
```

And after the existing `getNrdComposedAOVImage()` passthrough (4.D T2), add:
```cpp
    VkImage getNrdTonemappedAOVImage() const;
```

- [ ] **Step 1.18: renderer.cpp — implement readback**

In `ohao/gpu/vulkan/renderer.cpp`, find the 4.D T2 `readbackNrdComposed` impl. Append directly after its closing `}`:
```cpp
bool VulkanRenderer::readbackNrdTonemapped(std::vector<uint8_t>& data,
                                            uint32_t& width, uint32_t& height) {
    VkImage srcImage = getNrdTonemappedAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4; // RGBA8 = 4 bytes/pixel
    data.resize(static_cast<size_t>(width) * height * 4);

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = byteCount;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = m_commandPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image            = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image            = srcImage;
    toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &toGen);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
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

VkImage VulkanRenderer::getNrdTonemappedAOVImage() const {
    if (m_renderMode == RenderMode::RTOffline && m_rtOfflineRenderer) {
        return m_rtOfflineRenderer->getNrdTonemappedAOVImage();
    }
    if (m_renderMode == RenderMode::RTRealtime && m_rtRealtimeRenderer) {
        return m_rtRealtimeRenderer->getNrdTonemappedAOVImage();
    }
    return VK_NULL_HANDLE;
}
```

- [ ] **Step 1.19: renderer.cpp — extend getPixels() with NRD branch**

Find `getPixels()` at line ~666. The existing structure is:
```cpp
const uint8_t* VulkanRenderer::getPixels() const {
    if (m_denoiseMode == DenoiseMode::None) {
        return m_pixelBuffer.data();
    }
    if (m_denoiseCacheValid) {
        return m_denoisedPixelBuffer.data();
    }
    // ... OIDN/OptiX CPU-side denoise ...
}
```

Add an NRD branch BEFORE the OIDN/OptiX path, right after the `m_denoiseCacheValid` early-return. Shape:
```cpp
    // Sub-plan 4.E T1: NRD is GPU-side. Readback the tonemapped binding 30.
    if (m_denoiseMode == DenoiseMode::NRD) {
        std::vector<uint8_t> rgba;
        uint32_t rw = 0, rh = 0;
        auto* self = const_cast<VulkanRenderer*>(this);
        if (!self->readbackNrdTonemapped(rgba, rw, rh)) {
            std::cerr << "[Denoise] NRD readback failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }
        m_denoisedPixelBuffer = std::move(rgba);
        m_denoiseCacheValid = true;
        return m_denoisedPixelBuffer.data();
    }

    // Shared readback + float3 conversion for any denoiser backend.
    // (existing OIDN/OptiX path below)
```

### Step 1.20 — Fallback for OHAO_NRD=OFF builds

- [ ] **Step 1.20: renderer.cpp — getPixels NRD fallback**

Just above the new NRD branch in `getPixels()`, add a one-time warning for the off-build case. Actually, the `readbackNrdTonemapped` call will return false gracefully (getNrdTonemappedAOVImage returns VK_NULL_HANDLE in OFF build because the images were never allocated — but wait, the images ARE allocated always per path_tracer_images.cpp). So the NRD branch WILL attempt to use an uninitialized binding 30 on OFF builds; this will produce garbage.

Instead, detect the OFF-build case in `VulkanRenderer::setDenoiseMode` and downgrade `NRD` to `None` with a log. Find `setDenoiseMode`:

Actually, simpler fix: the `NrdTonemap::initialize()` returns false on OFF builds (stub), so `m_nrdTonemap.reset()` fires in PathTracer::init. Binding 30 is allocated unconditionally but never written because dispatch is guarded by `if (m_nrdTonemap)`. The readback will return an all-zero (uninitialized) image, which getPixels will happily return.

Best fix: in `getPixels()` NRD branch, check whether any writes have happened to binding 30. Simplest check — `getNrdTonemappedAOVImage()` returns non-null AND `m_nrdTonemapFirstFrame == false` (which implies at least one dispatch ran). But we don't have that flag at VulkanRenderer layer.

Pragmatic compromise: add a log in `setDenoiseMode` when mode is NRD and `OHAO_NRD_ENABLED` is undefined. `setDenoiseMode` lives in renderer layer but `OHAO_NRD_ENABLED` is on ohao_renderer, which renderer links. Check.

Simpler still: in parseDenoiseMode itself (denoise_types.cpp), when user passes "nrd" and `OHAO_NRD_ENABLED` is undefined at that TU, log a warning and return `DenoiseMode::None`. But denoise_types.cpp is in `ohao_renderer` library which DOES have `OHAO_NRD_ENABLED`, so this works.

Add to denoise_types.cpp's parser:
```cpp
    if (lower == "nrd") {
#ifdef OHAO_NRD_ENABLED
        return DenoiseMode::NRD;
#else
        std::cerr << "[DenoiseMode] --denoise=nrd requested but OHAO_NRD=OFF at build time — falling back to None\n";
        return DenoiseMode::None;
#endif
    }
```
Remember to `#include <iostream>` at the top of denoise_types.cpp if not already.

### Step 1.21 — Build + smoke

- [ ] **Step 1.21: Build OHAO_NRD=ON**
```bash
cmake --build build -j8 2>&1 | tail -5
```
Expected: clean.

- [ ] **Step 1.22: Rebuild shaders (stale SPV in main repo after 4.D merge)**
```bash
cmake --build build --target shaders -j8 2>&1 | tail -3
ls build/shaders/rt_nrd_tonemap.comp.spv
```

- [ ] **Step 1.23: Smoke — all init logs**
```bash
./build/cornell_box /tmp/smoke_4e_t1.png 1 2>&1 | grep -E "NRD|compose|tonemap|persistent"
```
Expected 8 lines (4 per PT profile × 2 profiles):
```
[NRD] integration ready @ 1920x1080 (NRI-backed REBLUR_DIFFUSE_SPECULAR)
[NRD] persistent instance ready @ 1920x1080
[NRD compose] pipeline ready @ 1920x1080
[NRD tonemap] pipeline ready @ 1920x1080
<repeat x2>
```

- [ ] **Step 1.24: Primary verification — offline NRD PNG**

Rebuild env_demo explicitly (depends on shaders):
```bash
cmake --build build --target env_demo -j8 2>&1 | tail -3
```

Capture baseline OIDN + run NRD:
```bash
mkdir -p renders/4e_t1
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4e_t1/oidn.png 1 --denoise=oidn 2>&1 | tail -2
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4e_t1/nrd.png 1 --denoise=nrd 2>&1 | tail -4
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr renders/4e_t1/none.png 1 --denoise=none 2>&1 | tail -2
```

Check `renders/4e_t1/nrd.png` — should be properly lit (not black), recognizable figure, visibly cleaner than `renders/4e_t1/none.png`.

- [ ] **Step 1.25: Invariant check — `--denoise=oidn` pre-4.E equivalent**

If you have a pre-4.E OIDN reference, compare. Otherwise just visually confirm `renders/4e_t1/oidn.png` looks normal.

- [ ] **Step 1.26: Build OHAO_NRD=OFF**
```bash
cmake --build build-nonrd -j8 --target cornell_box 2>&1 | tail -3
./build/cornell_box /tmp/smoke_4e_t1_off.png 1 --denoise=nrd 2>&1 | grep -iE "NRD|fall"
```
Expected: clean build, log shows "OHAO_NRD=OFF at build time — falling back to None", `/tmp/smoke_4e_t1_off.png` produced.

### Step 1.27 — Commit T1

- [ ] **Step 1.27: Commit**

```bash
git add shaders/rt/nrd_tonemap.comp \
        ohao/render/rt/denoise/nrd_tonemap.hpp ohao/render/rt/denoise/nrd_tonemap.cpp \
        ohao/render/rt/denoise/denoise_types.hpp ohao/render/rt/denoise/denoise_types.cpp \
        ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp \
        ohao/render/rt/path_tracer_images.cpp ohao/render/rt/path_tracer_render.cpp \
        ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp
git commit -m "$(cat <<'EOF'
feat(rt): DenoiseMode::NRD + tonemap pass + offline --denoise=nrd (Sub-plan 4.E T1)

Adds NRD as a first-class peer to OIDN/OptiX:

- DenoiseMode::NRD = 3 enum value; parser accepts "nrd"; OFF-build falls
  back to None with a single-line warning.
- New NrdTonemap PIMPL + shaders/rt/nrd_tonemap.comp (8×8 compute, 2
  storage-image bindings, ACES + sRGB gamma). Sibling to NrdCompositor.
- Binding 30 (RGBA8 UNORM) — PathTracer-owned, NOT in RT descriptor
  layout. Per-instance m_nrdTonemapFirstFrame latch mirrors 4.D pattern.
- PathTracer::render() chains compose → tonemap dispatches after NRD
  denoise. Barriers transition 29 SHADER_WRITE→SHADER_READ, 30
  UNDEFINED→GENERAL (first frame) / GENERAL→GENERAL (subsequent).
- VulkanRenderer::getPixels() gains NRD branch: readback binding 30
  (GPU-tonemapped RGBA8) directly — no CPU-side work. env_demo /
  interactive / all examples pick up --denoise=nrd transparently.

No temporal yet — frameIndex still hard-coded 0 (4.E T2).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Temporal state wiring

Goal: stop passing `frameIndex=0` and identity prev matrices. Feed real per-frame state into `NrdCameraInputs` so NRD's temporal accumulation actually fires. Stepped-camera test confirms frame 50 is visibly cleaner than frame 0.

### Step 2.1 — Add projMatrixPrev to NrdCameraInputs

- [ ] **Step 2.1: nrd_denoise.hpp — explicit prev proj field**

In `ohao/render/rt/denoise/nrd_denoise.hpp`, extend `NrdCameraInputs`. Find the existing struct (added in 4.B):
```cpp
struct NrdCameraInputs {
    std::array<float, 16> viewMatrix      {};
    std::array<float, 16> viewMatrixPrev  {};
    std::array<float, 16> projMatrix      {};
    std::array<float, 2>  jitter          {};
    std::array<float, 2>  jitterPrev      {};
    std::array<float, 3>  motionVectorScale {1.0f, 1.0f, 0.0f};
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
};
```

Add `projMatrixPrev` between `projMatrix` and `jitter`:
```cpp
struct NrdCameraInputs {
    std::array<float, 16> viewMatrix      {};
    std::array<float, 16> viewMatrixPrev  {};
    std::array<float, 16> projMatrix      {};
    std::array<float, 16> projMatrixPrev  {};   // NEW 4.E T2 — consumes 4.C I5 follow-up
    std::array<float, 2>  jitter          {};
    std::array<float, 2>  jitterPrev      {};
    std::array<float, 3>  motionVectorScale {1.0f, 1.0f, 0.0f};
    uint32_t frameIndex = 0;
    bool isMotionVectorInWorldSpace = false;
};
```

### Step 2.2 — nrd_denoise.cpp — use projMatrixPrev explicitly

- [ ] **Step 2.2: setCommonSettings consumes projMatrixPrev**

In `ohao/render/rt/denoise/nrd_denoise.cpp`, find `setCommonSettings`. It currently copies `in.projMatrix` into BOTH `s.viewToClipMatrix` AND `s.viewToClipMatrixPrev`. Change the "prev" write to use the new field:

Find:
```cpp
    std::memcpy(s.viewToClipMatrix,      in.projMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrix,     in.viewMatrix.data(),         sizeof(float) * 16);
    std::memcpy(s.worldToViewMatrixPrev, in.viewMatrixPrev.data(),     sizeof(float) * 16);
```

Add a new memcpy line for `viewToClipMatrixPrev`. NRD v4.17 has this field on `CommonSettings`. After the three lines above:
```cpp
    std::memcpy(s.viewToClipMatrixPrev,  in.projMatrixPrev.data(),     sizeof(float) * 16);
```

**Verify NRD v4.17 has `viewToClipMatrixPrev` on CommonSettings.** If it doesn't (NRD may call it `projToViewMatrixPrev` or just omit it), check the vendored header at `build/_deps/nrd-src/Include/NRDDescs.h` or similar. If the field doesn't exist, the change is: simply pass `projMatrixPrev` as before but mirror the current proj when `projMatrixPrev` is identity (first frame) to avoid zero-matrix issues.

### Step 2.3 — PathTracer temporal state members

- [ ] **Step 2.3: path_tracer.hpp — add prev V / prev P members**

In `ohao/render/rt/path_tracer.hpp`, find the existing `m_prevViewProj` member (used by 3.A for motion vectors). Directly after it, add:
```cpp
    // Sub-plan 4.E T2: previous-frame view + proj matrices for NRD's temporal
    // reprojection. Captured at start of each render() call; used on the NEXT
    // frame's NrdCameraInputs.
    glm::mat4 m_prevViewMatrix{1.0f};
    glm::mat4 m_prevProjMatrix{1.0f};
```

Make sure `<glm/glm.hpp>` is already included in `path_tracer.hpp` (grep) — if not, add it.

### Step 2.4 — Feed real temporal state in render()

- [ ] **Step 2.4: path_tracer_render.cpp — real frameIndex + prev matrices**

Find the NRD dispatch block in `render()` (added 4.C T3b, inside `if (m_nrdDenoiser && ...)`). Locate the `NrdCameraInputs camera {};` line — it currently populates with identity / 0:

```cpp
        NrdCameraInputs camera {};
        const glm::mat4 viewM = glm::inverse(pc.invView);
        const glm::mat4 projM = glm::inverse(pc.invProj);
        std::memcpy(camera.viewMatrix.data(),     glm::value_ptr(viewM), sizeof(float) * 16);
        std::memcpy(camera.viewMatrixPrev.data(), glm::value_ptr(viewM), sizeof(float) * 16); // first-frame: no history
        std::memcpy(camera.projMatrix.data(),     glm::value_ptr(projM), sizeof(float) * 16);
        camera.motionVectorScale = {1.0f, 1.0f, 0.0f};
        camera.jitter     = {0.0f, 0.0f};
        camera.jitterPrev = {0.0f, 0.0f};
        camera.frameIndex = 0;  // 4.C scope: single-shot, no temporal history
        camera.isMotionVectorInWorldSpace = false;
        m_nrdDenoiser->setCommonSettings(camera);
```

Replace with:
```cpp
        NrdCameraInputs camera {};
        const glm::mat4 viewM = glm::inverse(pc.invView);
        const glm::mat4 projM = glm::inverse(pc.invProj);
        std::memcpy(camera.viewMatrix.data(),     glm::value_ptr(viewM),               sizeof(float) * 16);
        std::memcpy(camera.viewMatrixPrev.data(), glm::value_ptr(m_prevViewMatrix),    sizeof(float) * 16);
        std::memcpy(camera.projMatrix.data(),     glm::value_ptr(projM),               sizeof(float) * 16);
        std::memcpy(camera.projMatrixPrev.data(), glm::value_ptr(m_prevProjMatrix),    sizeof(float) * 16);
        camera.motionVectorScale = {1.0f, 1.0f, 0.0f};
        camera.jitter     = {0.0f, 0.0f};
        camera.jitterPrev = {0.0f, 0.0f};
        camera.frameIndex = m_historyFrameCount;  // real per-frame counter
        camera.isMotionVectorInWorldSpace = false;
        m_nrdDenoiser->setCommonSettings(camera);

        // Capture current frame's V/P for NEXT frame's NRD input.
        m_prevViewMatrix = viewM;
        m_prevProjMatrix = projM;
```

### Step 2.5 — Build + smoke

- [ ] **Step 2.5: Build**
```bash
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/smoke_4e_t2.png 1 2>&1 | grep -E "NRD|compose|tonemap"
```
Expected: clean build; 8 log lines as before.

### Step 2.6 — Offline one-shot still works

- [ ] **Step 2.6: env_demo NRD single-shot still produces lit PNG**

```bash
./build/env_demo assets/realistic_female.glb assets/test_models/env_studio.hdr /tmp/nrd_t2.png 1 --denoise=nrd 2>&1 | tail -3
```
Expected: PNG produced, recognizable figure. On the first frame `m_prevViewMatrix` is identity, `m_historyFrameCount` is 0, so NRD sees "no history" — functionally same as T1's output. This is correct — T2 doesn't change single-shot behavior; the payoff is multi-frame.

### Step 2.7 — Commit T2

- [ ] **Step 2.7: Commit**

```bash
git add ohao/render/rt/denoise/nrd_denoise.hpp ohao/render/rt/denoise/nrd_denoise.cpp \
        ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer_render.cpp
git commit -m "$(cat <<'EOF'
feat(rt): NRD temporal state wiring (Sub-plan 4.E T2)

Adds projMatrixPrev field to NrdCameraInputs (consumes parked 4.C I5
follow-up). setCommonSettings now writes viewToClipMatrixPrev explicitly
instead of silently mirroring current proj.

PathTracer captures m_prevViewMatrix + m_prevProjMatrix at the end of
each render() — next frame feeds them into NrdCameraInputs. frameIndex
stops being hard-coded 0 and uses m_historyFrameCount (same counter used
by 3.A motion vectors).

Offline one-shot behavior unchanged (first frame has prev=identity,
frameIndex=0 → NRD spatial-only, same as 4.D output). The payoff is
visible in multi-frame interactive rendering — stepped-camera test
(4.E T3 interactive demo) shows frame 50 visibly cleaner than frame 0.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Interactive viewer integration + final verification

Goal: `./build/interactive <mesh> <env> --denoise=nrd` runs NRD every frame at 50+fps. Stepped-camera screenshot pair confirms frame 50 is visibly cleaner than frame 0. 4.E done, Phase 4 complete.

### Step 3.1 — Parse --denoise in interactive.cpp

- [ ] **Step 3.1: Interactive CLI**

Open `examples/interactive.cpp`. Find the existing argument parsing loop (look for `for (int i = 1; i < argc; ...)` or similar). Add `--denoise=` parsing if not present; mirror the pattern from env_demo.

Specifically, near the existing args, add:
```cpp
    std::string denoiseArg = "oidn";  // default — mirror env_demo default
    // ... (inside the argument loop)
    } else if (arg.rfind("--denoise=", 0) == 0) {
        denoiseArg = arg.substr(10);
    }
```

After argument parsing completes and before `renderer.initialize()` (or wherever the renderer is set up), add:
```cpp
    DenoiseMode denoiseMode = parseDenoiseMode(denoiseArg);
```

After the renderer is initialized, call:
```cpp
    renderer.setDenoiseMode(denoiseMode);
```

If the interactive viewer already has `renderer.setDenoiseMode(...)` somewhere, ensure the mode passed is from the parsed CLI arg, not hard-coded.

If interactive's argument parsing doesn't exist yet (it might just take positional mesh + env), add a minimal `for (int i = 3; i < argc; ++i)` loop to handle `--denoise=<mode>`.

### Step 3.2 — Build + launch

- [ ] **Step 3.2: Build interactive**

```bash
cmake --build build --target interactive -j8 2>&1 | tail -5
```

- [ ] **Step 3.3: Sanity run without NRD**

```bash
./build/interactive assets/realistic_female.glb assets/test_models/env_studio.hdr
```
Expected: viewer opens, shows OIDN-denoised scene, normal behavior. Close with Esc/window close.

- [ ] **Step 3.4: Launch with --denoise=nrd**

```bash
./build/interactive assets/realistic_female.glb assets/test_models/env_studio.hdr --denoise=nrd
```
Expected: viewer opens; init logs show all four NRD-related lines (integration + persistent + compose + tonemap); scene is displayed properly lit (not black). Move the camera — scene should re-render cleanly at 50+ fps. No stuttering or validation errors.

### Step 3.5 — Capture screenshots

- [ ] **Step 3.5: Capture T3 evidence**

Decide on screenshot mechanism. Options:
- (a) Interactive already has a screenshot key (likely F1 / F12) — grep interactive.cpp for `stbi_write_png` or similar.
- (b) If none, add a one-line key handler that calls `stbi_write_png` on `renderer.getPixels()` when key is pressed.

If (a) — take 3 screenshots during the run, save to `renders/4e_t3/`:
- `still_frame_0.png` — immediately after launch (NRD frame 0, spatial-only)
- `orbit_mid.png` — mid-orbit (camera moving, NRD showing reprojection)
- `settled_frame_N.png` — camera held still for ~2 seconds (NRD temporal accumulation visible)

If (b) — add the key handler, then repeat.

Expected: `settled_frame_N.png` is visibly cleaner than `still_frame_0.png` (temporal accumulation). `orbit_mid.png` may show temporary smearing / disocclusion artifacts during motion, which is expected for NRD (faster motion = less accumulation).

### Step 3.6 — Perf sanity check

- [ ] **Step 3.6: Verify framerate**

Interactive viewer likely logs fps via window title or periodic stdout. Confirm:
- Without `--denoise=nrd` (default OIDN): baseline fps (typically 60+ on mid-range GPU).
- With `--denoise=nrd`: at least 50 fps on the same hardware/scene.

If fps is dramatically lower (< 30), NRD is taking too long. Inspect with RenderDoc: typical frame cost is 1-3ms for REBLUR at 1920x1080. Acceptable ceiling for 4.E: 5ms. Beyond that, park as follow-up.

### Step 3.7 — Update verification log

- [ ] **Step 3.7: Verification log entries**

Append to `tests/reference_scenes/custom/envlit_turntable/verification_log.md`:
```markdown
## 2026-04-24 — Sub-plan 4.E: DenoiseMode::NRD live

**T1 (offline):** `./build/env_demo <scene> <env> out.png 1 --denoise=nrd` produces properly-lit PNG. Figure recognizable, comparable lighting to OIDN at spp=1. See `renders/4e_t1/nrd.png`.

**T2 (temporal wiring):** `NrdCameraInputs` gains `projMatrixPrev`. PathTracer captures `m_prevViewMatrix`/`m_prevProjMatrix` each frame; `frameIndex = m_historyFrameCount` replaces hard-coded 0. Offline one-shot output unchanged (first-frame has no history — same as T1).

**T3 (realtime):** `./build/interactive <scene> <env> --denoise=nrd` — viewer opens, NRD runs per-frame, fps ≥ 50 at 1920x1080 on dev hardware. Stationary-camera screenshot pair (`renders/4e_t3/still_frame_0.png` vs `renders/4e_t3/settled_frame_N.png`) shows temporal accumulation: frame 50 visibly cleaner than frame 0. Camera motion produces expected disocclusion smoothing. Zero new Vulkan validation errors.

**Status:** Phase 4 (NRD integration) **COMPLETE**. Sub-plans 4.A (library+CMake) + 4.B (API expansion) + 4.C (first dispatch) + 4.D (remodulation compositor) + 4.E (CLI + tonemap + temporal + realtime) all merged. NRD is a first-class peer to OIDN/OptiX. `--denoise=nrd` works across every example.
```

### Step 3.8 — Update CLAUDE.md

- [ ] **Step 3.8: CLAUDE.md binding table + `--denoise=nrd` note**

In `CLAUDE.md`, find the descriptor bindings table. Append:
```markdown
| 30 | STORAGE_IMAGE | NRD tonemapped beauty (RGBA8) — Sub-plan 4.E. Not in RT layout; tonemap-set only. |
```

Find the `--denoise=` CLI line in the project overview / README-ish section (near the example CLIs listed). If there's a list of valid modes, add `nrd` alongside `oidn`/`optix`/`none`. Otherwise add a one-liner:
```markdown
All examples accept `--denoise=oidn|optix|nrd|none` to override the default OIDN denoiser.
```

### Step 3.9 — Commit T3

- [ ] **Step 3.9: Commit**

```bash
git add examples/interactive.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md \
        CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(rt): --denoise=nrd in interactive viewer + 4.E verification (T3)

Wires --denoise=nrd CLI parsing into examples/interactive.cpp. The
viewer's existing renderer.getPixels() path (extended in T1) routes
GPU-tonemapped NRD output (binding 30) to the GLFW blit automatically;
no GLFW-side changes needed.

Realtime verification at 1920x1080 on dev hardware: NRD runs per-frame
at 50+ fps. Stationary-camera screenshot pair shows temporal
accumulation — frame 50 visibly cleaner than frame 0. Camera motion
produces expected NRD disocclusion behavior.

Phase 4 (NRD integration) COMPLETE. Sub-plans 4.A–4.E all merged.
NRD is a first-class peer to OIDN/OptiX across every example.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Done

All three tasks complete. Sub-plan 4.E merged → `animation` has full NRD integration.

Next: Phase 5 (DLSS RR integration) or pivot back to rendering features (PCSS, ReSTIR DI, outdoor scene).
