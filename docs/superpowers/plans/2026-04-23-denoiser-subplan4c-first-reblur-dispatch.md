# Denoiser Sub-plan 4.C — First REBLUR Dispatch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the first real NVIDIA NRD `REBLUR_DIFFUSE_SPECULAR` compute dispatch against our 1spp AOVs, producing denoised diffuse + specular radiance that env_demo can dump to disk for visual "noisy vs smoothed" verification.

**Architecture:** Vendor NVIDIA's reference `NRDIntegration` helper into `external/nrd_integration/`, wrap it behind our existing `NrdDenoiser` PIMPL. Add PathTracer-owned bindings 27/28 as denoised outputs. Record the dispatch inside `PathTracer::render()` directly after `vkCmdTraceRaysKHR`, gated on `OHAO_NRD_ENABLED`. Beauty and `OHAO_NRD=OFF` paths stay bit-identical.

**Tech Stack:** Vulkan 1.3 compute, NVIDIA NRD v4.17.2, NRDIntegration header+cpp (vendored), GLSL raygen unchanged, CMake FetchContent.

**Spec:** `docs/superpowers/specs/2026-04-23-denoiser-subplan4c-first-reblur-dispatch-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `external/nrd_integration/NRDIntegration.h` (NEW) | Vendored NVIDIA integration helper header (v4.17.2 snapshot). |
| `external/nrd_integration/NRDIntegration.hpp` (NEW) | Vendored NVIDIA integration template impl (depending on upstream filename split). |
| `external/nrd_integration/NRDIntegration.cpp` (NEW) | Vendored NVIDIA integration .cpp. |
| `external/cmake/nrd.cmake` (modify) | Add a static lib target `NRDIntegration` that compiles the vendored sources and links `NRD`. |
| `ohao/render/rt/denoise/nrd_denoise.hpp` (modify) | Add `bool denoise(VkCommandBuffer)`. |
| `ohao/render/rt/denoise/nrd_denoise.cpp` (modify) | Hold an `nri`-style NRDIntegration instance in Impl; implement `denoise(cmd)`. |
| `ohao/render/rt/path_tracer.hpp` (modify) | Add bindings 27/28 images + views + accessors + persistent `m_nrdDenoiser`. |
| `ohao/render/rt/path_tracer.cpp` (modify) | Allocate bindings 27/28, extend descriptor layout+pool+writes, barriers, add SAMPLED usage to 19/20/22/23/26, replace 4.B probe with persistent `m_nrdDenoiser`, wire dispatch in `render()`. |
| `examples/env_demo.cpp` (modify) | Add `--dump-nrd-diffuse=` / `--dump-nrd-specular=` CLI flags and readback calls. |
| `ohao/gpu/vulkan/renderer.hpp` (modify) | Add `readbackDenoisedDiffuse` / `readbackDenoisedSpecular` methods. |
| `ohao/gpu/vulkan/renderer.cpp` (modify) | Implement the two new readback helpers (mirroring existing `readbackDiffuseRadiance` exactly). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` (modify) | Append 4.C entry. |
| `CLAUDE.md` (modify) | Append bindings 27 + 28 to the Path Tracer descriptor bindings table. |

Two guiding invariants across all tasks:
1. **Beauty PNG stays bit-identical** to pre-4.C output at matching seed+scene+spp.
2. **`-DOHAO_NRD=OFF` builds and runs** producing the same output as a 4.B OFF-build.

---

## Task 1: Vendor NRDIntegration and link it into ohao_renderer

Goal of this task: pull NVIDIA's integration helper into the tree and prove it compiles. No dispatch wiring yet — `NrdDenoiser::denoise()` stays absent. When the task finishes, `OHAO_NRD=ON` and `OHAO_NRD=OFF` both still build and the 4.B probe still logs successfully.

**Files:**
- Create: `external/nrd_integration/NRDIntegration.h` (copy from upstream)
- Create: `external/nrd_integration/NRDIntegration.hpp` (copy from upstream, if present)
- Create: `external/nrd_integration/NRDIntegration.cpp` (copy from upstream)
- Modify: `external/cmake/nrd.cmake`
- Modify: `ohao/render/CMakeLists.txt` (link NRDIntegration target)
- Modify: `ohao/render/rt/denoise/nrd_denoise.cpp` (forward-declare / include NRDIntegration, add member, but do not call any of its methods yet)

### Step 1.1 — Configure once to ensure NRD source tree is on disk

- [ ] **Step 1.1: Configure + partial build so NRD is fetched**

Run:
```bash
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
```
Expected: CMake configures successfully, `build/_deps/nrd-src/` now exists with NRD v4.17.2 checked out.

Then verify:
```bash
ls build/_deps/nrd-src/Integration/
```
Expected: some subset of `NRDIntegration.h`, `NRDIntegration.hpp`, `NRDIntegration.cpp` (NVIDIA has shipped the file as different combos across versions — whatever is there is what we vendor).

### Step 1.2 — Vendor the integration files

