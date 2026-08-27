# Differentiable Renderer — Stage 0a: Scaffolding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the differentiable renderer's foundation — device capability gating, buffer-backed gradient arena with proven float atomics, parameter registry, replayable path RNG, and a ray-query compute kernel validated against closed-form geometry.

**Architecture:** A new `ohao/diff/` static library plus `shaders/diff/` and `shaders/includes/diff/`. Nothing in this plan computes a gradient. It builds and proves the substrate every later stage assumes: that float atomics actually accumulate, that a path is a pure function of its seed, and that ray queries resolve visibility correctly. Each piece is testable in isolation, and the two hard device requirements are proven by execution rather than by reading a feature flag.

**Tech Stack:** C++20, Vulkan 1.3 (`VK_KHR_ray_query`, `VK_EXT_shader_atomic_float`), GLSL compiled by `glslc --target-env=vulkan1.3`, VMA via `GpuAllocator`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md`

## Global Constraints

- **C++20.** `CMAKE_CXX_STANDARD 20`, `target_compile_features(... cxx_std_20)`.
- **GLSL, not Slang.** Adjoints are hand-written. Do not introduce a Slang toolchain (Spec §3).
- **No CPU reference renderer.** Correctness comes from closed-form derivations and finite differences, never from a second implementation (Spec §2, §3).
- **Gradients are buffer-backed, never image-backed.** Required feature is `shaderBufferFloat32AtomicAdd` only; `shaderImageFloat32AtomicAdd` must not be depended on (Spec §4.3, §6.4).
- **Seed invariant.** A path is a pure function of `(pixel, sampleIndex, iterationSeed)`. No mutable RNG state may cross a kernel boundary (Spec §4.5).
- **Ray query, not RT pipeline.** The differentiable traversal must live in one function (Spec §3).
- **Shader naming.** `shaders/a/b.comp` compiles to `bin/shaders/a_b.comp.spv`. Include dirs are `shaders/` and `shaders/includes/`.
- **Commit style.** Conventional commits with scope, e.g. `feat(diff): ...`.

**Before starting:** branch from the current `chore/remove-inverse-stack` tip.

```bash
git checkout -b feat/diff-stage0a
```

**Build command used throughout:**

```bash
cmake --build build --config Release -j8
```

---

## File Structure

| File | Responsibility |
|---|---|
| `ohao/diff/CMakeLists.txt` | Declares `ohao_diff` static library |
| `ohao/diff/device_caps.hpp` / `.cpp` | Query whether a physical device can run the subsystem |
| `ohao/diff/grad/arena_layout.hpp` / `.cpp` | Pure suballocation arithmetic — no Vulkan |
| `ohao/diff/grad/gradient_arena.hpp` / `.cpp` | `VkBuffer`-backed arena; zero, readback |
| `ohao/diff/param/param_registry.hpp` / `.cpp` | `DiffParam` registration + format enforcement |
| `ohao/diff/rng/diff_rng.hpp` / `.cpp` | CPU reference for the replayable path RNG |
| `shaders/includes/diff/rng.glsl` | GLSL mirror of `diff_rng.cpp` |
| `shaders/diff/atomic_probe.comp` | Proves `atomicAdd` on float SSBO works |
| `shaders/diff/visibility_probe.comp` | Ray-query primary visibility |
| `tests/diff/CMakeLists.txt` | Test targets |
| `tests/diff/diff_unit_tests.cpp` | GoogleTest — CPU-only logic |
| `tests/diff/diff_gpu_probe.cpp` | Standalone exe — device-requiring checks |

Modified: `ohao/CMakeLists.txt`, `ohao/gpu/vulkan/device_setup.cpp`, `CMakeLists.txt` (root).

---

### Task 1: Device capability query and enablement

Two extensions the engine does not currently enable. `device_setup.cpp:131` lists device extensions; neither `VK_KHR_ray_query` nor `VK_EXT_shader_atomic_float` is present, and the feature chain at `device_setup.cpp:210-219` has no struct for either.

**Files:**
- Create: `ohao/diff/device_caps.hpp`, `ohao/diff/device_caps.cpp`, `ohao/diff/CMakeLists.txt`
- Modify: `ohao/CMakeLists.txt`, `ohao/gpu/vulkan/device_setup.cpp:131-152` and `:210-224`
- Test: `tests/diff/diff_unit_tests.cpp`, `tests/diff/CMakeLists.txt`, root `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `ohao::diff::DeviceCaps { bool rayQuery; bool bufferFloat32AtomicAdd; bool sufficient() const noexcept; }` and `ohao::diff::DeviceCaps ohao::diff::queryDeviceCaps(VkPhysicalDevice)`.

- [ ] **Step 1: Write the failing test**

Create `tests/diff/diff_unit_tests.cpp`:

```cpp
#include "diff/device_caps.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

// Bare instance + physical device. No surface, no logical device — this test
// asks only what the hardware advertises.
struct BareVulkan {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};

    bool init() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "diff_unit_tests";
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        // Prefer a discrete GPU; the iGPU lacks image atomics and may lack ray query.
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(d, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical = d;
                return true;
            }
        }
        physical = devices[0];
        return true;
    }

    ~BareVulkan() {
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

}  // namespace

TEST(DiffDeviceCaps, DiscreteGpuSupportsSubsystemRequirements) {
    BareVulkan vk;
    ASSERT_TRUE(vk.init()) << "no Vulkan instance / physical device available";

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(vk.physical);

    EXPECT_TRUE(caps.rayQuery)
        << "VK_KHR_ray_query is required: the differentiable traversal must be a single "
           "function, which rules out an SBT-dispatched RT pipeline";
    EXPECT_TRUE(caps.bufferFloat32AtomicAdd)
        << "shaderBufferFloat32AtomicAdd is required for gradient scatter";
    EXPECT_TRUE(caps.sufficient());
}

TEST(DiffDeviceCaps, SufficientRequiresBothFlags) {
    ohao::diff::DeviceCaps caps;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = true;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = false;
    caps.bufferFloat32AtomicAdd = true;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = true;
    EXPECT_TRUE(caps.sufficient());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Release -j8`
Expected: FAIL — `diff/device_caps.hpp` does not exist, target `diff_unit_tests` does not exist.

- [ ] **Step 3: Write the header**

Create `ohao/diff/device_caps.hpp`:

```cpp
#pragma once

#include <vulkan/vulkan.h>

namespace ohao::diff {

/// What the differentiable renderer needs from a physical device.
///
/// Note that shaderImageFloat32AtomicAdd is deliberately absent: gradients are
/// buffer-backed (design doc S4.3), so image atomics are never used. That
/// matters because image float atomics are materially less available -- the
/// Intel iGPU on this machine reports buffer=true, image=false.
struct DeviceCaps {
    bool rayQuery{false};
    bool bufferFloat32AtomicAdd{false};

    [[nodiscard]] bool sufficient() const noexcept {
        return rayQuery && bufferFloat32AtomicAdd;
    }
};

[[nodiscard]] DeviceCaps queryDeviceCaps(VkPhysicalDevice physicalDevice);

}  // namespace ohao::diff
```

