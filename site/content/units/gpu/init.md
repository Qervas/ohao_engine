---
module: gpu
id: init
title: Device init
standard: v2
---

## A Vulkan device with nowhere to present

The first surprising thing about OHAO's boot sequence is what it never asks for.
There is no `VK_KHR_surface`, no `VK_KHR_swapchain`, no
`glfwGetRequiredInstanceExtensions`, and no `vkCreateSwapchainKHR` anywhere in
`ohao/` or `examples/`. The instance is a plain Vulkan 1.3 application info block
with an empty extension list; the only two extensions it can ever add are
`VK_EXT_debug_utils` when `OHAO_VALIDATION` is set in the environment, and
`VK_KHR_get_physical_device_properties2` in a DLSS-enabled build.

That propagates all the way to the interactive viewer: `examples/interactive.cpp`
opens a GLFW OpenGL window and pushes CPU-side pixels through `glTexSubImage2D`
every frame, because the renderer's output contract is a byte buffer, not a
presented image. Frame resources budget for exactly that — one persistently-mapped
host-visible staging buffer per in-flight frame, sized width × height × 4.

{{cite ohao/gpu/vulkan/device_setup.cpp "size_t stagingBufferSize = m_width * m_height * 4;"}}

:::why
The rejected alternative is a WSI swapchain with present-mode negotiation, surface
format selection, and resize handling. Offscreen-only buys a renderer that runs
identically headless (`cornell_box`, `turntable`, the smoke test) and on a desktop;
the only trace of WSI left in the tree is an undefined `cleanupSwapchain()`
declaration in `renderer.hpp`. The cost is paid every displayed frame: a
VRAM → host → GL round trip instead of a zero-copy present.
:::

## Picking a GPU by type, then demanding it be an RT GPU

Device selection scores nothing. It walks the enumerated devices, takes the first
queue family with `VK_QUEUE_GRAPHICS_BIT`, and keeps the candidate if no discrete
GPU has been seen yet *or* if this one is discrete — so the effect is "the last
discrete GPU enumerated wins, otherwise the last graphics-capable device wins".

{{cite ohao/gpu/vulkan/device_setup.cpp "if (!foundDiscrete || isDiscrete) {"}}

No VRAM size, no device-local heap check, and — the important omission — no
verification that the winner supports ray tracing. The device extension list is then
built unconditionally: acceleration structure, ray tracing pipeline, deferred host
operations, buffer device address, descriptor indexing, SPIR-V 1.4 and shader float
controls, all hard requirements.

A second block below it does the opposite — it enumerates what the device advertises
and pushes the three NGX extensions only if it finds them. But that block sits under
`OHAO_DLSS_ENABLED`, which CMake defines only when the vendored NGX static library
under `external/DLSS/` is present, and that directory is gitignored. A clean checkout
compiles this function with exactly one policy: everything or nothing.

The consequence: on a machine whose selected GPU lacks RT, `vkCreateDevice` returns
`VK_ERROR_EXTENSION_NOT_PRESENT`, `initialize()` returns false, and the *entire*
renderer is unavailable — not just path tracing.
`RTAccelerationStructure::init` does contain a graceful capability probe for
acceleration structure and ray tracing pipeline, but it runs *after* device creation
already made them mandatory. The soft-fail path survives only for its other failure mode, a
`vkGetDeviceProcAddr` that comes back null.

{{cite ohao/render/rt/rt_acceleration_structure.cpp "if (!hasAS || !hasRTPipeline) {"}}

Exactly one queue is created, at index 0 of that graphics family, and everything
runs on it: rasterization, RT dispatch, compute post-process, and every staging
copy. An `AsyncComputeQueue` class exists in `ohao/render/async/` and is compiled
into `ohao_renderer` by the source glob, but nothing constructs one — its only
reference in the tree is `tests/renderer/renderer_pipeline_tests.cpp`, a file no
CMake target builds. The device create info never asks for a second queue either.

{{cite ohao/gpu/vulkan/device_setup.cpp "vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);"}}

## Four extensions nothing calls

Also in that mandatory block are `VK_KHR_external_memory` and
`VK_KHR_external_semaphore`, each with its platform pair (`_win32` on Windows, `_fd`
elsewhere), commented as Vulkan-CUDA interop.

{{cite ohao/gpu/vulkan/device_setup.cpp "VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,            // Vulkan-CUDA interop"}}

Nothing uses them. `VkExportMemoryAllocateInfo`, `VkExternalMemoryHandleTypeFlagBits`
and `vkGetMemoryFdKHR` appear nowhere in `ohao/`, `examples/` or `tests/` — not even
in the file that enables the extensions. Their consumer was the OptiX denoiser, since
removed: `--denoise=optix` prints a deprecation line and falls back to OIDN, which
runs on `oidn::DeviceType::Default` over host arrays with no shared allocation. Four
extensions are hard-required to serve a deleted feature, and because they sit in the
unconditional list, a driver missing `VK_KHR_external_semaphore_fd` fails the whole
renderer over a capability it never exercises.

## The feature chain is built tail-first

The `pNext` chain is assembled in reverse of the order the structs are declared:
`features12` is written first but linked last, and each subsequent struct points
back at the one above it, so the final chain reaching `vkCreateDevice` is
`VkPhysicalDeviceFeatures2` → ray tracing pipeline → acceleration structure →
Vulkan 1.3 → Vulkan 1.2.

{{cite ohao/gpu/vulkan/device_setup.cpp "features13.pNext = &features12;"}}