- [ ] **Step 1.2: Copy the integration files into external/nrd_integration/**

Run:
```bash
mkdir -p external/nrd_integration
cp build/_deps/nrd-src/Integration/NRDIntegration.h   external/nrd_integration/ 2>/dev/null || true
cp build/_deps/nrd-src/Integration/NRDIntegration.hpp external/nrd_integration/ 2>/dev/null || true
cp build/_deps/nrd-src/Integration/NRDIntegration.cpp external/nrd_integration/ 2>/dev/null || true
```

Expected: `external/nrd_integration/` contains at least one `.h`/`.hpp` plus one `.cpp` — whichever pair upstream v4.17.2 ships.

Then prepend a 3-line source-provenance comment to each vendored file:
```cpp
// Vendored from NVIDIA RayTracingDenoiser v4.17.2
// https://github.com/NVIDIAGameWorks/RayTracingDenoiser
// License: NVIDIA RTX SDKs License (carries upstream terms)
```

- [ ] **Step 1.3: Verify vendored files compile on their own**

(Nothing to build yet — the real compile happens after CMake wiring in Step 1.4.)

### Step 1.4 — Extend external/cmake/nrd.cmake with an NRDIntegration target

- [ ] **Step 1.4: Append NRDIntegration static-lib target**

Open `external/cmake/nrd.cmake`. After the existing `if(TARGET NRD) ... endif()` block, append:

```cmake
# Sub-plan 4.C: NRDIntegration — NVIDIA's reference Vulkan integration helper,
# vendored into external/nrd_integration/. Wraps NRD's "recipe" API
# (GetComputeDispatches, GetInstanceDesc) with concrete Vulkan pipeline,
# descriptor pool, texture pool, and barrier management.
set(OHAO_NRD_INTEGRATION_DIR ${CMAKE_SOURCE_DIR}/external/nrd_integration)

file(GLOB OHAO_NRD_INTEGRATION_SOURCES
    "${OHAO_NRD_INTEGRATION_DIR}/*.cpp"
    "${OHAO_NRD_INTEGRATION_DIR}/*.h"
    "${OHAO_NRD_INTEGRATION_DIR}/*.hpp"
)

add_library(NRDIntegration STATIC ${OHAO_NRD_INTEGRATION_SOURCES})
target_include_directories(NRDIntegration PUBLIC
    ${OHAO_NRD_INTEGRATION_DIR}
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(NRDIntegration PUBLIC NRD ${Vulkan_LIBRARIES})
target_compile_features(NRDIntegration PUBLIC cxx_std_17)
```

- [ ] **Step 1.5: Link NRDIntegration into ohao_renderer under OHAO_NRD gate**

Open `ohao/render/CMakeLists.txt`. Extend the existing `if(OHAO_NRD) ... endif()` block (around line 73) so it reads:

```cmake
if(OHAO_NRD)
    target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)
    target_link_libraries(ohao_renderer PRIVATE NRD NRDIntegration)
endif()
```

- [ ] **Step 1.6: Configure + verify NRDIntegration target exists**

Run:
```bash
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -20
```

Expected: `NRD: target 'NRD' available (pinned tag v4.17.2)` prints, no errors, configure succeeds.

### Step 1.5 — Stub NRDIntegration into NrdDenoiser::Impl without calling it

The goal here: hold the integration object so later tasks can use it, but do NOT wire per-frame calls yet. This keeps T1 a buildability-only task.

- [ ] **Step 1.7: Add NRDIntegration member to Impl**

Open `ohao/render/rt/denoise/nrd_denoise.cpp`. Inside the `OHAO_NRD_ENABLED` section, after the existing `#include <NRD.h>` (line 8), add:

```cpp
#include <NRDIntegration.h>  // vendored in external/nrd_integration/
```

(If upstream filename is `NRDIntegration.hpp`, include that instead — whichever the vendored tree contains.)

Then in `struct NrdDenoiser::Impl` (starting around line 14), add a member:

```cpp
struct NrdDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    nrd::Instance*   instance       = nullptr;
    NrdInputImages   inputs {};
    NrdIntegration   integration {};  // NEW 4.C: manages NRD pipelines + dispatch
};
```

If `NrdIntegration` requires constructor args (some upstream versions do), default-init + defer the real init to a future task. Do NOT call any of its methods yet.

- [ ] **Step 1.8: Build — OHAO_NRD=ON**

Run:
```bash
cmake --build build -j8 2>&1 | tail -40
```

Expected: clean build, no new warnings beyond NRDIntegration's own (which we don't own — acceptable). `ohao_renderer` links against `NRDIntegration` target. Existing `cornell_box`/`env_demo` binaries rebuild.

If NRDIntegration includes headers with different filenames than expected (e.g. `NRIDescs.h`, `NRI.h` — NRI is NVIDIA's abstraction layer), inspect `external/nrd_integration/*.cpp` `#include` lines, and either vendor those too or disable the code path by wrapping the integration member in `#ifdef`. Upstream NRDIntegration since v4.10+ depends on NRI (another NVIDIA project). If NRI is not pulled in, the integration helper won't compile — in that case, defer to Step 1.9.

- [ ] **Step 1.9: Fallback if NRDIntegration depends on NRI**

If Step 1.8 fails because NRDIntegration needs `NRI.h`, do NOT vendor NRI as well (too large). Instead:

1. Revert the `NrdIntegration integration {}` member added in Step 1.7 (keep Impl at its 4.B shape).
2. In `external/cmake/nrd.cmake`, guard the `add_library(NRDIntegration ...)` block with:
   ```cmake
   if(EXISTS "${OHAO_NRD_INTEGRATION_DIR}/NRDIntegration.cpp")
       # ... the add_library block ...
   endif()
   ```
3. In `ohao/render/CMakeLists.txt` switch to:
   ```cmake
   if(OHAO_NRD)
       target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)
       target_link_libraries(ohao_renderer PRIVATE NRD)
       if(TARGET NRDIntegration)
           target_link_libraries(ohao_renderer PRIVATE NRDIntegration)
           target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_INTEGRATION_AVAILABLE)
       endif()
   endif()
   ```
4. Document in `docs/superpowers/plans/2026-04-23-denoiser-subplan4c-first-reblur-dispatch.md` (this file) under Task 3 Step 3.1 that Task 3 will use raw `nrd::GetComputeDispatches` against `NRD.h` directly (without NRDIntegration). This is a fallback path; prefer the helper if NRI is simple enough to also vendor.

State the choice clearly in the commit message for Step 1.10.

- [ ] **Step 1.10: Build — OHAO_NRD=OFF**

Run:
```bash
cmake -B build-nonrd -S . -DOHAO_NRD=OFF -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build-nonrd -j8 2>&1 | tail -10
```

Expected: clean build. NRD is NOT fetched. NRDIntegration lib target not created. No unresolved symbols.

- [ ] **Step 1.11: Smoke run — 4.B probe still fires**

Run:
```bash
./build/cornell_box /tmp/smoke.png 1 2>&1 | grep -E "NRD|probe"
```

Expected stdout includes:
```
[NRD] initialized for 1024x1024
[NRD probe] 4.B CommonSettings accepted
```

- [ ] **Step 1.12: Commit**

Run:
```bash
git add external/nrd_integration/ external/cmake/nrd.cmake ohao/render/CMakeLists.txt ohao/render/rt/denoise/nrd_denoise.cpp
git commit -m "$(cat <<'EOF'
feat(rt): vendor NRDIntegration helper under external/nrd_integration/ (Sub-plan 4.C T1)

Copies NVIDIA's reference Vulkan integration helper from
build/_deps/nrd-src/Integration/ into external/nrd_integration/ so we
own a pinned v4.17.2 snapshot. Builds it as a separate static lib
NRDIntegration, linked into ohao_renderer under OHAO_NRD=ON.

No dispatch wiring yet — 4.C T2 adds bindings 27/28 and T3 wires the
per-frame dispatch call. 4.B probe continues to fire and pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add bindings 27/28 + promote probe to persistent member

Goal: allocate `m_outDiffRadianceImage` / `m_outSpecRadianceImage` at bindings 27 + 28 (RGBA32F), register them in descriptor layout/pool/writes, add UNDEFINED→GENERAL barrier, promote `m_nrdDenoiser` to a persistent `std::unique_ptr<NrdDenoiser>` member, and add `VK_IMAGE_USAGE_SAMPLED_BIT` to bindings 19/20/22/23/26. No dispatch yet — just resource plumbing + member promotion. When done: beauty bit-identical, smoke test passes, `readbackDenoisedDiffuse` returns all-zero image (because nothing has written to bindings 27/28 yet).

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp` — members + accessors
- Modify: `ohao/render/rt/path_tracer.cpp` — image creation + descriptor layout + pool + writes + barriers + probe replacement + usage flag bumps
- Modify: `ohao/gpu/vulkan/renderer.hpp` — readback method declarations
- Modify: `ohao/gpu/vulkan/renderer.cpp` — readback method impls

### Step 2.1 — Add bindings 27/28 members + accessors in path_tracer.hpp

- [ ] **Step 2.1: Add out-radiance images and accessors**

Open `ohao/render/rt/path_tracer.hpp`. Directly after line 279 (`VkImageView m_normalRoughnessView = VK_NULL_HANDLE;`), add:

```cpp
    // Feature 4.C: NRD denoised diffuse output (RGBA32F)
    VkImage        m_outDiffRadianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_outDiffRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_outDiffRadianceView   = VK_NULL_HANDLE;

    // Feature 4.C: NRD denoised specular output (RGBA32F)
    VkImage        m_outSpecRadianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_outSpecRadianceMemory = VK_NULL_HANDLE;
    VkImageView    m_outSpecRadianceView   = VK_NULL_HANDLE;
```

Then, in the accessor region (directly after line 121, the `getNormalRoughnessAOVImage` method), add:

```cpp
    VkImageView getOutDiffRadianceAOV()      const { return m_outDiffRadianceView; }
    VkImage     getOutDiffRadianceAOVImage() const { return m_outDiffRadianceImage; }
    VkImageView getOutSpecRadianceAOV()      const { return m_outSpecRadianceView; }
    VkImage     getOutSpecRadianceAOVImage() const { return m_outSpecRadianceImage; }
```

Then at the top of `path_tracer.hpp` (just after the existing `#include` block, before the `namespace ohao { … class PathTracer` block), add the forward declaration (only visible behind the guard to avoid dragging NRD symbols into every consumer):

```cpp
#ifdef OHAO_NRD_ENABLED
namespace ohao { class NrdDenoiser; }
#endif
```

Add the persistent denoiser member. At the top of the private section, near the existing `VkDevice m_device = VK_NULL_HANDLE;` declaration (around line 183), add:

```cpp
#ifdef OHAO_NRD_ENABLED
    std::unique_ptr<NrdDenoiser> m_nrdDenoiser;  // Sub-plan 4.C: replaces the 4.B scoped probe
#endif
```

Make sure `<memory>` is included in `path_tracer.hpp`.

### Step 2.2 — Allocate binding 27 image in createImages()

- [ ] **Step 2.2: Allocate binding 27 (OUT_DIFF_RADIANCE_HITDIST)**

Open `ohao/render/rt/path_tracer.cpp`. Directly after the closing `}` of the binding 26 normal+roughness image block (around line 720), add:

```cpp
    // ---- Sub-plan 4.C: NRD denoised diffuse output (RGBA32F) at binding 27 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outDiffRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outDiffRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outDiffRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outDiffRadianceImage, m_outDiffRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outDiffRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outDiffRadianceView) != VK_SUCCESS) return false;
    }
```

### Step 2.3 — Allocate binding 28 image in createImages()

- [ ] **Step 2.3: Allocate binding 28 (OUT_SPEC_RADIANCE_HITDIST)**

Directly after the binding 27 block you just added, append:

```cpp
    // ---- Sub-plan 4.C: NRD denoised specular output (RGBA32F) at binding 28 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outSpecRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outSpecRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outSpecRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outSpecRadianceImage, m_outSpecRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outSpecRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outSpecRadianceView) != VK_SUCCESS) return false;
    }
```

### Step 2.4 — Add SAMPLED_BIT to bindings 19/20/22/23/26

- [ ] **Step 2.4: Bump usage flags on NRD-sampled inputs**

NRD's `IN_*` slots are sampled textures. Our current images declare only `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. Find each of the following image creations in `ohao/render/rt/path_tracer.cpp` and change the usage flags:

| Binding | Image | Current usage line | Replacement |
|---------|-------|-------------------|-------------|
| 19 | `m_motionVectorImage` | (search `motionVectorImage` near `VK_IMAGE_USAGE_STORAGE_BIT`) | `VK_IMAGE_USAGE_STORAGE_BIT \| VK_IMAGE_USAGE_SAMPLED_BIT \| VK_IMAGE_USAGE_TRANSFER_SRC_BIT` |
| 20 | `m_depthAOVImage` | around line 655 | same replacement |
| 22 | `m_diffuseRadianceImage` | line 530 | same replacement |
| 23 | `m_specularRadianceImage` | line 570 | same replacement |
| 26 | `m_normalRoughnessImage` | line 693 | same replacement |

For each, replace the line:
```cpp
imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
```
with:
```cpp
imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
```

Use Edit tool with unique surrounding context (e.g. include the preceding `imageInfo.format = ...` line) to target the correct occurrence.

### Step 2.5 — Register bindings 27 + 28 in descriptor set layout

- [ ] **Step 2.5: Extend descriptor set layout array from 27 → 29**

In `ohao/render/rt/path_tracer.cpp`, at line 861, change:
```cpp
    VkDescriptorSetLayoutBinding bindings[27] = {};
```
to:
```cpp
    VkDescriptorSetLayoutBinding bindings[29] = {};
```

Directly after the binding 26 block (ending around line 1015), add:

```cpp
    // Binding 27: denoised diffuse (RGBA32F storage image) — Sub-plan 4.C, written by NRD
    bindings[27].binding         = 27;
    bindings[27].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[27].descriptorCount = 1;
    bindings[27].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;  // keep raygen stage for layout parity; raygen does not sample these

    // Binding 28: denoised specular (RGBA32F storage image) — Sub-plan 4.C, written by NRD
    bindings[28].binding         = 28;
    bindings[28].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[28].descriptorCount = 1;
    bindings[28].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
```

Then change (around line 1018):
```cpp
    VkDescriptorBindingFlags bindingFlags[27] = {};
```
to:
```cpp
    VkDescriptorBindingFlags bindingFlags[29] = {};
```

And (around line 1025):
```cpp
    flagsInfo.bindingCount = 27;
```
to:
```cpp
    flagsInfo.bindingCount = 29;
```

### Step 2.6 — Grow the pool size

- [ ] **Step 2.6: Bump STORAGE_IMAGE pool count**

In `ohao/render/rt/path_tracer.cpp` at line 1041, change:
```cpp
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},  // +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec radiance (3.C), +2 albedo/specColor (3.C.6), +1 normalRoughness (4.B)
```
to:
```cpp
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 18},  // +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec radiance (3.C), +2 albedo/specColor (3.C.6), +1 normalRoughness (4.B), +2 denoised out (4.C)
```

### Step 2.7 — Add descriptor writes for bindings 27 and 28

- [ ] **Step 2.7: Write bindings 27 + 28 into the descriptor set**

Find the descriptor-write block for binding 26 in `ohao/render/rt/path_tracer.cpp` (around line 1669, the `normalRoughnessInfo` write). Directly after that `writeCount++;`, add:

```cpp
    // Binding 27: denoised diffuse — Sub-plan 4.C
    VkDescriptorImageInfo outDiffInfo{};
    outDiffInfo.imageView   = m_outDiffRadianceView;
    outDiffInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 27;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &outDiffInfo;
    writeCount++;

    // Binding 28: denoised specular — Sub-plan 4.C
    VkDescriptorImageInfo outSpecInfo{};
    outSpecInfo.imageView   = m_outSpecRadianceView;
    outSpecInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 28;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &outSpecInfo;
    writeCount++;
```

The `writes[]` array size near the top of this function likely has a fixed dimension (e.g. `VkWriteDescriptorSet writes[27] = {}`). Search upward from the normal+roughness write for that array declaration — if it's sized to exactly 27, bump it to 29:
```cpp
    VkWriteDescriptorSet writes[29] = {};
```
If the array uses `std::vector` or is already oversized, no change needed — verify the actual declaration.

### Step 2.8 — Add UNDEFINED→GENERAL barriers for bindings 27 and 28

- [ ] **Step 2.8: Grow the AOV barrier array from 10 → 12**

In `ohao/render/rt/path_tracer.cpp` around line 1718, change:
```cpp
        VkImageMemoryBarrier aovBarriers[10] = {};
```
to:
```cpp
        VkImageMemoryBarrier aovBarriers[12] = {};
```

Directly after the `aovBarriers[9]` block for `m_normalRoughnessImage` (ending around line 1808), append:

```cpp
        // Sub-plan 4.C: denoised diffuse output barrier
        aovBarriers[10].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[10].srcAccessMask = 0;
        aovBarriers[10].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[10].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[10].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[10].image = m_outDiffRadianceImage;
        aovBarriers[10].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 4.C: denoised specular output barrier
        aovBarriers[11].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[11].srcAccessMask = 0;
        aovBarriers[11].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[11].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[11].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[11].image = m_outSpecRadianceImage;
        aovBarriers[11].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
```

Then update the `vkCmdPipelineBarrier` call on line 1812 from:
```cpp
            0, 0, nullptr, 0, nullptr, 10, aovBarriers);
```
to:
```cpp
            0, 0, nullptr, 0, nullptr, 12, aovBarriers);
```

### Step 2.9 — Add cleanup for bindings 27 + 28

- [ ] **Step 2.9: Destroy bindings 27 + 28 in destroyImages()**

In `ohao/render/rt/path_tracer.cpp`, directly after the `m_normalRoughness*` cleanup block (around line 772), add:

```cpp
    if (m_outDiffRadianceView)    { vkDestroyImageView(m_device, m_outDiffRadianceView, nullptr);   m_outDiffRadianceView = VK_NULL_HANDLE; }
    if (m_outDiffRadianceImage)   { vkDestroyImage(m_device, m_outDiffRadianceImage, nullptr);      m_outDiffRadianceImage = VK_NULL_HANDLE; }
    if (m_outDiffRadianceMemory)  { vkFreeMemory(m_device, m_outDiffRadianceMemory, nullptr);       m_outDiffRadianceMemory = VK_NULL_HANDLE; }

    if (m_outSpecRadianceView)    { vkDestroyImageView(m_device, m_outSpecRadianceView, nullptr);   m_outSpecRadianceView = VK_NULL_HANDLE; }
    if (m_outSpecRadianceImage)   { vkDestroyImage(m_device, m_outSpecRadianceImage, nullptr);      m_outSpecRadianceImage = VK_NULL_HANDLE; }
    if (m_outSpecRadianceMemory)  { vkFreeMemory(m_device, m_outSpecRadianceMemory, nullptr);       m_outSpecRadianceMemory = VK_NULL_HANDLE; }
```

### Step 2.10 — Replace 4.B probe with persistent member initialization

- [ ] **Step 2.10: Promote the probe to a persistent m_nrdDenoiser**

In `ohao/render/rt/path_tracer.cpp`, find the `OHAO_NRD_ENABLED` block at line 121. Replace:

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

with:

```cpp
#ifdef OHAO_NRD_ENABLED
    m_nrdDenoiser = std::make_unique<NrdDenoiser>();
    if (m_nrdDenoiser->initialize(m_device, m_physicalDevice, m_width, m_height)) {
        std::cout << "[NRD] persistent instance ready @ " << m_width << "x" << m_height << std::endl;
    } else {
        std::cerr << "[NRD] persistent instance init FAILED — disabling NRD path" << std::endl;
        m_nrdDenoiser.reset();
    }
#endif
```

Then add to `PathTracer::destroy()` (search for the function; likely cleans up m_rtPipeline etc.). Directly before any `destroyImages()` call, add:

```cpp
#ifdef OHAO_NRD_ENABLED
    if (m_nrdDenoiser) {
        m_nrdDenoiser->shutdown();
        m_nrdDenoiser.reset();
    }
#endif
```

### Step 2.11 — Add readback helpers for bindings 27 + 28

- [ ] **Step 2.11: Declare readback methods**

In `ohao/gpu/vulkan/renderer.hpp`, directly after `readbackSpecularRadiance` (line 186), add:

```cpp
    // Sub-plan 4.C: NRD denoised output readback
    bool readbackDenoisedDiffuse(std::vector<float>& data, uint32_t& width, uint32_t& height);
    bool readbackDenoisedSpecular(std::vector<float>& data, uint32_t& width, uint32_t& height);
```

- [ ] **Step 2.12: Implement readback methods (copy existing pattern)**

In `ohao/gpu/vulkan/renderer.cpp`, directly after the `readbackSpecularRadiance` implementation (search for the closing `}` of that function, around line 1280), add two new functions that mirror the existing ones verbatim but target the denoised images. Replace `getDiffuseRadianceAOVImage()` with `getOutDiffRadianceAOVImage()` / `getOutSpecRadianceAOVImage()` in each.

Full `readbackDenoisedDiffuse` (mirror of `readbackDiffuseRadiance`):

```cpp
bool VulkanRenderer::readbackDenoisedDiffuse(std::vector<float>& data, uint32_t& width, uint32_t& height) {
    VkImage srcImage = getOutDiffRadianceAOVImage();
    if (srcImage == VK_NULL_HANDLE) return false;

    width  = m_width;
    height = m_height;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 16;
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

Then paste an exact-duplicate function `readbackDenoisedSpecular` immediately after, with `getOutDiffRadianceAOVImage()` → `getOutSpecRadianceAOVImage()`.

You also need accessor passthroughs in `VulkanRenderer` to reach these. Search `ohao/gpu/vulkan/renderer.cpp` for an existing `getDiffuseRadianceAOVImage()` impl (probably forwards to `PathTracer::getDiffuseRadianceAOVImage()`). Mirror that forwarder pattern for `getOutDiffRadianceAOVImage()` and `getOutSpecRadianceAOVImage()`. If existing forwarders are inline in the header, add the two new ones next to `getDiffuseRadianceAOVImage()` in `renderer.hpp`.

### Step 2.12 — Build and smoke test

- [ ] **Step 2.13: Build OHAO_NRD=ON**

Run:
```bash
cmake --build build -j8 2>&1 | tail -20
```

Expected: clean build. No errors.

- [ ] **Step 2.14: Build OHAO_NRD=OFF**

Run:
```bash
cmake --build build-nonrd -j8 2>&1 | tail -20
```

Expected: clean build.

- [ ] **Step 2.15: Smoke run — confirm persistent instance + beauty bit-identical**

Run:
```bash
./build/env_demo assets/DamagedHelmet.glb assets/env/studio.hdr /tmp/beauty_4c.png 1 2>&1 | grep -E "NRD|persistent"
```

Expected: log contains `[NRD] persistent instance ready @ 1024x1024` (or whatever the render resolution is).
Expected: `/tmp/beauty_4c.png` is produced and byte-identical to the corresponding pre-4.C run (sha256 compared against a pre-4.C baseline if one was captured before starting T2).

Capture a sha256 baseline *before* applying Task 2 changes by running the same command on the pre-T2 state and recording the hash; compare after. Drift = bug.

- [ ] **Step 2.16: Commit**

```bash
git add ohao/render/rt/path_tracer.hpp ohao/render/rt/path_tracer.cpp ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp
git commit -m "$(cat <<'EOF'
feat(rt): bindings 27+28 + persistent NrdDenoiser (Sub-plan 4.C T2)

Allocates denoised-diffuse (binding 27) and denoised-specular (binding 28)
storage images at RGBA32F, extends descriptor layout/pool/writes/barriers.
Adds VK_IMAGE_USAGE_SAMPLED_BIT to NRD-input AOVs (bindings 19, 20, 22,
23, 26) so NRD can sample them. Promotes 4.B scoped probe to persistent
m_nrdDenoiser unique_ptr member; logs "[NRD] persistent instance ready".
Adds readbackDenoisedDiffuse / readbackDenoisedSpecular in VulkanRenderer.

No dispatch wiring yet — bindings 27/28 stay zero-initialized until T3
records the NRD compute dispatch. Beauty bit-identical verified against
pre-T2 baseline.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Wire NRD dispatch + env_demo dumps + verify

Goal: make `NrdDenoiser::denoise(cmd)` actually record NRD's compute dispatches, call it from `PathTracer::render()` after raygen, add env_demo CLI flags, dump visually-smoother PNGs, update verification log + CLAUDE.md.

**Files:**
- Modify: `ohao/render/rt/denoise/nrd_denoise.hpp` — add `denoise()` method
- Modify: `ohao/render/rt/denoise/nrd_denoise.cpp` — implement `denoise()` delegating to NRDIntegration
- Modify: `ohao/render/rt/path_tracer.cpp` — barrier AOVs to SHADER_READ, call denoise(), barrier back
- Modify: `examples/env_demo.cpp` — parse flags, call readbacks, dump PNGs
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`
- Modify: `CLAUDE.md`

### Step 3.1 — Add denoise() to NrdDenoiser public API

- [ ] **Step 3.1: Declare denoise(VkCommandBuffer)**

Open `ohao/render/rt/denoise/nrd_denoise.hpp`. Directly after the existing `setInputImages` declaration (line 68), add:

```cpp
    /// Record NRD REBLUR_DIFFUSE_SPECULAR compute dispatches onto `cmd`.
    /// Preconditions:
    ///   - initialize() has succeeded
    ///   - setCommonSettings() has been called this frame
    ///   - setInputImages() has been called this frame with all IN_* + OUT_* views bound
    ///   - IN_* images are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    ///   - OUT_* images are in VK_IMAGE_LAYOUT_GENERAL
    /// Returns false on NRD-side failure (logs error).
    bool denoise(VkCommandBuffer cmd);
```

### Step 3.2 — Implement denoise() wiring NRDIntegration

The concrete call into NRDIntegration depends on whether Task 1 took the integration path (Step 1.7/1.8) or the fallback raw-NRD path (Step 1.9). Handle both.

- [ ] **Step 3.2: Implement denoise() using NRDIntegration (primary path)**

In `ohao/render/rt/denoise/nrd_denoise.cpp`, inside the `OHAO_NRD_ENABLED` block, directly after `setInputImages` (line 94), add:

```cpp
bool NrdDenoiser::denoise(VkCommandBuffer cmd) {
    if (!m_impl->instance) return false;

    // Wire our cached input image views into NRDIntegration's UserPool.
    // Slot names match nrd::ResourceType enumerators; exact setter method
    // depends on upstream NRDIntegration header — it is typically
    // something like `SetResource(NrdUserPool&, ResourceType, ...)`.

    NrdUserPool userPool {};
    NrdIntegrationTexture& mv       = userPool[(size_t)nrd::ResourceType::IN_MV];
    NrdIntegrationTexture& nr       = userPool[(size_t)nrd::ResourceType::IN_NORMAL_ROUGHNESS];
    NrdIntegrationTexture& vz       = userPool[(size_t)nrd::ResourceType::IN_VIEWZ];
    NrdIntegrationTexture& diffIn   = userPool[(size_t)nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST];
    NrdIntegrationTexture& specIn   = userPool[(size_t)nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST];
    NrdIntegrationTexture& diffOut  = userPool[(size_t)nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST];
    NrdIntegrationTexture& specOut  = userPool[(size_t)nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST];

    // Concrete member names on NrdIntegrationTexture depend on upstream;
    // typically "subresourceStates" / "format" / "resource" / "view". Fill
    // them from m_impl->inputs.* views + known image formats.
    // Consult external/nrd_integration/NRDIntegration.h for the struct.

    // Pseudo-shape — ADAPT to upstream struct layout:
    // mv.resource = m_impl->inputs.motionVector; mv.format = ...;
    // ... etc for nr, vz, diffIn, specIn, diffOut, specOut.

    // Identifier matches the one we used in CreateInstance.
    const nrd::Identifier identifiers[1] = { 0 };
    m_impl->integration.Denoise(identifiers, 1, cmd, userPool, /*enableDescriptorCaching*/ true);
    return true;
}
```

**Important:** the exact NRDIntegration API signature differs across NRD versions. Before writing this block verbatim, open `external/nrd_integration/NRDIntegration.h` and:
1. Find the type `NrdUserPool` (may be `NrdIntegrationUserPool`, `nri::UserPool`, etc. in upstream).
2. Find the entry-point method — typically named `Denoise(…)` or `Draw(…)`. Read the signature.
3. Find the member names on the per-slot descriptor struct.
4. Write the minimum glue code to bind each `m_impl->inputs.*` view into the matching slot.

If NRDIntegration requires NRI handles (NVIDIA Render Interface) instead of raw VkImage/VkImageView, and NRI wasn't vendored, go to Step 3.3.

- [ ] **Step 3.3: Fallback — raw `nrd::GetComputeDispatches` dispatch**

Only if NRDIntegration requires NRI and NRI wasn't vendored in Task 1. This path manually consumes NRD's dispatch recipes.

Replace the body of `denoise()` with the minimal manual dispatch sketched below. Because manual dispatch requires creating shader modules, compute pipelines, descriptor pools, samplers, and a constant-buffer ring, this is a **substantially larger code change** than the NRDIntegration path — estimate 400-600 lines.

Sketch (NOT complete — adapt from NRD's own sample code under `build/_deps/nrd-src/Samples/`):

```cpp
bool NrdDenoiser::denoise(VkCommandBuffer cmd) {
    if (!m_impl->instance) return false;

    // 1. Query dispatch list
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0;
    const nrd::Identifier identifiers[1] = { 0 };
    nrd::Result r = nrd::GetComputeDispatches(*m_impl->instance, identifiers, 1,
                                              dispatches, dispatchCount);
    if (r != nrd::Result::SUCCESS) return false;

    // 2. For each dispatch:
    //    a. ensure the compute VkPipeline for dispatch[i].pipelineIndex exists
    //       (lazy-create from nrd::GetInstanceDesc shader blob on first call)
    //    b. allocate/update a descriptor set binding:
    //       - resources from userPool (our 7 views)
    //       - transient resources from NRD-owned texture pool
    //       - sampler set from nrd::GetInstanceDesc samplers
    //       - constants block written to a ring buffer + bound as uniform
    //    c. vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDispatch(gridWidth, gridHeight, 1)
    //    d. memory barrier between dispatches for texture slots flagged
    //       as both produced and consumed

    return true;
}
```

Writing this fallback fully is beyond a single step. If you take this path, scope the first merge to: "dispatch-count > 0, pipelines compile, dispatches recorded, but outputs still may be zero due to missing constant buffer / resource-pool wiring" — and split actual correctness verification into a follow-up task. This should be avoided if NRDIntegration (primary path) works.

### Step 3.4 — Call denoise() from PathTracer::render()

- [ ] **Step 3.4: Add SHADER_READ transitions + denoise call after vkCmdTraceRaysKHR**

In `ohao/render/rt/path_tracer.cpp`, find `vkCmdTraceRaysKHR(cmd, …)` (line 1898). Directly after it, before any `TRANSFER_SRC_OPTIMAL` transition (line 1901), insert:

```cpp
#ifdef OHAO_NRD_ENABLED
    if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs) {
        // Transition NRD input AOVs GENERAL → SHADER_READ_ONLY_OPTIMAL
        VkImageMemoryBarrier inBarriers[5] = {};
        VkImage inImages[5] = {
            m_motionVectorImage,     // binding 19 (IN_MV)
            m_depthAOVImage,         // binding 20 (IN_VIEWZ)
            m_diffuseRadianceImage,  // binding 22 (IN_DIFF_RADIANCE_HITDIST)
            m_specularRadianceImage, // binding 23 (IN_SPEC_RADIANCE_HITDIST)
            m_normalRoughnessImage,  // binding 26 (IN_NORMAL_ROUGHNESS)
        };
        for (int i = 0; i < 5; ++i) {
            inBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            inBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            inBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            inBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            inBarriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            inBarriers[i].image = inImages[i];
            inBarriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 5, inBarriers);

        // Pump per-frame state into NRD.
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

        NrdInputImages inputs {};
        inputs.motionVector           = m_motionVectorView;
        inputs.viewZ                  = m_depthAOVView;
        inputs.normalRoughness        = m_normalRoughnessView;
        inputs.diffRadianceHitDist    = m_diffuseRadianceView;
        inputs.specRadianceHitDist    = m_specularRadianceView;
        inputs.diffAlbedo             = m_diffAlbedoView;      // for 4.D completeness — NRD slot not used in 4.C
        inputs.specColor              = m_specColorView;
        inputs.outDiffRadianceHitDist = m_outDiffRadianceView;
        inputs.outSpecRadianceHitDist = m_outSpecRadianceView;
        m_nrdDenoiser->setInputImages(inputs);

        m_nrdDenoiser->denoise(cmd);

        // Transition NRD input AOVs back SHADER_READ_ONLY_OPTIMAL → GENERAL so
        // downstream readbacks (TRANSFER_SRC) still find them in a known state.
        for (int i = 0; i < 5; ++i) {
            inBarriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            inBarriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            inBarriers[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            inBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 5, inBarriers);
    }
#endif
```

Make sure `<glm/gtc/type_ptr.hpp>` is included at the top of `path_tracer.cpp`.

### Step 3.5 — Add env_demo CLI flags

- [ ] **Step 3.5: Parse --dump-nrd-diffuse / --dump-nrd-specular**

Open `examples/env_demo.cpp`. Directly after line 44 (`std::string dumpSpecularPath;`), add:

```cpp
    std::string dumpNrdDiffusePath;
    std::string dumpNrdSpecularPath;
```

Directly after line 65 (`dumpSpecularPath = arg.substr(16);`), add:

```cpp
        } else if (arg.rfind("--dump-nrd-diffuse=", 0) == 0) {
            dumpNrdDiffusePath = arg.substr(19);
        } else if (arg.rfind("--dump-nrd-specular=", 0) == 0) {
            dumpNrdSpecularPath = arg.substr(20);
```

### Step 3.6 — Add readback + dump calls for denoised outputs

- [ ] **Step 3.6: Dump denoised AOVs**

In `examples/env_demo.cpp`, directly after the existing `dumpSpecularPath` block (around line 345), add:

```cpp
    if (!dumpNrdDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t dw = 0, dh = 0;
        if (!renderer.readbackDenoisedDiffuse(data, dw, dh)) {
            std::cerr << "[NRD diffuse dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdDiffusePath, data, dw, dh);
        }
    }

    if (!dumpNrdSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t sw = 0, sh = 0;
        if (!renderer.readbackDenoisedSpecular(data, sw, sh)) {
            std::cerr << "[NRD specular dump] readback failed\n";
        } else {
            dumpRGBA32FStream(dumpNrdSpecularPath, data, sw, sh);
        }
    }
```

### Step 3.7 — Build + validation smoke

- [ ] **Step 3.7: Build OHAO_NRD=ON and OHAO_NRD=OFF**

Run:
```bash
cmake --build build -j8 2>&1 | tail -20
cmake --build build-nonrd -j8 2>&1 | tail -10
```

Expected: both clean builds.

- [ ] **Step 3.8: Run the dispatch under validation layer**

Run with validation layers enabled (typically `VK_LAYER_KHRONOS_validation` enabled in device_setup.cpp; verify no new warnings):
```bash
./build/env_demo assets/DamagedHelmet.glb assets/env/studio.hdr /tmp/beauty.png 1 \
    --dump-diffuse=/tmp/raw_diff.png \
    --dump-nrd-diffuse=/tmp/nrd_diff.png \
    --dump-specular=/tmp/raw_spec.png \
    --dump-nrd-specular=/tmp/nrd_spec.png 2>&1 | tee /tmp/4c_run.log
```

Expected:
- `[NRD] persistent instance ready @ ...` in log
- Zero `ERROR / VALIDATION` lines in `/tmp/4c_run.log`
- All four PNG files written
- `/tmp/beauty.png` bit-identical to pre-T3 baseline (capture baseline before T3 starts)

### Step 3.8 — Visual verification of denoised PNGs

- [ ] **Step 3.9: Visual inspection**

Open `/tmp/raw_diff.png` and `/tmp/nrd_diff.png` side-by-side. Expected: `nrd_diff.png` shows the same overall image but visibly smoother — noise grains in the raw 1spp should be replaced by spatially-filtered smooth patches. Same relationship for specular.

If `nrd_diff.png` is **all black** or **all NaN** (PNG might render as odd colors): this indicates NRD wrote zero to the output. Root-cause before committing:
- Confirm the NRD dispatches actually fire: add a `vkQueueWaitIdle` immediately after `m_nrdDenoiser->denoise(cmd)` and assert no device-lost.
- Confirm the output images receive writes: check an intermediate binding (e.g. swap input for output in a hand-rolled shader that just copies through).
- Check the `nrd::GetComputeDispatches` return count via a temporary log.

If the outputs look exactly like the input (no smoothing): `frameIndex=0` with no history should still produce spatial filtering. Verify NRD's `ReblurSettings::antilagSettings` / history validation defaults aren't killing the spatial path. If needed, log the number of dispatches returned by NRD for this frame (should be several).

### Step 3.9 — Verification log entry

- [ ] **Step 3.10: Append 4.C entry to verification log**

Open `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. At the bottom, append:

```markdown
## 2026-04-23 — Sub-plan 4.C: First REBLUR dispatch

**Command:**
```bash
./build/env_demo assets/DamagedHelmet.glb assets/env/studio.hdr /tmp/beauty_4c.png 1 \
    --dump-diffuse=/tmp/raw_diff.png --dump-nrd-diffuse=/tmp/nrd_diff.png \
    --dump-specular=/tmp/raw_spec.png --dump-nrd-specular=/tmp/nrd_spec.png
```

**Evidence:**
- `[NRD] persistent instance ready @ <W>x<H>` logged once at PathTracer init (replaces 4.B probe).
- `/tmp/nrd_diff.png` visibly smoother than `/tmp/raw_diff.png` — spatial filter dominates because `frameIndex=0` provides no temporal history.
- `/tmp/nrd_spec.png` likewise smoother than `/tmp/raw_spec.png` on the helmet's metallic regions; matte areas are near-zero in spec channel and reflect that (both raw and denoised).
- Beauty PNG bit-identical to pre-4.C baseline (sha256 match).
- Zero Vulkan validation errors across the NRD dispatch path.

**Observation:**
First real NRD work. Spatial-only filtering at `frameIndex=0` is expected — multi-frame temporal accumulation becomes meaningful once 4.E wires per-frame state. Compositing back into beauty comes in 4.D.

**Status:** dispatch path proven; 4.D (remodulation compositor) unblocked.
```

### Step 3.10 — CLAUDE.md binding table

- [ ] **Step 3.11: Append bindings 27 + 28 to the binding table in CLAUDE.md**

In `CLAUDE.md`, find the `### Descriptor Bindings (Path Tracer)` table. Directly after the row for binding 12 (`Bindless textures`), the table only documents up to 12 — extend it with:

```markdown
| 13-18 | (various) | Surface/shading history, env CDFs (see path_tracer.cpp) |
| 19 | STORAGE_IMAGE | Motion vectors (RG16F) — Sub-plan 3.A |
| 20 | STORAGE_IMAGE | Depth AOV / ViewZ (R32F) — Sub-plan 3.B |
| 21 | STORAGE_IMAGE | Roughness AOV (R16F) — Sub-plan 3.B/3.C.5 |
| 22 | STORAGE_IMAGE | Diffuse radiance + hit-dist (RGBA32F) — Sub-plan 3.C/3.D |
| 23 | STORAGE_IMAGE | Specular radiance + hit-dist (RGBA32F) — Sub-plan 3.C/3.D |
| 24 | STORAGE_IMAGE | Diffuse albedo (RGBA8) — Sub-plan 3.C.6 |
| 25 | STORAGE_IMAGE | Specular color / F0 (RGBA8) — Sub-plan 3.C.6 |
| 26 | STORAGE_IMAGE | Normal+roughness packed (R10G10B10A2) — Sub-plan 4.B |
| 27 | STORAGE_IMAGE | NRD denoised diffuse (RGBA32F) — Sub-plan 4.C |
| 28 | STORAGE_IMAGE | NRD denoised specular (RGBA32F) — Sub-plan 4.C |
```

### Step 3.11 — Commit 4.C

- [ ] **Step 3.12: Commit**

```bash
git add ohao/render/rt/denoise/nrd_denoise.hpp ohao/render/rt/denoise/nrd_denoise.cpp \
        ohao/render/rt/path_tracer.cpp \
        examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md \
        CLAUDE.md
git commit -m "$(cat <<'EOF'
feat(rt): first REBLUR dispatch via NRDIntegration (Sub-plan 4.C T3)

Wires NrdDenoiser::denoise(cmd) to NRDIntegration's per-frame Denoise call.
PathTracer::render() now transitions IN_MV / IN_NORMAL_ROUGHNESS / IN_VIEWZ /
IN_DIFF_RADIANCE_HITDIST / IN_SPEC_RADIANCE_HITDIST (bindings 19/26/20/22/23)
to SHADER_READ, pumps CommonSettings + input views, records the dispatch, and
transitions inputs back to GENERAL.

env_demo gets --dump-nrd-diffuse=<p> / --dump-nrd-specular=<p>. Visual diff
shows spatial-filter smoothing at frameIndex=0 (temporal left for 4.E).

Beauty PNG bit-identical to pre-4.C baseline; zero Vulkan validation errors;
-DOHAO_NRD=OFF still produces identical output.

Verification log + CLAUDE.md descriptor binding table updated.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Done

All three tasks above complete:
- NRDIntegration vendored + linked
- Bindings 27/28 allocated + probe persistent
- Dispatch recorded + verified visually

Sub-plan 4.C ready to merge. Next: 4.D (remodulation compositor) unblocks.