- [ ] **Step 4: Write the implementation**

Create `ohao/diff/device_caps.cpp`:

```cpp
#include "diff/device_caps.hpp"

#include <cstring>
#include <vector>

namespace ohao::diff {
namespace {

bool hasExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

DeviceCaps queryDeviceCaps(VkPhysicalDevice physicalDevice) {
    DeviceCaps caps;
    if (physicalDevice == VK_NULL_HANDLE) return caps;

    // An advertised extension does not imply an enabled feature bit, so check both.
    const bool rqExt = hasExtension(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool afExt = hasExtension(physicalDevice, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{};
    atomicFloat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQuery.pNext = &atomicFloat;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &rayQuery;

    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    caps.rayQuery = rqExt && (rayQuery.rayQuery == VK_TRUE);
    caps.bufferFloat32AtomicAdd = afExt && (atomicFloat.shaderBufferFloat32AtomicAdd == VK_TRUE);
    return caps;
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Create the library CMake**

Create `ohao/diff/CMakeLists.txt`:

```cmake
# OHAO Differentiable Renderer
# Stage 0a: scaffolding only -- no gradients are computed here yet.

file(GLOB_RECURSE DIFF_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_library(ohao_diff STATIC ${DIFF_SOURCES})

target_include_directories(ohao_diff
    PUBLIC
        ${CMAKE_SOURCE_DIR}/ohao
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_features(ohao_diff PUBLIC cxx_std_20)

target_link_libraries(ohao_diff PUBLIC
    ohao_gpu_vulkan
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
)
```

Add to `ohao/CMakeLists.txt` after the `add_subdirectory(physics)` line:

```cmake
add_subdirectory(diff)
```

- [ ] **Step 6: Create the test CMake**

Create `tests/diff/CMakeLists.txt`:

```cmake
# Differentiable renderer tests.
#   diff_unit_tests  -- GoogleTest, CPU logic + bare-instance device queries
#   diff_gpu_probe   -- standalone exe, requires a working device (added in Task 3)

add_executable(diff_unit_tests diff_unit_tests.cpp)
target_include_directories(diff_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_unit_tests PRIVATE cxx_std_20)
target_link_libraries(diff_unit_tests PRIVATE
    ohao_diff gtest gtest_main ${Vulkan_LIBRARIES})
if(UNIX)
    target_link_libraries(diff_unit_tests PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(diff_unit_tests)
```

Add to root `CMakeLists.txt`, after the `BUILD_ENGINE_TESTS` block:

```cmake
# Differentiable renderer tests
option(BUILD_DIFF_TESTS "Build differentiable renderer tests" ON)
if(BUILD_DIFF_TESTS)
    message(STATUS "Building differentiable renderer tests")
    add_subdirectory(tests/diff)
endif()
```

- [ ] **Step 7: Re-configure and run the test**

Run:
```bash
cmake -B build -S . -G "Visual Studio 17 2022"
cmake --build build --config Release -j8 --target diff_unit_tests
./build/Release/diff_unit_tests.exe --gtest_filter=DiffDeviceCaps.*
```
Expected: both tests PASS. `DiscreteGpuSupportsSubsystemRequirements` passing confirms the RTX 5070 advertises what the subsystem needs.

- [ ] **Step 8: Enable the extensions and features on the logical device**

In `ohao/gpu/vulkan/device_setup.cpp`, add two entries to the `m_enabledDeviceExtensions` initializer list (which begins at line 131), immediately after `VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME`:

```cpp
        VK_KHR_RAY_QUERY_EXTENSION_NAME,                 // differentiable traversal (inline RT)
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,       // gradient scatter into SSBOs
```

Then extend the feature chain. Find this block (around line 216):

```cpp
    // Ray tracing pipeline features
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeatures.pNext = &asFeatures;
    rtFeatures.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = &rtFeatures;
```

and replace it with:

```cpp
    // Ray tracing pipeline features
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeatures.pNext = &asFeatures;
    rtFeatures.rayTracingPipeline = VK_TRUE;

    // Inline ray tracing for the differentiable traversal. The forward and
    // backward kernels must share one traversal source so their RNG consumption
    // order is identical, which requires the whole path in a single function.
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = &rtFeatures;
    rayQueryFeatures.rayQuery = VK_TRUE;

    // Gradient scatter. Buffer atomics only -- gradients are never image-backed.
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    atomicFloatFeatures.pNext = &rayQueryFeatures;
    atomicFloatFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = &atomicFloatFeatures;
```

- [ ] **Step 9: Verify existing renderer still boots**

Run:
```bash
cmake --build build --config Release -j8
./build/Release/renderer_test.exe
```
Expected: exits successfully and writes its PNG as before. A `vkCreateDevice` failure here means an extension or feature was rejected — check the validation output before proceeding.

- [ ] **Step 10: Commit**

```bash
git add ohao/diff/device_caps.hpp ohao/diff/device_caps.cpp ohao/diff/CMakeLists.txt \
        ohao/CMakeLists.txt ohao/gpu/vulkan/device_setup.cpp \
        tests/diff/CMakeLists.txt tests/diff/diff_unit_tests.cpp CMakeLists.txt
git commit -m "feat(diff): device caps query, enable ray query + buffer float atomics"
```

---

### Task 2: Arena layout arithmetic

Pure suballocation math, no Vulkan. Split out from the GPU arena so the offset logic is testable in microseconds.

**Files:**
- Create: `ohao/diff/grad/arena_layout.hpp`, `ohao/diff/grad/arena_layout.cpp`
- Test: `tests/diff/diff_unit_tests.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `ohao::diff::ArenaBlock { std::size_t offsetBytes; std::size_t sizeBytes; }`, `ohao::diff::ArenaLayout` with `std::size_t add(std::size_t floatCount)`, `ArenaBlock block(std::size_t) const`, `std::size_t blockCount() const noexcept`, `std::size_t totalBytes() const noexcept`, `static constexpr std::size_t kAlignmentBytes = 256`.

- [ ] **Step 1: Write the failing test**

Append to `tests/diff/diff_unit_tests.cpp`:

```cpp
#include "diff/grad/arena_layout.hpp"

TEST(DiffArenaLayout, EmptyLayoutIsZeroBytes) {
    ohao::diff::ArenaLayout layout;
    EXPECT_EQ(layout.blockCount(), 0u);
    EXPECT_EQ(layout.totalBytes(), 0u);
}

TEST(DiffArenaLayout, SingleBlockStartsAtZeroAndPadsToAlignment) {
    ohao::diff::ArenaLayout layout;
    const std::size_t idx = layout.add(3);  // 3 floats = 12 bytes

    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(layout.blockCount(), 1u);
    EXPECT_EQ(layout.block(0).offsetBytes, 0u);
    EXPECT_EQ(layout.block(0).sizeBytes, 12u);
    EXPECT_EQ(layout.totalBytes(), ohao::diff::ArenaLayout::kAlignmentBytes);
}

TEST(DiffArenaLayout, SecondBlockIsAligned) {
    ohao::diff::ArenaLayout layout;
    layout.add(3);
    const std::size_t idx = layout.add(64);  // 256 bytes

    EXPECT_EQ(idx, 1u);
    EXPECT_EQ(layout.block(1).offsetBytes, ohao::diff::ArenaLayout::kAlignmentBytes);
    EXPECT_EQ(layout.block(1).sizeBytes, 256u);
    EXPECT_EQ(layout.totalBytes(), ohao::diff::ArenaLayout::kAlignmentBytes + 256u);
}

TEST(DiffArenaLayout, BlocksNeverOverlap) {
    ohao::diff::ArenaLayout layout;
    for (std::size_t n : {1u, 7u, 100u, 4096u, 3u}) {
        layout.add(n);
    }
    for (std::size_t i = 1; i < layout.blockCount(); ++i) {
        const auto prev = layout.block(i - 1);
        const auto cur = layout.block(i);
        EXPECT_GE(cur.offsetBytes, prev.offsetBytes + prev.sizeBytes)
            << "block " << i << " overlaps block " << (i - 1);
    }
}

TEST(DiffArenaLayout, ZeroFloatBlockIsRejected) {
    ohao::diff::ArenaLayout layout;
    EXPECT_EQ(layout.add(0), ohao::diff::ArenaLayout::kInvalidBlock);
    EXPECT_EQ(layout.blockCount(), 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Release -j8 --target diff_unit_tests`
Expected: FAIL — `diff/grad/arena_layout.hpp` not found.

- [ ] **Step 3: Write the header**

Create `ohao/diff/grad/arena_layout.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <vector>

namespace ohao::diff {

struct ArenaBlock {
    std::size_t offsetBytes{0};
    std::size_t sizeBytes{0};
};

/// Suballocation arithmetic for the gradient arena. Pure -- no Vulkan.
///
/// Every gradient and optimizer-state allocation lives in one VkBuffer so that
/// per-iteration zeroing is a single vkCmdFillBuffer rather than N clears.
class ArenaLayout {
public:
    static constexpr std::size_t kAlignmentBytes = 256;
    static constexpr std::size_t kInvalidBlock = static_cast<std::size_t>(-1);

    /// Reserve `floatCount` float32s. Returns the block index, or kInvalidBlock
    /// if floatCount is zero.
    std::size_t add(std::size_t floatCount);

    [[nodiscard]] ArenaBlock block(std::size_t index) const;
    [[nodiscard]] std::size_t blockCount() const noexcept { return m_blocks.size(); }
    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_cursorBytes; }

private:
    std::vector<ArenaBlock> m_blocks;
    std::size_t m_cursorBytes{0};
};

}  // namespace ohao::diff
```

- [ ] **Step 4: Write the implementation**

Create `ohao/diff/grad/arena_layout.cpp`:

```cpp
#include "diff/grad/arena_layout.hpp"

namespace ohao::diff {
namespace {

std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace

std::size_t ArenaLayout::add(std::size_t floatCount) {
    if (floatCount == 0) return kInvalidBlock;

    ArenaBlock b;
    b.offsetBytes = m_cursorBytes;
    b.sizeBytes = floatCount * sizeof(float);

    m_cursorBytes = alignUp(b.offsetBytes + b.sizeBytes, kAlignmentBytes);
    m_blocks.push_back(b);
    return m_blocks.size() - 1;
}

ArenaBlock ArenaLayout::block(std::size_t index) const {
    if (index >= m_blocks.size()) return ArenaBlock{};
    return m_blocks[index];
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cmake --build build --config Release -j8 --target diff_unit_tests
./build/Release/diff_unit_tests.exe --gtest_filter=DiffArenaLayout.*
```
Expected: all 5 PASS.

- [ ] **Step 6: Commit**

```bash
git add ohao/diff/grad/arena_layout.hpp ohao/diff/grad/arena_layout.cpp tests/diff/diff_unit_tests.cpp
git commit -m "feat(diff): gradient arena suballocation layout"
```

---

### Task 3: GPU gradient arena with float atomics proven by execution

The spec calls `shaderBufferFloat32AtomicAdd` a hard device requirement. Task 1 checked the flag; this task proves atomics actually accumulate, which is a different claim.

**Files:**
- Create: `ohao/diff/grad/gradient_arena.hpp`, `ohao/diff/grad/gradient_arena.cpp`, `shaders/diff/atomic_probe.comp`, `tests/diff/diff_gpu_probe.cpp`
- Modify: `tests/diff/CMakeLists.txt`

**Interfaces:**
- Consumes: `ohao::diff::ArenaLayout` (Task 2), `ohao::GpuAllocator` (`ohao/gpu/vulkan/gpu_allocator.hpp`).
- Produces: `ohao::diff::GradientArena` with `bool build(GpuAllocator&, const ArenaLayout&)`, `void zero(VkCommandBuffer) const`, `VkBuffer buffer() const noexcept`, `std::vector<float> readback(GpuAllocator&, std::size_t blockIndex) const`, `void destroy(GpuAllocator&)`.

- [ ] **Step 1: Write the probe shader**

Create `shaders/diff/atomic_probe.comp`:

```glsl
#version 450
#extension GL_EXT_shader_atomic_float : require

// Proves shaderBufferFloat32AtomicAdd actually accumulates under contention.
// Every invocation adds 1.0 to the SAME element, so a correct result is exactly
// the invocation count. A non-atomic read-modify-write loses updates and lands
// well below it.

layout(local_size_x = 64) in;

layout(std430, binding = 0) buffer GradientArena {
    float data[];
} arena;

layout(push_constant) uniform Push {
    uint targetIndex;
    uint invocationCount;
} pc;

void main() {
    if (gl_GlobalInvocationID.x >= pc.invocationCount) return;
    atomicAdd(arena.data[pc.targetIndex], 1.0);
}
```

- [ ] **Step 2: Write the failing probe test**

Create `tests/diff/diff_gpu_probe.cpp`. This is a standalone executable (the repo's pattern for device-requiring checks — see `tests/renderer/renderer_test.cpp`). It returns non-zero on failure.

```cpp
// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    ohao::diff::GpuProbeContext ctx;
    if (!ctx.init()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: could not initialise Vulkan\n");
        return 1;
    }

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(ctx.physicalDevice());
    if (!caps.sufficient()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: device lacks ray query or float atomics\n");
        return 1;
    }

    ohao::diff::ArenaLayout layout;
    const std::size_t blockA = layout.add(16);
    const std::size_t blockB = layout.add(4);

    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), layout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build\n");
        return 1;
    }

    // 1. zero + readback
    ctx.runImmediate([&](VkCommandBuffer cmd) { arena.zero(cmd); });
    for (std::size_t b : {blockA, blockB}) {
        const std::vector<float> values = arena.readback(ctx.allocator(), b);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: block %zu element %zu = %f, expected 0\n",
                             b, i, values[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: arena zero + readback\n");

    // 2. float atomics under contention
    constexpr uint32_t kInvocations = 4096;
    if (!ctx.runAtomicProbe(arena, /*targetIndex=*/0, kInvocations)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomic probe dispatch\n");
        return 1;
    }
    const std::vector<float> after = arena.readback(ctx.allocator(), blockA);
    if (after[0] != static_cast<float>(kInvocations)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: atomicAdd gave %f, expected %u "
                     "(lost updates = non-atomic accumulation)\n",
                     after[0], kInvocations);
        return 1;
    }
    if (after[1] != 0.0f) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomicAdd wrote outside target index\n");
        return 1;
    }
    std::printf("[diff_gpu_probe] OK: atomicAdd accumulated %u contended adds exactly\n",
                kInvocations);

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
```

> **Note for the implementer:** `ohao::diff::GpuProbeContext` is a test-only helper you write in Step 3. It owns a headless `VkInstance`, `VkDevice`, `GpuAllocator`, command pool, and queue, and exposes exactly:
>
> ```cpp
> namespace ohao::diff {
> class GpuProbeContext {
> public:
>     [[nodiscard]] bool init();
>     void shutdown();
>     [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept;
>     [[nodiscard]] GpuAllocator& allocator() noexcept;
>     void runImmediate(const std::function<void(VkCommandBuffer)>& fn);
>     [[nodiscard]] bool runAtomicProbe(GradientArena& arena, uint32_t targetIndex,
>                                       uint32_t invocations);
> };
> }  // namespace ohao::diff
> ```
>
> It must enable the same two extensions and feature structs added to `device_setup.cpp` in Task 1 Step 8 — copy that feature chain verbatim.
>
> `gpu_probe_context.hpp` needs `#include <functional>` explicitly. MSVC will not supply it transitively, and a `std::function` parameter type without it is a known build break in this repo.

- [ ] **Step 3: Write GpuProbeContext**

Create `tests/diff/gpu_probe_context.hpp` and `tests/diff/gpu_probe_context.cpp` implementing the interface described in Step 2. Requirements:

- `init()` creates a `VkInstance` (API 1.3), picks the first discrete GPU, creates a `VkDevice` with `VK_KHR_RAY_QUERY_EXTENSION_NAME` + `VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME` and the `VkPhysicalDeviceRayQueryFeaturesKHR` / `VkPhysicalDeviceShaderAtomicFloatFeaturesEXT` chain from Task 1 Step 8, plus one compute-capable queue and a command pool. It then calls `GpuAllocator::initialize(instance, physicalDevice, device)`.
- `runImmediate(fn)` allocates a primary command buffer, begins it with `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`, calls `fn(cmd)`, ends, submits, and `vkQueueWaitIdle`s.
- `runAtomicProbe(arena, targetIndex, invocations)` loads `bin/shaders/diff_atomic_probe.comp.spv`, creates a descriptor set layout with one `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` at binding 0 plus a push-constant range of 8 bytes, creates the compute pipeline, binds `arena.buffer()`, pushes `{targetIndex, invocations}`, dispatches `(invocations + 63) / 64` groups, inserts a `VK_ACCESS_SHADER_WRITE_BIT -> VK_ACCESS_HOST_READ_BIT` barrier, and waits. Returns false on any Vulkan error.

- [ ] **Step 4: Write GradientArena**

Create `ohao/diff/grad/gradient_arena.hpp`:

```cpp
#pragma once

#include "diff/grad/arena_layout.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <cstddef>
#include <vector>

namespace ohao::diff {

/// One VkBuffer holding every gradient and optimizer-state block.
///
/// Buffer-backed, never image-backed: the scatter path needs
/// shaderBufferFloat32AtomicAdd only, which is materially more available than
/// the image equivalent (design doc S4.3).
class GradientArena {
public:
    [[nodiscard]] bool build(GpuAllocator& allocator, const ArenaLayout& layout);
    void destroy(GpuAllocator& allocator);

    /// Records a single fill over the whole arena. One command, one barrier.
    void zero(VkCommandBuffer cmd) const;

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }
    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_layout.totalBytes(); }

    /// Host-visible copy of one block's floats. Caller must have waited for the
    /// GPU work that wrote it.
    [[nodiscard]] std::vector<float> readback(GpuAllocator& allocator,
                                              std::size_t blockIndex) const;

private:
    ArenaLayout m_layout;
    GpuBuffer m_buffer;
};

}  // namespace ohao::diff
```

Create `ohao/diff/grad/gradient_arena.cpp`:

```cpp
#include "diff/grad/gradient_arena.hpp"

#include <cstring>

namespace ohao::diff {

bool GradientArena::build(GpuAllocator& allocator, const ArenaLayout& layout) {
    if (layout.totalBytes() == 0) return false;

    m_layout = layout;
    // CpuToGpu + persistently mapped keeps readback simple for the scaffolding
    // stage. Stage 1 can move this to GpuOnly with a staging copy once the
    // per-iteration cost matters.
    m_buffer = allocator.createBuffer(
        static_cast<VkDeviceSize>(layout.totalBytes()),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        AllocationUsage::CpuToGpu,
        /*persistentlyMapped=*/true);

    return m_buffer.isValid();
}

void GradientArena::destroy(GpuAllocator& allocator) {
    if (m_buffer.isValid()) allocator.destroyBuffer(m_buffer);
    m_layout = ArenaLayout{};
}

void GradientArena::zero(VkCommandBuffer cmd) const {
    if (!m_buffer.isValid()) return;
    vkCmdFillBuffer(cmd, m_buffer.buffer, 0,
                    static_cast<VkDeviceSize>(m_layout.totalBytes()), 0u);
}

std::vector<float> GradientArena::readback(GpuAllocator& allocator,
                                           std::size_t blockIndex) const {
    std::vector<float> out;
    if (!m_buffer.isValid()) return out;

    const ArenaBlock block = m_layout.block(blockIndex);
    if (block.sizeBytes == 0) return out;

    allocator.invalidateBuffer(const_cast<GpuBuffer&>(m_buffer));
    const auto* base = static_cast<const std::byte*>(m_buffer.getMappedData());
    if (base == nullptr) return out;

    out.resize(block.sizeBytes / sizeof(float));
    std::memcpy(out.data(), base + block.offsetBytes, block.sizeBytes);
    return out;
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Add the probe target to CMake**

Append to `tests/diff/CMakeLists.txt`:

```cmake
add_executable(diff_gpu_probe diff_gpu_probe.cpp gpu_probe_context.cpp)
target_include_directories(diff_gpu_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_gpu_probe PRIVATE cxx_std_20)
target_link_libraries(diff_gpu_probe PRIVATE
    ohao_diff ohao_gpu_vulkan glm VulkanMemoryAllocator ${Vulkan_LIBRARIES})
if(MSVC)
    target_link_options(diff_gpu_probe PRIVATE /FORCE:MULTIPLE)
endif()
add_custom_command(TARGET diff_gpu_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:diff_gpu_probe>/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "$<TARGET_FILE_DIR:diff_gpu_probe>/bin/shaders"
    COMMENT "Copying shader SPVs for diff_gpu_probe"
)
```

- [ ] **Step 6: Run the probe**

Run:
```bash
cmake -B build -S . -G "Visual Studio 17 2022"
cmake --build build --config Release -j8 --target diff_gpu_probe
./build/Release/diff_gpu_probe.exe
```
Expected output:
```
[diff_gpu_probe] OK: arena zero + readback
[diff_gpu_probe] OK: atomicAdd accumulated 4096 contended adds exactly
[diff_gpu_probe] PASS
```
Exit code 0. If `after[0]` comes back below 4096, atomics are not actually atomic — stop and investigate before building anything on top.

- [ ] **Step 7: Commit**

```bash
git add ohao/diff/grad/gradient_arena.hpp ohao/diff/grad/gradient_arena.cpp \
        shaders/diff/atomic_probe.comp tests/diff/diff_gpu_probe.cpp \
        tests/diff/gpu_probe_context.hpp tests/diff/gpu_probe_context.cpp \
        tests/diff/CMakeLists.txt
git commit -m "feat(diff): buffer-backed gradient arena, float atomics proven by dispatch"
```

---

### Task 4: Parameter registry with format enforcement

**Files:**
- Create: `ohao/diff/param/param_registry.hpp`, `ohao/diff/param/param_registry.cpp`
- Test: `tests/diff/diff_unit_tests.cpp` (append)

**Interfaces:**
- Consumes: `ohao::diff::ArenaLayout` (Task 2).
- Produces: `ohao::diff::ParamKind`, `ohao::diff::ParamShape`, `ohao::diff::ParamId`, `ohao::diff::RegisterResult`, `ohao::diff::ParamRegistry` with `RegisterResult registerTexture(std::string, ParamShape, VkFormat)`, `RegisterResult registerScalarBlock(std::string, uint32_t)`, `std::size_t count() const noexcept`, `const DiffParam* find(std::string_view) const`, `const ArenaLayout& layout() const noexcept`.

- [ ] **Step 1: Write the failing test**

Append to `tests/diff/diff_unit_tests.cpp`:

```cpp
#include "diff/param/param_registry.hpp"

TEST(DiffParamRegistry, RejectsEightBitSrgbTexture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3}, VK_FORMAT_R8G8B8A8_SRGB);

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("promoteToFloat"), std::string::npos)
        << "the error must name the remedy, not just the problem; got: " << result.error;
    EXPECT_EQ(reg.count(), 0u);
}

TEST(DiffParamRegistry, RejectsEightBitUnormTexture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3}, VK_FORMAT_R8G8B8A8_UNORM);

    EXPECT_FALSE(result.ok)
        << "8-bit quantisation stalls Adam silently: any step below 1/255 rounds to nothing";
    EXPECT_EQ(reg.count(), 0u);
}