The 1.3 struct is a pure link. Its comment names dynamic rendering and
synchronization2, but `sType` and `pNext` are the only fields assigned, and the tree
contains no `vkCmdBeginRendering`, `vkCmdPipelineBarrier2` or `vkQueueSubmit2` — the
engine is on classic render passes and VK1.0-style barriers throughout.

The Vulkan 1.2 bits do the work. Of eight assigned fields, seven are
descriptor-indexing features; the eighth, `bufferDeviceAddress`, is required by
acceleration structure builds. `runtimeDescriptorArray` plus
`shaderSampledImageArrayNonUniformIndexing` let shaders declare an unsized
`uniform sampler2D textures[]` and index it with `nonuniformEXT(...)`, which
`forward.frag`, `gbuffer.frag`, `pt_closesthit.rchit`, `pt_anyhit.rahit` and
`pt_miss.rmiss` all do. Three more — `descriptorBindingPartiallyBound`,
`descriptorBindingSampledImageUpdateAfterBind`,
`descriptorBindingVariableDescriptorCount` — are precisely the three binding flags
`BindlessTextureManager` sets on its single variable-count texture binding.

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;"}}

:::key
Whether bindless texturing is possible at all is decided here, in the `features12`
block — not in the texture manager that uses it. Neither file queries
`maxDescriptorSetUpdateAfterBindSampledImages` before sizing that array, so an
unsupported configuration surfaces as a descriptor-layout failure in
`BindlessTextureManager`, a file away from the line that caused it.
:::

## Why the enabled extension list is a member variable

`m_enabledInstanceExtensions` and `m_enabledDeviceExtensions` outlive
`createLogicalDevice` for one reason, and the code states it: NRD's NRI backend does
not create a device, it wraps the one OHAO already made, and must be told the exact
extension set that device was created with. The renderer hands both lists to every RT
profile it constructs.

{{cite ohao/gpu/vulkan/device_setup.cpp "// device wrapper (which needs to see the exact enabled list) can read it."}}

Lifetime is not the reason. `NrdDenoiser::initialize` copies both lists into its own
storage before pointing `nri::DeviceCreationVKDesc` at it, NRI copies the pointers
again into its own desired-extension vectors, and the names are string literals from
the Vulkan headers anyway — the caller's vectors need only survive the call.

That copy loop drops exactly the three RT extensions from the device list. NRI's
dispatch-table resolution sees `VK_KHR_ray_tracing_pipeline` and
eagerly tries to resolve `vkCmdTraceRaysIndirect2KHR`, which lives in
`VK_KHR_ray_tracing_maintenance1` — an extension OHAO does not enable. REBLUR is
compute-only and touches no RT entry point, so hiding those three names is what lets
NRI skip the RT path instead of failing to resolve a function.

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "if (!isRtExt(e)) m_impl->deviceExtensionsStorage.push_back(e);"}}

## The order inside initialize() that is not cosmetic

`initialize()` reads as a flat list of `if (!createX()) return false`, but two of
the steps have real dependencies that a reordering would break silently rather than
loudly.

- Shadow resources must precede descriptor sets. `createDescriptorSets` writes
  `m_shadowImageView` and `m_shadowSampler` into binding 2 with no null check; run
  it first and the set is written with `VK_NULL_HANDLE`. The layout is indifferent —
  `createDescriptorSetLayout` never reads a shadow handle.

{{cite ohao/gpu/vulkan/pipeline.cpp "shadowMapInfo.imageView = m_shadowImageView;"}}

- `BindlessTextureManager` must precede `createPipeline`. The pipeline layout
  appends the bindless descriptor set layout only if the manager exists; without it,
  the forward pipeline is built with one set and `forward.frag`'s
  `set = 1, binding = 0` array has nothing behind it.

{{cite ohao/gpu/vulkan/pipeline.cpp "layouts.push_back(m_textureManager->getDescriptorSetLayout());"}}

A third step is notable for its absence rather than its position. `initialize()`
builds only the acceleration-structure manager; the realtime and offline
`PathTracer`s are constructed lazily on `setRenderMode`, because each holds beauty
plus a full AOV set at render resolution and building both eagerly can exhaust VRAM
at 4K. `ensureRTRenderer` pays for that by re-running `updateSceneBuffers()` after a
profile is created, which would otherwise start with empty material, texture, light
and AS buffers.

{{cite ohao/gpu/vulkan/renderer.cpp "// Lazy-create realtime/offline PathTracers on setRenderMode(). Eager dual"}}

## Contracts

- A selected GPU without `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_tracing_pipeline` fails `vkCreateDevice`, not pipeline creation — the
  whole renderer aborts, and the "continuing without RT" fallback never runs for
  that case.
- `m_enabledDeviceExtensions` must still hold exactly what `vkCreateDevice` was given
  when an RT profile is constructed; NRI resolves its dispatch table from that list.
  It need not outlive that call — `NrdDenoiser` copies before it stores.
- Shadow resources must be created before `createDescriptorSets`, or binding 2 is
  written with a null image view.
- `BindlessTextureManager::initialize` must complete before `createPipeline`, or the
  bindless descriptor set layout is missing from the pipeline layout.
- `createSyncObjects` still creates a single legacy `VkFence`; the per-frame
  `renderFence`s live in `FrameResourceManager` and the two coexist.
- Only one queue is created. A second needs a change to the queue create info
  (`queueCount`, or another `VkDeviceQueueCreateInfo`), not just a `vkGetDeviceQueue`
  call.
