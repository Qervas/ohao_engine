# Denoiser Sub-plan 4.D — Remodulation Compositor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Composite NRD's denoised diffuse (binding 27) + specular (binding 28) with demod albedo (binding 24) + spec-color (binding 25) → HDR color at new binding 29. `env_demo --dump-nrd-composed=<path>` dumps it for visual verification.

**Architecture:** A new `NrdCompositor` PIMPL class (sibling to `NrdDenoiser`) owns a standalone compute pipeline with its own 5-binding descriptor set layout. A minimal GLSL compute shader (`shaders/rt/nrd_compose.comp`) multiplies denoised radiance × demod albedo/color per pixel. `PathTracer::render()` dispatches compose after NRD denoise. Binding 29 is PathTracer-owned but NOT in PT's RT descriptor layout — only compose's compute layout references it.

**Tech Stack:** Vulkan 1.3 compute, GLSL 460, CMake FetchContent (no new deps), existing `shaders/CMakeLists.txt` auto-picks up new `.comp` via GLOB_RECURSE.

**Spec:** `docs/superpowers/specs/2026-04-24-denoiser-subplan4d-remodulation-compositor-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `shaders/rt/nrd_compose.comp` (NEW) | GLSL compute: reads 4 input images, writes 1 output. Zero branching, 6 `imageLoad`s + 1 `imageStore`. |
| `ohao/render/rt/denoise/nrd_compose.hpp` (NEW) | Public API: `NrdCompositor` class + `NrdComposeInputs` struct. |
| `ohao/render/rt/denoise/nrd_compose.cpp` (NEW) | PIMPL impl: SPV load, descriptor layout, pipeline, pool, dispatch. OFF-branch stub. |
| `ohao/render/rt/path_tracer.hpp` (modify) | Unconditional `m_nrdCompositor` + `m_nrdComposedImage/View/Memory`, accessors, forward-decl. |
| `ohao/render/rt/path_tracer.cpp` (modify) | Binding 29 image creation + cleanup + UNDEFINED→GENERAL barrier. `m_nrdCompositor` init in `init()`, shutdown in `destroy()`. Dispatch in `render()` after NRD denoise with input-layout transitions. |
| `ohao/render/rt/rt_profile_renderer.hpp` (modify) | `getNrdComposedAOV()` + `getNrdComposedAOVImage()` pure virtuals + forwarders. |
| `ohao/gpu/vulkan/renderer.hpp` (modify) | `readbackNrdComposed(data, w, h)` + `getNrdComposedAOVImage()` passthrough. |
| `ohao/gpu/vulkan/renderer.cpp` (modify) | `readbackNrdComposed` impl (mirror of `readbackDenoisedDiffuse`). |
| `examples/env_demo.cpp` (modify) | `--dump-nrd-composed=<path>` CLI + readback + `dumpRGBA32FStream` dump. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` (modify) | 4.D entry. |
| `CLAUDE.md` (modify) | Add binding 29 row. |

Invariants across both tasks:
1. `out.png` (binding 2 tonemapped beauty) bit-identical to pre-4.D at spp=1 + Sobol (PT is deterministic on that path).
2. `-DOHAO_NRD=OFF` builds + runs identically to pre-4.D OFF build.
3. `m_nrdCompositor` declared UNCONDITIONALLY (outside `#ifdef OHAO_NRD_ENABLED`) to avoid the ABI-mismatch trap discovered in 4.C T2. OFF-branch stub in `nrd_compose.cpp` provides an empty impl.

---

## Task 1: NrdCompositor PIMPL + compute shader

Goal: the compute pipeline is buildable and linkable in isolation. `NrdCompositor::initialize()` creates shader + descriptor layout + pipeline + pool. `dispatch()` exists and records a real dispatch but has no caller yet. Build clean under both `OHAO_NRD=ON` and `OHAO_NRD=OFF`. No PathTracer wiring in this task.

### Step 1.1 — Create the compute shader

- [ ] **Step 1.1: Write `shaders/rt/nrd_compose.comp`**