TEST(DiffParamRegistry, AcceptsFloat32Texture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3},
                                            VK_FORMAT_R32G32B32A32_SFLOAT);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(reg.count(), 1u);

    const ohao::diff::DiffParam* p = reg.find("albedo");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, ohao::diff::ParamKind::Texture);
    EXPECT_EQ(p->floatCount, 64u * 64u * 3u);
}

TEST(DiffParamRegistry, RejectsDuplicateName) {
    ohao::diff::ParamRegistry reg;
    ASSERT_TRUE(reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT).ok);

    const auto dup = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(reg.count(), 1u);
}

TEST(DiffParamRegistry, ScalarBlockIsNotATextureSpecialCase) {
    // The registry's primitive is "floats with a gradient block". Neural weights,
    // pose, and geometry all register this way -- textures merely add shape.
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerScalarBlock("ssao_params", 4);

    ASSERT_TRUE(result.ok) << result.error;
    const ohao::diff::DiffParam* p = reg.find("ssao_params");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, ohao::diff::ParamKind::ScalarBlock);
    EXPECT_EQ(p->floatCount, 4u);
}

TEST(DiffParamRegistry, LayoutGrowsWithEachParam) {
    ohao::diff::ParamRegistry reg;
    EXPECT_EQ(reg.layout().blockCount(), 0u);

    ASSERT_TRUE(reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT).ok);
    ASSERT_TRUE(reg.registerScalarBlock("ssao_params", 4).ok);

    EXPECT_EQ(reg.layout().blockCount(), 2u);
    EXPECT_GT(reg.layout().totalBytes(), 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Release -j8 --target diff_unit_tests`
Expected: FAIL — `diff/param/param_registry.hpp` not found.

- [ ] **Step 3: Write the header**

Create `ohao/diff/param/param_registry.hpp`:

```cpp
#pragma once

#include "diff/grad/arena_layout.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::diff {

enum class ParamKind : std::uint8_t {
    Texture,         // shaped, bilinear scatter
    ScalarBlock,     // flat floats: BSDF scalars, SSAO knobs, MLP weights
    VertexPositions, // stage 3
};

struct ParamShape {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channels{0};

    [[nodiscard]] std::uint32_t floatCount() const noexcept {
        return width * height * channels;
    }
};

struct ParamId {
    std::uint32_t value{0xFFFFFFFFu};
    [[nodiscard]] bool valid() const noexcept { return value != 0xFFFFFFFFu; }
};

struct DiffParam {
    std::string name;
    ParamKind kind{ParamKind::ScalarBlock};
    ParamShape shape{};
    std::uint32_t floatCount{0};
    std::size_t gradBlock{ArenaLayout::kInvalidBlock};
    std::size_t stateBlock{ArenaLayout::kInvalidBlock};  // Adam m and v, 2x floatCount
};

struct RegisterResult {
    bool ok{false};
    ParamId id{};
    std::string error;
};

/// Registry of everything being differentiated.
///
/// The primitive is a float buffer with a matching gradient block. Textures are
/// not a special case -- they carry shape metadata for the bilinear scatter.
class ParamRegistry {
public:
    /// Primal must be a float format. 8-bit storage is rejected: a gradient step
    /// below 1/255 rounds to nothing, so the optimizer stalls in a way that is
    /// indistinguishable from convergence.
    RegisterResult registerTexture(std::string name, ParamShape shape, VkFormat primalFormat);

    RegisterResult registerScalarBlock(std::string name, std::uint32_t floatCount);

    [[nodiscard]] std::size_t count() const noexcept { return m_params.size(); }
    [[nodiscard]] const DiffParam* find(std::string_view name) const;
    [[nodiscard]] const DiffParam* get(ParamId id) const;
    [[nodiscard]] const ArenaLayout& layout() const noexcept { return m_layout; }

    /// True when the format stores enough precision to be optimized.
    [[nodiscard]] static bool isDifferentiableFormat(VkFormat format) noexcept;

private:
    RegisterResult addParam(std::string name, ParamKind kind, ParamShape shape,
                            std::uint32_t floatCount);

    std::vector<DiffParam> m_params;
    ArenaLayout m_layout;
};

}  // namespace ohao::diff
```

- [ ] **Step 4: Write the implementation**

Create `ohao/diff/param/param_registry.cpp`:

```cpp
#include "diff/param/param_registry.hpp"

namespace ohao::diff {

bool ParamRegistry::isDifferentiableFormat(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16_SFLOAT:
        return true;
    default:
        return false;
    }
}

RegisterResult ParamRegistry::addParam(std::string name, ParamKind kind, ParamShape shape,
                                       std::uint32_t floatCount) {
    RegisterResult result;

    if (name.empty()) {
        result.error = "parameter name must not be empty";
        return result;
    }
    if (floatCount == 0) {
        result.error = "parameter '" + name + "' has zero floats";
        return result;
    }
    if (find(name) != nullptr) {
        result.error = "parameter '" + name + "' is already registered";
        return result;
    }

    DiffParam p;
    p.name = std::move(name);
    p.kind = kind;
    p.shape = shape;
    p.floatCount = floatCount;
    p.gradBlock = m_layout.add(floatCount);
    p.stateBlock = m_layout.add(floatCount * 2u);  // Adam m and v

    m_params.push_back(std::move(p));

    result.ok = true;
    result.id.value = static_cast<std::uint32_t>(m_params.size() - 1);
    return result;
}

RegisterResult ParamRegistry::registerTexture(std::string name, ParamShape shape,
                                              VkFormat primalFormat) {
    if (!isDifferentiableFormat(primalFormat)) {
        RegisterResult result;
        result.error =
            "parameter '" + name +
            "': primal texture is not a float format. 8-bit storage cannot be optimized "
            "(an Adam step below 1/255 rounds to nothing and the fit stalls silently). "
            "Call promoteToFloat(handle) on the bindless texture first.";
        return result;
    }
    return addParam(std::move(name), ParamKind::Texture, shape, shape.floatCount());
}