```glsl
#version 460

// Sub-plan 4.D: remodulation compositor.
// Combines NRD's denoised diffuse + specular radiance with demodulated
// albedo + specular-color AOVs to produce HDR beauty:
//   composed = denoisedDiff.rgb * diffAlbedo.rgb
//            + denoisedSpec.rgb * specColor.rgb
// Alpha of denoised radiance (hit-distance) is dropped.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly  image2D inDiffRad;     // denoised diffuse  (PT 27)
layout(set = 0, binding = 1, rgba32f) uniform readonly  image2D inSpecRad;     // denoised specular (PT 28)
layout(set = 0, binding = 2, rgba8)   uniform readonly  image2D inDiffAlbedo;  // demod albedo      (PT 24)
layout(set = 0, binding = 3, rgba8)   uniform readonly  image2D inSpecColor;   // demod F0/color    (PT 25)
layout(set = 0, binding = 4, rgba32f) uniform writeonly image2D outComposed;   // new binding       (PT 29)

void main() {
    ivec2 p    = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outComposed);
    if (p.x >= size.x || p.y >= size.y) return;

    vec3 diffRad    = imageLoad(inDiffRad,    p).rgb;
    vec3 specRad    = imageLoad(inSpecRad,    p).rgb;
    vec3 diffAlbedo = imageLoad(inDiffAlbedo, p).rgb;
    vec3 specColor  = imageLoad(inSpecColor,  p).rgb;

    vec3 composed = diffRad * diffAlbedo + specRad * specColor;

    imageStore(outComposed, p, vec4(composed, 1.0));
}
```

- [ ] **Step 1.2: Confirm CMake auto-picks up the new shader**

Run:
```bash
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | grep -i "nrd_compose"
```
Expected: at least one line referencing `nrd_compose.comp.spv` (the shader build rule fires). If no output, the GLOB_RECURSE in `shaders/CMakeLists.txt` didn't pick it up — verify the file is under `shaders/rt/` and has extension `.comp`.

Then build shaders:
```bash
cmake --build build --target shaders -j8 2>&1 | tail -5
```
Expected: `nrd_compose.comp.spv` produced in `build/shaders/` (or wherever the shader build outputs). No glslc errors.

### Step 1.3 — Declare the public API

- [ ] **Step 1.3: Write `ohao/render/rt/denoise/nrd_compose.hpp`**

```cpp
#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Per-frame inputs for NrdCompositor::dispatch. All views are borrowed;
/// the compositor does not take ownership. Views must be valid + resident
/// until dispatch() returns.
struct NrdComposeInputs {
    VkImageView diffRadiance = VK_NULL_HANDLE;  // from PT binding 27 (NRD denoised diffuse)
    VkImageView specRadiance = VK_NULL_HANDLE;  // from PT binding 28 (NRD denoised specular)
    VkImageView diffAlbedo   = VK_NULL_HANDLE;  // from PT binding 24 (3.C.6 demod albedo)
    VkImageView specColor    = VK_NULL_HANDLE;  // from PT binding 25 (3.C.6 demod F0)
    VkImageView composedOut  = VK_NULL_HANDLE;  // PT binding 29 (4.D output)
};

/// Sub-plan 4.D: compute pipeline that remodulates NRD's denoised radiance
/// with demod albedo+F0 to produce HDR beauty.
///
/// Standalone compute pipeline — its descriptor set layout is independent
/// of PathTracer's RT descriptor layout. The bindings 24/25/27/28/29 that
/// this pipeline consumes live in their own set.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, the implementation
/// compiles to no-op stubs and initialize() returns false.
class NrdCompositor {
public:
    NrdCompositor();
    ~NrdCompositor();

    NrdCompositor(const NrdCompositor&)            = delete;
    NrdCompositor& operator=(const NrdCompositor&) = delete;

    /// Load SPV, create descriptor set layout, pipeline layout, compute
    /// pipeline, descriptor pool, and allocate one persistent descriptor set.
    /// Stores w/h for dispatch-size calculation. Returns false on failure.
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t width, uint32_t height);

    /// Destroy all Vulkan objects. Safe to call multiple times.
    void shutdown();

    /// Record a compose dispatch onto `cmd`. Preconditions:
    ///   - initialize() succeeded
    ///   - All 5 image views in `inputs` are valid
    ///   - Input images (diff/spec radiance, albedo, F0) in SHADER_READ_ONLY_OPTIMAL
    ///   - Output image in GENERAL
    /// Writes composed HDR (RGBA32F) to outputs.composedOut.
    void dispatch(VkCommandBuffer cmd, const NrdComposeInputs& inputs);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

### Step 1.4 — Write the PIMPL impl

- [ ] **Step 1.4: Write `ohao/render/rt/denoise/nrd_compose.cpp`**

```cpp
// Sub-plan 4.D: NrdCompositor — Vulkan compute pipeline that remodulates
// NRD's denoised radiance with demod albedo+F0.

#include "render/rt/denoise/nrd_compose.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <fstream>
#include <iostream>
#include <vector>

namespace ohao {

namespace {

// Matches PathTracer's readFile helper: tries the raw path first, then
// common build-output locations. Shaders/CMakeLists.txt drops SPVs under
// build/shaders/, but some configurations use build/Release/bin/shaders/.
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

struct NrdCompositor::Impl {
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

NrdCompositor::NrdCompositor()  : m_impl(std::make_unique<Impl>()) {}
NrdCompositor::~NrdCompositor() { shutdown(); }

bool NrdCompositor::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // 1. Descriptor set layout: 5 storage-image bindings, all COMPUTE stage.
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
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
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout) != VK_SUCCESS) {
        std::cerr << "[NRD compose] vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    // 2. Pipeline layout — no push constants.
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_impl->setLayout;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[NRD compose] vkCreatePipelineLayout failed\n";
        return false;
    }

    // 3. Load SPV and create shader module.
    auto spv = readShaderSpv("nrd_compose.comp.spv");
    if (spv.empty()) {
        std::cerr << "[NRD compose] failed to load nrd_compose.comp.spv\n";
        return false;
    }
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[NRD compose] vkCreateShaderModule failed\n";
        return false;
    }

    // 4. Compute pipeline.
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
        std::cerr << "[NRD compose] vkCreateComputePipelines failed: " << int(pipeResult) << "\n";
        return false;
    }

    // 5. Descriptor pool (one set, 5 storage images).
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 5;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool) != VK_SUCCESS) {
        std::cerr << "[NRD compose] vkCreateDescriptorPool failed\n";
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_impl->setLayout;
    if (vkAllocateDescriptorSets(device, &dsai, &m_impl->descriptorSet) != VK_SUCCESS) {
        std::cerr << "[NRD compose] vkAllocateDescriptorSets failed\n";
        return false;
    }

    std::cout << "[NRD compose] pipeline ready @ " << width << "x" << height << std::endl;
    return true;
}

void NrdCompositor::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    if (m_impl->descriptorPool) { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)       { vkDestroyPipeline(d, m_impl->pipeline, nullptr);              m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout) { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr);  m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)      { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr);  m_impl->setLayout      = VK_NULL_HANDLE; }
    m_impl->descriptorSet = VK_NULL_HANDLE;
    m_impl->device        = VK_NULL_HANDLE;
}

void NrdCompositor::dispatch(VkCommandBuffer cmd, const NrdComposeInputs& inputs) {
    if (!m_impl->pipeline) return;

    // Bind 5 image views into the descriptor set.
    std::array<VkDescriptorImageInfo, 5> imageInfos{};
    imageInfos[0].imageView   = inputs.diffRadiance;
    imageInfos[1].imageView   = inputs.specRadiance;
    imageInfos[2].imageView   = inputs.diffAlbedo;
    imageInfos[3].imageView   = inputs.specColor;
    imageInfos[4].imageView   = inputs.composedOut;
    for (auto& ii : imageInfos) ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_impl->descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_impl->device, static_cast<uint32_t>(writes.size()), writes.data(),
                            0, nullptr);

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

struct NrdCompositor::Impl {};
NrdCompositor::NrdCompositor()  : m_impl(std::make_unique<Impl>()) {}
NrdCompositor::~NrdCompositor() = default;