RegisterResult ParamRegistry::registerScalarBlock(std::string name, std::uint32_t floatCount) {
    return addParam(std::move(name), ParamKind::ScalarBlock, ParamShape{}, floatCount);
}

const DiffParam* ParamRegistry::find(std::string_view name) const {
    for (const auto& p : m_params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const DiffParam* ParamRegistry::get(ParamId id) const {
    if (!id.valid() || id.value >= m_params.size()) return nullptr;
    return &m_params[id.value];
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cmake --build build --config Release -j8 --target diff_unit_tests
./build/Release/diff_unit_tests.exe --gtest_filter=DiffParamRegistry.*
```
Expected: all 6 PASS.

- [ ] **Step 6: Commit**

```bash
git add ohao/diff/param/param_registry.hpp ohao/diff/param/param_registry.cpp \
        tests/diff/diff_unit_tests.cpp
git commit -m "feat(diff): parameter registry with float-format enforcement"
```

---

### Task 5: Replayable path RNG

The seed invariant, made concrete. Path replay backpropagation requires the backward pass to walk exactly the path the forward pass walked, so the RNG must be a pure function of `(pixel, sampleIndex, iterationSeed)` with no carried state.

This mirrors the existing `SobolGenerator` pattern (`ohao/render/rt/sobol_generator.hpp`): a CPU reference whose math the GLSL mirrors exactly, with unit tests on the CPU side.

**Files:**
- Create: `ohao/diff/rng/diff_rng.hpp`, `ohao/diff/rng/diff_rng.cpp`, `shaders/includes/diff/rng.glsl`
- Test: `tests/diff/diff_unit_tests.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `ohao::diff::PathRng` with `static PathRng forPath(uint32_t pixelIndex, uint32_t sampleIndex, uint32_t iterationSeed)`, `float next1D()`, `uint32_t drawCount() const noexcept`.

- [ ] **Step 1: Write the failing test**

Append to `tests/diff/diff_unit_tests.cpp`:

```cpp
#include "diff/rng/diff_rng.hpp"

TEST(DiffPathRng, SameTupleProducesIdenticalStream) {
    auto a = ohao::diff::PathRng::forPath(1234, 7, 99);
    auto b = ohao::diff::PathRng::forPath(1234, 7, 99);

    for (int i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(a.next1D(), b.next1D()) << "divergence at draw " << i;
    }
}

TEST(DiffPathRng, ReplayFromTupleReproducesTheStream) {
    // This is the seed invariant that path replay backpropagation depends on:
    // the backward pass reconstructs the RNG from the tuple alone and must
    // walk the same path the forward pass walked.
    auto forward = ohao::diff::PathRng::forPath(4096, 3, 12345);
    std::vector<float> forwardDraws;
    for (int i = 0; i < 16; ++i) forwardDraws.push_back(forward.next1D());

    auto backward = ohao::diff::PathRng::forPath(4096, 3, 12345);
    for (int i = 0; i < 16; ++i) {
        EXPECT_FLOAT_EQ(backward.next1D(), forwardDraws[static_cast<std::size_t>(i)])
            << "replay diverged at draw " << i;
    }
}

TEST(DiffPathRng, DifferentPixelsDecorrelate) {
    auto a = ohao::diff::PathRng::forPath(10, 0, 1);
    auto b = ohao::diff::PathRng::forPath(11, 0, 1);

    int identical = 0;
    for (int i = 0; i < 16; ++i) {
        if (a.next1D() == b.next1D()) ++identical;
    }
    EXPECT_LT(identical, 3) << "neighbouring pixels are producing correlated streams";
}

TEST(DiffPathRng, DifferentSeedsDecorrelate) {
    auto a = ohao::diff::PathRng::forPath(10, 0, 1);
    auto b = ohao::diff::PathRng::forPath(10, 0, 2);

    int identical = 0;
    for (int i = 0; i < 16; ++i) {
        if (a.next1D() == b.next1D()) ++identical;
    }
    EXPECT_LT(identical, 3);
}

TEST(DiffPathRng, DrawsAreInUnitInterval) {
    auto rng = ohao::diff::PathRng::forPath(777, 5, 42);
    for (int i = 0; i < 4096; ++i) {
        const float v = rng.next1D();
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
    }
}

TEST(DiffPathRng, DrawCountTracksConsumption) {
    // Forward and backward must consume the same number of draws. A mismatch
    // here is the failure mode that silently corrupts every gradient, so the
    // counter exists to be asserted on in later stages.
    auto rng = ohao::diff::PathRng::forPath(1, 1, 1);
    EXPECT_EQ(rng.drawCount(), 0u);
    rng.next1D();
    rng.next1D();
    EXPECT_EQ(rng.drawCount(), 2u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Release -j8 --target diff_unit_tests`
Expected: FAIL — `diff/rng/diff_rng.hpp` not found.

- [ ] **Step 3: Write the header**

Create `ohao/diff/rng/diff_rng.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace ohao::diff {

/// PCG-based path RNG. A path is a pure function of (pixel, sample, seed).
///
/// The forward and backward kernels reconstruct this from the tuple alone --
/// no state crosses the kernel boundary. If the two ever consume a different
/// number of draws, the replayed path is a DIFFERENT path and every gradient
/// is silently wrong, which is why drawCount() exists.
///
/// shaders/includes/diff/rng.glsl mirrors this math exactly. Change both or
/// neither.
class PathRng {
public:
    [[nodiscard]] static PathRng forPath(std::uint32_t pixelIndex,
                                         std::uint32_t sampleIndex,
                                         std::uint32_t iterationSeed) noexcept;

    /// Next uniform value in [0, 1).
    [[nodiscard]] float next1D() noexcept;

    [[nodiscard]] std::uint32_t drawCount() const noexcept { return m_draws; }

private:
    std::uint32_t m_state{0};
    std::uint32_t m_draws{0};
};

}  // namespace ohao::diff
```

- [ ] **Step 4: Write the implementation**

Create `ohao/diff/rng/diff_rng.cpp`:

```cpp
#include "diff/rng/diff_rng.hpp"

namespace ohao::diff {
namespace {

// PCG hash. Mirrored verbatim in shaders/includes/diff/rng.glsl.
std::uint32_t pcgHash(std::uint32_t input) noexcept {
    std::uint32_t state = input * 747796405u + 2891336453u;
    std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

}  // namespace

PathRng PathRng::forPath(std::uint32_t pixelIndex, std::uint32_t sampleIndex,
                         std::uint32_t iterationSeed) noexcept {
    PathRng rng;
    // Hash each component in turn so no two tuples collide cheaply.
    rng.m_state = pcgHash(pixelIndex ^ pcgHash(sampleIndex ^ pcgHash(iterationSeed)));
    rng.m_draws = 0;
    return rng;
}

float PathRng::next1D() noexcept {
    m_state = pcgHash(m_state);
    ++m_draws;
    // 24 mantissa bits -> [0, 1). Dividing by 2^24 cannot round up to 1.0f.
    return static_cast<float>(m_state >> 8u) * (1.0f / 16777216.0f);
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Write the GLSL mirror**

Create `shaders/includes/diff/rng.glsl`:

```glsl
#ifndef OHAO_DIFF_RNG_GLSL
#define OHAO_DIFF_RNG_GLSL

// GLSL mirror of ohao/diff/rng/diff_rng.cpp. The CPU side is unit-tested;
// this must produce bit-identical values. Change both or neither.
//
// A path is a pure function of (pixel, sample, seed). Forward and backward
// kernels reconstruct the stream from the tuple, never by carrying state.

struct DiffPathRng {
    uint state;
    uint draws;
};

uint diffPcgHash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

DiffPathRng diffRngForPath(uint pixelIndex, uint sampleIndex, uint iterationSeed) {
    DiffPathRng rng;
    rng.state = diffPcgHash(pixelIndex ^ diffPcgHash(sampleIndex ^ diffPcgHash(iterationSeed)));
    rng.draws = 0u;
    return rng;
}

float diffRngNext1D(inout DiffPathRng rng) {
    rng.state = diffPcgHash(rng.state);
    rng.draws += 1u;
    return float(rng.state >> 8u) * (1.0 / 16777216.0);
}

#endif  // OHAO_DIFF_RNG_GLSL
```

- [ ] **Step 6: Run tests to verify they pass**

Run:
```bash
cmake --build build --config Release -j8 --target diff_unit_tests
./build/Release/diff_unit_tests.exe --gtest_filter=DiffPathRng.*
```
Expected: all 6 PASS.

- [ ] **Step 7: Commit**

```bash
git add ohao/diff/rng/diff_rng.hpp ohao/diff/rng/diff_rng.cpp \
        shaders/includes/diff/rng.glsl tests/diff/diff_unit_tests.cpp
git commit -m "feat(diff): replayable path RNG with CPU reference and GLSL mirror"
```

---

### Task 6: Ray-query visibility against closed-form geometry

First use of `rayQueryEXT`. Validated against a scene whose intersection distances are known analytically — the gate-2 philosophy from the spec applied to visibility rather than to a gradient.

**Files:**
- Create: `shaders/diff/visibility_probe.comp`
- Modify: `tests/diff/diff_gpu_probe.cpp`, `tests/diff/gpu_probe_context.hpp`, `tests/diff/gpu_probe_context.cpp`

**Interfaces:**
- Consumes: `ohao::diff::GradientArena` (Task 3), `ohao::RTAccelerationStructure` (`ohao/render/rt/rt_acceleration_structure.hpp`).
- Produces: nothing consumed by later tasks in this plan — this is a validation milestone.

- [ ] **Step 1: Write the probe shader**

Create `shaders/diff/visibility_probe.comp`:

```glsl
#version 460
#extension GL_EXT_ray_query : require

// Ray-query primary visibility. Inline tracing keeps the whole path inside one
// function, which is what lets the forward and backward kernels share a single
// traversal source in stage 1.
//
// Writes hit distance per pixel; -1.0 on miss.

layout(local_size_x = 8, local_size_y = 8) in;

layout(std430, binding = 0) buffer HitBuffer {
    float t[];
} hits;

layout(binding = 1) uniform accelerationStructureEXT topLevelAS;

layout(push_constant) uniform Push {
    vec3 origin;
    float pad0;
    vec3 forward;
    float pad1;
    vec3 right;
    float pad2;
    vec3 up;
    float pad3;
    uint width;
    uint height;
    float tanHalfFov;
    float pad4;
} pc;

void main() {
    const uvec2 pix = gl_GlobalInvocationID.xy;
    if (pix.x >= pc.width || pix.y >= pc.height) return;

    const uint index = pix.y * pc.width + pix.x;

    // Pixel centre in NDC, y down to match image row order.
    const float ndcX = (2.0 * (float(pix.x) + 0.5) / float(pc.width) - 1.0);
    const float ndcY = (1.0 - 2.0 * (float(pix.y) + 0.5) / float(pc.height));
    const float aspect = float(pc.width) / float(pc.height);

    const vec3 dir = normalize(pc.forward
                             + pc.right * (ndcX * aspect * pc.tanHalfFov)
                             + pc.up    * (ndcY * pc.tanHalfFov));

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF,
                          pc.origin, 0.001, dir, 1000.0);
    while (rayQueryProceedEXT(rq)) { }

    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        hits.t[index] = -1.0;
    } else {
        hits.t[index] = rayQueryGetIntersectionTEXT(rq, true);
    }
}
```

- [ ] **Step 2: Extend GpuProbeContext with the visibility check**

Add to `tests/diff/gpu_probe_context.hpp` a method:

```cpp
    /// Builds a BLAS/TLAS for a single axis-aligned quad spanning
    /// x,y in [-1,1] at z = -planeDistance, traces one ray per pixel from the
    /// origin looking down -Z, and fills `outHits` with width*height distances.
    /// Returns false on any Vulkan error.
    bool runVisibilityProbe(float planeDistance, uint32_t width, uint32_t height,
                            float tanHalfFov, std::vector<float>& outHits);
```

Implement it in `gpu_probe_context.cpp`:

- Upload two triangles forming the quad (vertices `(-1,-1,-d)`, `(1,-1,-d)`, `(1,1,-d)`, `(-1,1,-d)`; indices `0,1,2, 0,2,3`) into device-local vertex and index buffers via `GpuAllocator::createBufferFromSpan` with `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.
- Use `ohao::RTAccelerationStructure`: `init(device, physicalDevice, allocator, queueFamily, queue)`, `createBLAS(...)`, `clearInstances()`, `addInstance(blas, glm::mat4(1.0f))`, then `buildTLAS(cmd)` inside a `runImmediate`.
- Create a compute pipeline from `bin/shaders/diff_visibility_probe.comp.spv` with a descriptor set layout of `{binding 0: STORAGE_BUFFER, binding 1: ACCELERATION_STRUCTURE_KHR}` and a push constant range sized to the `Push` block above (80 bytes).
- Push `origin = (0,0,0)`, `forward = (0,0,-1)`, `right = (1,0,0)`, `up = (0,1,0)`, the supplied `width`, `height`, `tanHalfFov`.
- Dispatch `((width + 7) / 8, (height + 7) / 8, 1)`, barrier, wait, read back into `outHits`.

- [ ] **Step 3: Write the failing assertions**

Insert into `tests/diff/diff_gpu_probe.cpp`, before `arena.destroy(...)`:

```cpp
    // 3. Ray-query visibility against a plane whose intersections are analytic.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 64;
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;  // narrow, so every ray hits the quad

    std::vector<float> hitsT;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, hitsT)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: visibility probe dispatch\n");
        return 1;
    }
    if (hitsT.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: hit buffer size %zu, expected %u\n",
                     hitsT.size(), kW * kH);
        return 1;
    }

    // The centre pixel looks straight down -Z, so t is exactly the plane distance.
    const std::size_t centre = static_cast<std::size_t>(kH / 2) * kW + (kW / 2);
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-3f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return 1;
    }

    // Off-axis rays are longer by exactly 1/cos(theta), and every ray must hit.
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            if (hitsT[i] < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) missed a quad that "
                             "covers the whole frustum\n", x, y);
                return 1;
            }
            const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
            const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
            const float dx = ndcX * kTanHalfFov;  // aspect is 1
            const float dy = ndcY * kTanHalfFov;
            const float expected = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
            if (std::fabs(hitsT[i] - expected) > 2e-3f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) t = %f, closed form %f\n",
                             x, y, hitsT[i], expected);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: ray query matches closed-form plane intersection "
                "over %u pixels\n", kW * kH);
```

Add `#include <cmath>` and `#include <vector>` to the probe's includes.

- [ ] **Step 4: Run the probe to verify it fails**

Run:
```bash
cmake --build build --config Release -j8 --target diff_gpu_probe
./build/Release/diff_gpu_probe.exe
```
Expected: FAIL — `runVisibilityProbe` is not yet implemented / the SPV is missing.

- [ ] **Step 5: Implement and re-run**

Complete the Step 2 implementation, then:
```bash
cmake --build build --config Release -j8 --target shaders
cmake --build build --config Release -j8 --target diff_gpu_probe
./build/Release/diff_gpu_probe.exe
```
Expected:
```
[diff_gpu_probe] OK: arena zero + readback
[diff_gpu_probe] OK: atomicAdd accumulated 4096 contended adds exactly
[diff_gpu_probe] OK: ray query matches closed-form plane intersection over 4096 pixels
[diff_gpu_probe] PASS
```

- [ ] **Step 6: Commit**

```bash
git add shaders/diff/visibility_probe.comp tests/diff/diff_gpu_probe.cpp \
        tests/diff/gpu_probe_context.hpp tests/diff/gpu_probe_context.cpp
git commit -m "feat(diff): ray-query visibility validated against closed-form geometry"
```

---

## Stage 0a exit criteria

All must hold before Stage 0b begins:

- [ ] `diff_unit_tests` passes (device caps, arena layout, param registry, path RNG).
- [ ] `diff_gpu_probe` exits 0 with all three OK lines.
- [ ] `renderer_test` still runs — the device changes did not regress the existing renderer.
- [ ] `VK_KHR_ray_query` and `VK_EXT_shader_atomic_float` are enabled in `device_setup.cpp` and their feature bits set.

## What Stage 0a deliberately does not do

- **No gradients.** Nothing here differentiates anything.
- **No radiance.** `visibility_probe.comp` returns hit distance, not colour.
- **No shared traversal.** `traverse.glsl` and the `VERTEX_HOOK` structure (Spec §6.2) arrive in Stage 0b, once there is a real integrator to factor.
- **No `promoteToFloat`.** Spec §4.3 requires primals to live in float32 linear images injected into the bindless array via `BindlessTextureManager::registerExternalTexture`. Stage 0a only *validates* the format in `ParamRegistry::registerTexture` — it does not create such an image, and `promoteToFloat(handle)` is named in the rejection message but not yet implemented. It arrives with the first real texture parameter in Stage 1, which is the first code that needs one. If an implementer trips over the dangling name before then, that is the reason.
- **No parity with the existing path tracer.** That is the Stage 0 gate proper and belongs to Stage 0b, which ports the raygen + closesthit light transport into the megakernel and compares against `PathTracer` output within Monte Carlo noise.

## Next plan

**Stage 0b — forward megakernel and PT parity.** Port BSDF evaluation, NEE, MIS, and environment importance sampling from `shaders/rt/pt_raygen.rgen` and `shaders/rt/pt_closesthit.rchit` into a single ray-query compute kernel structured around `traverse.glsl` + `VERTEX_HOOK`, then gate on matching `PathTracer` output within MC noise on the Cornell box. Write that plan after Stage 0a lands, when the kernel's real structure is known rather than guessed.