bool NrdCompositor::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdCompositor::shutdown() {}
void NrdCompositor::dispatch(VkCommandBuffer, const NrdComposeInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
```

### Step 1.5 — Build both configs

- [ ] **Step 1.5: Build OHAO_NRD=ON**

Run:
```bash
cmake --build build -j8 2>&1 | tail -20
```
Expected: clean build. `ohao_renderer` compiles `nrd_compose.cpp` and links successfully. No new warnings.

- [ ] **Step 1.6: Build OHAO_NRD=OFF**

Run:
```bash
cmake --build build-nonrd -j8 2>&1 | tail -10
```
Expected: clean build. OFF branch stub compiles.

### Step 1.7 — Commit

- [ ] **Step 1.7: Commit T1**

```bash
git add shaders/rt/nrd_compose.comp ohao/render/rt/denoise/nrd_compose.hpp ohao/render/rt/denoise/nrd_compose.cpp
git commit -m "$(cat <<'EOF'
feat(rt): NrdCompositor PIMPL + nrd_compose.comp shader (Sub-plan 4.D T1)

Standalone compute pipeline that will remodulate NRD's denoised radiance
with demod albedo+F0 AOVs. Its descriptor set layout (5 storage-image
bindings, all COMPUTE stage) is independent of PathTracer's RT layout.

No PathTracer wiring yet — T2 allocates binding 29 and calls
m_nrdCompositor->dispatch() from render() after NRD denoise.

OHAO_NRD=OFF stub impl keeps the link clean when NRD is disabled.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: PathTracer wiring + env_demo dump + verify

Goal: binding 29 lives in PathTracer as a PT-owned image. `m_nrdCompositor` is a persistent member initialized alongside `m_nrdDenoiser`. `PathTracer::render()` dispatches compose after NRD denoise, with correct barriers on bindings 24/25/27/28 (GENERAL↔SHADER_READ) and 29 (UNDEFINED→GENERAL). env_demo gets `--dump-nrd-composed=` and dumps the HDR output. Visual verification: composed PNG at spp=1 shows recognizable object colors and is dramatically cleaner than raw PT beauty at spp=1.

### Step 2.0 — Capture pre-T2 beauty baseline

- [ ] **Step 2.0: Beauty baseline sha256 at spp=1**

Run:
```bash
./build/cornell_box /tmp/beauty_pre_t2_4d.png 1 2>&1 | tail -2
sha256sum /tmp/beauty_pre_t2_4d.png
```
Record the hash. After T2, `cornell_box … 1` MUST produce the same hash (beauty-untouched invariant).

### Step 2.1 — Add binding 29 image/view/memory + accessors (path_tracer.hpp)

- [ ] **Step 2.1: Add data members**

Open `ohao/render/rt/path_tracer.hpp`. Directly after the `m_outSpecRadianceMemory` member added in 4.C T2 (around line 286), add:
```cpp
    // Feature 4.D: NRD composed HDR output (RGBA32F) at binding 29.
    // NOT in PathTracer's RT descriptor layout — only in NrdCompositor's
    // compute descriptor set.
    VkImage        m_nrdComposedImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_nrdComposedMemory = VK_NULL_HANDLE;
    VkImageView    m_nrdComposedView   = VK_NULL_HANDLE;
    bool           m_nrdComposeFirstFrame = true;  // gates UNDEFINED→GENERAL transition on binding 29
```

Find the forward-declaration block for `NrdDenoiser` (added unconditional in 4.C T2 at the top of the file) and add `NrdCompositor` alongside:
```cpp
namespace ohao {
    class NrdDenoiser;
    class NrdCompositor;  // NEW 4.D
}
```

Find the `m_nrdDenoiser` unique_ptr member (unconditional after 4.C T2) and add a sibling member right below:
```cpp
    std::unique_ptr<NrdDenoiser>   m_nrdDenoiser;
    std::unique_ptr<NrdCompositor> m_nrdCompositor;  // NEW 4.D
```

Add accessors. After the 4.C T2 `getOutSpecRadianceAOVImage()` entry (around line 127), add:
```cpp
    VkImageView getNrdComposedAOV()      const { return m_nrdComposedView; }
    VkImage     getNrdComposedAOVImage() const { return m_nrdComposedImage; }
```

### Step 2.2 — Add `#include` for nrd_compose.hpp in path_tracer.cpp

- [ ] **Step 2.2: Include the compositor header**

In `ohao/render/rt/path_tracer.cpp`, find the existing `#include "render/rt/denoise/nrd_denoise.hpp"` line (unconditional since 4.C T2) and add right below:
```cpp
#include "render/rt/denoise/nrd_compose.hpp"
```

Unconditional — the OFF stub in `nrd_compose.cpp` means consumers always link cleanly.

### Step 2.3 — Allocate binding 29 image in createImages()

- [ ] **Step 2.3: Append binding 29 allocation**

In `ohao/render/rt/path_tracer.cpp`, find the end of the binding 28 image allocation block (added in 4.C T2, ends with `vkCreateImageView(..., &m_outSpecRadianceView)`). Directly after its closing `}`, append:

```cpp
    // ---- Sub-plan 4.D: NRD composed HDR output (RGBA32F) at binding 29 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent        = {m_width, m_height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_nrdComposedImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_nrdComposedImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_nrdComposedMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_nrdComposedImage, m_nrdComposedMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_nrdComposedImage;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_nrdComposedView) != VK_SUCCESS) return false;
    }
```

### Step 2.4 — Cleanup binding 29 in destroyImages()

- [ ] **Step 2.4: Destroy binding 29**

In `path_tracer.cpp`, find `destroyImages()`. After the `m_outSpecRadianceMemory` cleanup (added in 4.C T2 around line 786), append:
```cpp
    if (m_nrdComposedView)   { vkDestroyImageView(m_device, m_nrdComposedView, nullptr);   m_nrdComposedView   = VK_NULL_HANDLE; }
    if (m_nrdComposedImage)  { vkDestroyImage(m_device, m_nrdComposedImage, nullptr);      m_nrdComposedImage  = VK_NULL_HANDLE; }
    if (m_nrdComposedMemory) { vkFreeMemory(m_device, m_nrdComposedMemory, nullptr);       m_nrdComposedMemory = VK_NULL_HANDLE; }
```

### Step 2.5 — Initialize `m_nrdCompositor` in PathTracer::init

- [ ] **Step 2.5: Extend the persistent-init block**

In `path_tracer.cpp`, find the `#ifdef OHAO_NRD_ENABLED` block in `PathTracer::init` that was rewritten in 4.C T2. Currently:
```cpp
#ifdef OHAO_NRD_ENABLED
    m_nrdDenoiser = std::make_unique<NrdDenoiser>();
    if (m_nrdDenoiser->initialize(m_instance, m_device, m_physicalDevice,
                                   m_graphicsQueueFamilyIndex,
                                   m_instanceExtensions, m_deviceExtensions,
                                   m_width, m_height)) {
        std::cout << "[NRD] persistent instance ready @ " << m_width << "x" << m_height << std::endl;
    } else {
        std::cerr << "[NRD] persistent instance init FAILED — disabling NRD path" << std::endl;
        m_nrdDenoiser.reset();
    }
#endif
```

(Exact current shape may differ slightly after 4.C T3a signature changes — locate the block by the `[NRD] persistent instance ready` log line.)

Immediately AFTER the closing `}` of the `else` branch (and still inside `#ifdef OHAO_NRD_ENABLED`), append:
```cpp
    m_nrdCompositor = std::make_unique<NrdCompositor>();
    if (!m_nrdCompositor->initialize(m_device, m_physicalDevice, m_width, m_height)) {
        std::cerr << "[NRD compose] init FAILED — compose pass will be skipped" << std::endl;
        m_nrdCompositor.reset();
    }
```
(The `initialize()` logs its own "[NRD compose] pipeline ready …" message on success, so we only log on failure.)

### Step 2.6 — Shutdown `m_nrdCompositor` in PathTracer::destroy

- [ ] **Step 2.6: Release on destroy**

In `path_tracer.cpp` find the `PathTracer::destroy` (or whichever destructor path holds the 4.C T2 NRD cleanup). Find the `m_nrdDenoiser.reset()` call and add right after:
```cpp
    if (m_nrdCompositor) {
        m_nrdCompositor->shutdown();
        m_nrdCompositor.reset();
    }
```

### Step 2.7 — Dispatch compose in PathTracer::render() after NRD denoise

- [ ] **Step 2.7: Add compose dispatch block**

In `path_tracer.cpp`, find the existing NRD denoise dispatch block in `render()` (added in 4.C T3b, inside `#ifdef OHAO_NRD_ENABLED`). It currently ends with the reverse-barrier loop that transitions bindings 19/20/22/23/26 from SHADER_READ back to GENERAL.

Directly after that reverse barrier AND still inside `#ifdef OHAO_NRD_ENABLED` AND still inside the `if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs)` block, append the compose dispatch logic:

```cpp
        if (m_nrdCompositor) {
            // Transition inputs for compose: bindings 24, 25, 27, 28 GENERAL → SHADER_READ
            VkImageMemoryBarrier cbIn[4] = {};
            VkImage cbInImages[4] = {
                m_diffAlbedoImage,       // binding 24
                m_specColorImage,        // binding 25
                m_outDiffRadianceImage,  // binding 27
                m_outSpecRadianceImage,  // binding 28
            };
            for (int i = 0; i < 4; ++i) {
                cbIn[i].sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                cbIn[i].srcAccessMask  = VK_ACCESS_SHADER_WRITE_BIT;
                cbIn[i].dstAccessMask  = VK_ACCESS_SHADER_READ_BIT;
                cbIn[i].oldLayout      = VK_IMAGE_LAYOUT_GENERAL;
                cbIn[i].newLayout      = VK_IMAGE_LAYOUT_GENERAL;  // stays GENERAL (compose reads storage)
                cbIn[i].image          = cbInImages[i];
                cbIn[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 4, cbIn);

            // Transition binding 29: UNDEFINED→GENERAL on first dispatch, GENERAL→GENERAL thereafter.
            // Using m_nrdComposeFirstFrame (not a function-local static) so offline + realtime PT
            // profiles each get their own first-frame counter.
            VkImageMemoryBarrier cbOut{};
            cbOut.sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            cbOut.srcAccessMask  = m_nrdComposeFirstFrame ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
            cbOut.dstAccessMask  = VK_ACCESS_SHADER_WRITE_BIT;
            cbOut.oldLayout      = m_nrdComposeFirstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            cbOut.newLayout      = VK_IMAGE_LAYOUT_GENERAL;
            cbOut.image          = m_nrdComposedImage;
            cbOut.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_nrdComposeFirstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &cbOut);
            m_nrdComposeFirstFrame = false;

            // Dispatch
            NrdComposeInputs ci {};
            ci.diffRadiance = m_outDiffRadianceView;
            ci.specRadiance = m_outSpecRadianceView;
            ci.diffAlbedo   = m_diffAlbedoView;
            ci.specColor    = m_specColorView;
            ci.composedOut  = m_nrdComposedView;
            m_nrdCompositor->dispatch(cmd, ci);

            // After compose, binding 29 is in GENERAL with SHADER_WRITE access. No further
            // in-frame consumer in 4.D scope (readback transitions GENERAL→TRANSFER_SRC itself).
        }
```

**Note on `m_nrdComposeFirstFrame`:** defaults `true` via the header initializer (Step 2.1). Reset to `true` anywhere PathTracer recreates binding 29 (e.g. resize — not in 4.D scope but good hygiene). A function-local `static` would be WRONG here because offline + realtime PT profiles each have their own `PathTracer` instance; sharing state across instances would cause profile B to skip the UNDEFINED→GENERAL transition on its first frame.

### Step 2.8 — Extend IRTRendererProfile + forwarder

- [ ] **Step 2.8: Add profile accessor pair**

In `ohao/render/rt/rt_profile_renderer.hpp`, find the existing 4.C T2-added pair `getOutDiffRadianceAOVImage()` / `getOutSpecRadianceAOVImage()` in both the pure-virtual block and the `RTProfileRendererBase` forwarder block. Add below each pair:

Pure virtuals:
```cpp
    virtual VkImage     getNrdComposedAOVImage() const = 0;  // Sub-plan 4.D
    virtual VkImageView getNrdComposedAOV()      const = 0;
```

Forwarders (inside `RTProfileRendererBase`):
```cpp
    VkImage     getNrdComposedAOVImage() const override { return m_pathTracer->getNrdComposedAOVImage(); }
    VkImageView getNrdComposedAOV()      const override { return m_pathTracer->getNrdComposedAOV(); }
```

### Step 2.9 — Add VulkanRenderer readback + accessor

- [ ] **Step 2.9: Declare readback helper in renderer.hpp**

In `ohao/gpu/vulkan/renderer.hpp`, find the existing 4.C `readbackDenoisedDiffuse` / `readbackDenoisedSpecular` declarations. Add below:
```cpp
    // Sub-plan 4.D: NRD composed HDR output readback (RGBA32F)
    bool readbackNrdComposed(std::vector<float>& data, uint32_t& width, uint32_t& height);
```

Also add a passthrough accessor near `getOutDiffRadianceAOVImage()`:
```cpp
    VkImage getNrdComposedAOVImage() const;  // impl in renderer.cpp
```

- [ ] **Step 2.10: Implement readback + accessor in renderer.cpp**

In `ohao/gpu/vulkan/renderer.cpp`, find the `readbackDenoisedSpecular` implementation (added in 4.C T2). Immediately after its closing `}`, append `readbackNrdComposed` as an exact structural copy, swapping the source-image accessor call:

```cpp
bool VulkanRenderer::readbackNrdComposed(std::vector<float>& data,
                                          uint32_t& width, uint32_t& height) {
    VkImage srcImage = getNrdComposedAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16; // RGBA32F = 16 bytes
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
    toSrc.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout            = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image                = srcImage;
    toSrc.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            stagingBuf, 1, &region);

    VkImageMemoryBarrier toGen{};
    toGen.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGen.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
    toGen.image                = srcImage;
    toGen.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGen.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    toGen.dstAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
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

VkImage VulkanRenderer::getNrdComposedAOVImage() const {
    if (!m_activeRTProfile) return VK_NULL_HANDLE;
    return m_activeRTProfile->getNrdComposedAOVImage();
}
```

**Note:** the exact name of the active-profile pointer (`m_activeRTProfile`) depends on existing renderer.cpp structure. Look at how `getOutDiffRadianceAOVImage()` is implemented (added in 4.C T2) and mirror that exactly — if 4.C T2 used a different member name or branching, follow suit.

### Step 2.11 — env_demo CLI flag

- [ ] **Step 2.11: Parse --dump-nrd-composed in env_demo.cpp**

In `examples/env_demo.cpp`, find the existing 4.C T3b-added `dumpNrdDiffusePath` / `dumpNrdSpecularPath` declarations (around line 45-46). Add:
```cpp
    std::string dumpNrdComposedPath;
```

Then find the corresponding `--dump-nrd-diffuse=` / `--dump-nrd-specular=` parser branches (around line 68-71) and add:
```cpp
        } else if (arg.rfind("--dump-nrd-composed=", 0) == 0) {
            dumpNrdComposedPath = arg.substr(20);
```

Finally, find the `dumpNrdSpecularPath` block that calls `renderer.readbackDenoisedSpecular(...)` (added in 4.C T3b, around line 361) and append after it:
```cpp
    if (!dumpNrdComposedPath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackNrdComposed(data, w, h)) {
            std::cerr << "[NRD composed dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdComposedPath, data, w, h);
        }
    }
```

### Step 2.12 — Build and validation smoke

- [ ] **Step 2.12: Build OHAO_NRD=ON**

```bash
cmake --build build -j8 2>&1 | tail -10
```
Expected: clean build.

- [ ] **Step 2.13: Build OHAO_NRD=OFF**

```bash
cmake --build build-nonrd -j8 --target cornell_box 2>&1 | tail -3
```
Expected: clean build.

- [ ] **Step 2.14: Smoke — cornell_box probes**

```bash
./build/cornell_box /tmp/beauty_post_t2_4d.png 1 2>&1 | grep -E "NRD|compose|persistent"
```
Expected:
```
[NRD] integration ready @ 1920x1080 (NRI-backed REBLUR_DIFFUSE_SPECULAR)
[NRD] persistent instance ready @ 1920x1080
[NRD compose] pipeline ready @ 1920x1080
[NRD] integration ready @ 1920x1080 (NRI-backed REBLUR_DIFFUSE_SPECULAR)
[NRD] persistent instance ready @ 1920x1080
[NRD compose] pipeline ready @ 1920x1080
```
(Six log lines — each of the two PT profiles fires all three: NRD integration, NRD persistent, compose pipeline.)

- [ ] **Step 2.15: Beauty invariant check**

```bash
sha256sum /tmp/beauty_pre_t2_4d.png /tmp/beauty_post_t2_4d.png
```
Expected: the two hashes MATCH. If they differ, T2 broke the beauty-untouched invariant — STOP and investigate. Likely culprits: accidentally modified output-image transitions; descriptor write drift; memory offset shift.

### Step 2.16 — Visual verification with env_demo

- [ ] **Step 2.16: Dump composed HDR**

Locate a GLB + HDR pair in `assets/` (e.g. `assets/DamagedHelmet.glb` + `assets/env/*.hdr`, or `assets/normal_humans/*.glb`). Substitute real paths.

```bash
./build/env_demo <mesh.glb> <env.hdr> /tmp/beauty_4d.png 1 \
    --dump-diffuse=/tmp/raw_diff_4d.png \
    --dump-nrd-diffuse=/tmp/nrd_diff_4d.png \
    --dump-specular=/tmp/raw_spec_4d.png \
    --dump-nrd-specular=/tmp/nrd_spec_4d.png \
    --dump-diff-albedo=/tmp/albedo_4d.png \
    --dump-spec-color=/tmp/f0_4d.png \
    --dump-nrd-composed=/tmp/composed_4d.png 2>&1 | tee /tmp/t2_4d_run.log
```
Expected outputs:
- All 7 PNGs produced without errors.
- `/tmp/t2_4d_run.log` contains the six-line init sequence from Step 2.14 (plus env_demo's own log).
- No `VALIDATION` or `ERROR` lines beyond the pre-existing pre-4.D baseline.
- `/tmp/composed_4d.png` is NOT black and NOT all white — it shows the scene with recognizable object colors.

- [ ] **Step 2.17: Render a high-spp reference for comparison**

```bash
./build/env_demo <mesh.glb> <env.hdr> /tmp/ref_256spp.png 256
```

Compare `/tmp/composed_4d.png` (1spp NRD composed) against `/tmp/ref_256spp.png` (raw PT 256spp). Expect rough lighting equivalence: overall color palette + shape should match. Composed may lose some high-frequency specular detail because NRD's spatial filter at frameIndex=0 blurs them; that's expected.

If the composed output is dramatically different (wrong overall brightness, color cast), flag as DONE_WITH_CONCERNS.

### Step 2.18 — Verification log

- [ ] **Step 2.18: Append 4.D entry**

Open `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. At the bottom, append:

```markdown
## 2026-04-24 — Sub-plan 4.D: Remodulation compositor

**Command:**
\```bash
./build/env_demo <mesh.glb> <env.hdr> /tmp/beauty_4d.png 1 \
    --dump-diffuse=/tmp/raw_diff_4d.png --dump-nrd-diffuse=/tmp/nrd_diff_4d.png \
    --dump-specular=/tmp/raw_spec_4d.png --dump-nrd-specular=/tmp/nrd_spec_4d.png \
    --dump-diff-albedo=/tmp/albedo_4d.png --dump-spec-color=/tmp/f0_4d.png \
    --dump-nrd-composed=/tmp/composed_4d.png
\```

**Evidence:**
- `[NRD compose] pipeline ready @ 1920x1080` logged at PathTracer init (once per PT profile — offline + realtime = 2 occurrences total).
- Binding 29 (RGBA32F) allocated; UNDEFINED→GENERAL barrier fires first frame only.
- Composed HDR PNG shows scene with recognizable object colors (albedo re-applied to denoised radiance).
- Composed PNG dramatically cleaner than raw 1spp beauty; overall lighting plausible vs. 256-spp raw reference modulo NRD spatial-filter blur on specular highlights.
- Beauty (binding 2, `out.png`) bit-identical to pre-4.D baseline — the compose pass writes only to binding 29.
- Zero new Vulkan validation errors.

**Observation:**
Demodulation loop closed: 3.C.6 split raw radiance into (demod AOV × albedo), 4.C denoised the demod AOV, and 4.D re-multiplies. Composed output is the first usable NRD beauty signal in OHAO. Temporal accumulation still disabled (`frameIndex=0`) — 4.E wires per-frame state and the `DenoiseMode::NRD` CLI flag.

**Status:** remodulation compositor live; 4.E unblocked.
```

### Step 2.19 — CLAUDE.md update

- [ ] **Step 2.19: Add binding 29 to the descriptor table in CLAUDE.md**

Open `CLAUDE.md`. Find the descriptor bindings table updated by 4.C T3b (last row is binding 28). Add a new row:
```markdown
| 29 | STORAGE_IMAGE | NRD composed HDR (RGBA32F) — Sub-plan 4.D. Not in RT layout; compose-set only. |
```

### Step 2.20 — Commit T2

- [ ] **Step 2.20: Commit**

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp \
        ohao/render/rt/rt_profile_renderer.hpp \
        ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md \
        CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(rt): remodulation compositor wired + env_demo dump (Sub-plan 4.D T2)

Binding 29 allocated as RGBA32F HDR storage image on PathTracer. NOT
added to PT's RT descriptor layout — only NrdCompositor's 5-binding
compute layout references it.

m_nrdCompositor is a persistent unique_ptr member alongside m_nrdDenoiser;
initialized in PathTracer::init after the NRD instance, destroyed in
PathTracer::destroy. render() transitions bindings 24/25/27/28 to
SHADER_READ_ONLY_OPTIMAL, barriers binding 29 UNDEFINED→GENERAL (first
frame) / GENERAL→GENERAL (subsequent), and calls dispatch().

env_demo gains --dump-nrd-composed=<path>. Spp=1 dump shows scene with
recognizable object colors (albedo re-applied to denoised radiance) and
is dramatically cleaner than raw 1spp beauty. Beauty PNG bit-identical
to pre-4.D baseline.

Verification log + CLAUDE.md descriptor binding table updated.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Done

Sub-plan 4.D complete:
- NrdCompositor PIMPL + compute shader (T1)
- Binding 29 + PathTracer wiring + env_demo CLI + visual verification (T2)

Next: 4.E will wire `DenoiseMode::NRD` CLI through all examples, route binding 29 into the tonemap path, and enable temporal accumulation (`frameIndex` + `viewMatrixPrev` + `jitterPrev`).
