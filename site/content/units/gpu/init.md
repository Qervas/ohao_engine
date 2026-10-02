---
module: gpu
id: init
title: Device init
standard: v2
---

## Presentation is something the host hands in

The renderer never includes a windowing header. It boots one of two ways: `initialize()`
builds a headless device whose output is a CPU byte buffer, and
`initialize(const PresentTarget&)` stores what a windowing system handed it and then
runs the same sequence. A `PresentTarget` is four things — the instance extensions the
window system needs, a callback that creates a `VkSurfaceKHR` from an instance, the
drawable extent, and a vsync flag.

{{cite ohao/gpu/vulkan/present_target.hpp "std::function<VkSurfaceKHR(VkInstance)> createSurface;"}}

The instance is a plain Vulkan 1.3 application info block. Headless, its extension
list is empty unless `OHAO_VALIDATION` is set in the environment (which adds
`VK_EXT_debug_utils`) or the build is DLSS-enabled (which adds
`VK_KHR_get_physical_device_properties2`); windowed, the target's extensions are
merged in first, de-duplicated by name. The surface is created immediately after the
instance, because it has to exist before a device is picked: device selection skips
any graphics queue family that cannot present to it, and `VK_KHR_swapchain` joins the
device extension list only when a surface exists.

{{cite ohao/gpu/vulkan/device_setup.cpp "if (m_surface != VK_NULL_HANDLE) m_enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);"}}

Frame resources are sized for the headless contract either way — one
persistently-mapped host-visible staging buffer per in-flight frame, width × height ×
4 bytes. A windowed renderer allocates them and never reads them; its frames end in a
blit into the swapchain image.

{{cite ohao/gpu/vulkan/device_setup.cpp "const size_t stagingBufferSize = static_cast<size_t>(m_width) * m_height * 4;   // RGBA8"}}

Validation is opt-in at the same step. With `OHAO_VALIDATION` set, `createInstance`
checks that `VK_LAYER_KHRONOS_validation` is actually installed before enabling it,
then installs the engine's own debug messenger, which counts every warning and error
and keeps the first 256 messages. `validationEnabled()` reports false both when the
variable is unset and when the layer is missing, so a caller must read it as "not
checked", never as "clean".

{{cite ohao/gpu/vulkan/device_setup.cpp "m_validationEnabled = create && create(m_instance, &mi, nullptr, &m_debugMessenger) == VK_SUCCESS;"}}

:::why
Presentation is an option on the renderer, not its default, so that one renderer runs
identically headless (`cornell_box`, `turntable`, the smoke test, the golden corpus)
and in a window. The window system stays outside `ohao/gpu`: `PresentTarget` carries
a surface-creation callback rather than a window handle, so the renderer depends on
`VkSurfaceKHR` and nothing else, and the GLFW-specific half lives in `ohao/runtime`.
:::

## Picking a GPU by type, then demanding it be an RT GPU

Device selection scores nothing. It walks the enumerated devices, takes the first
queue family with `VK_QUEUE_GRAPHICS_BIT` (and, when a surface exists, the first
such family that can present to it), and keeps the candidate if no discrete
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
runs on it: rasterization, RT dispatch, compute post-process, every staging
copy, and presentation. An `AsyncComputeQueue` class exists in `ohao/render/async/` and is compiled
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
back at the one above it, so the mandatory part of the chain reaching
`vkCreateDevice` is `VkPhysicalDeviceFeatures2` → ray tracing pipeline →
acceleration structure → Vulkan 1.3 → Vulkan 1.2. Ray query and buffer float atomics,
both optional, are linked in front of the ray tracing pipeline struct only when their
extensions were found and enabled — a feature struct in the chain for an extension
that is not enabled is invalid regardless of its bits.

{{cite ohao/gpu/vulkan/device_setup.cpp "features13.pNext = &features12;"}}

The 1.3 struct sets one field, `shaderDemoteToHelperInvocation`, and it is not
optional in practice. `glslc` targeting SPIR-V 1.6 lowers `discard` to
`OpDemoteToHelperInvocation`, so every shader that discards — deferred lighting, sky,
particles, SVGF — needs the feature enabled to be valid. It is mandatory on every
Vulkan 1.3 device, so enabling it costs no portability. Nothing else from 1.3 is
used: the tree contains no `vkCmdBeginRendering`, `vkCmdPipelineBarrier2` or
`vkQueueSubmit2` — the engine is on classic render passes and VK1.0-style barriers
throughout.

{{cite ohao/gpu/vulkan/device_setup.cpp "features13.shaderDemoteToHelperInvocation = VK_TRUE;"}}

The Vulkan 1.2 bits do the descriptor work. Of eight assigned fields, seven are
descriptor-indexing features; the eighth, `bufferDeviceAddress`, is required by
acceleration structure builds. `runtimeDescriptorArray` plus
`shaderSampledImageArrayNonUniformIndexing` let shaders declare an unsized
`uniform sampler2D textures[]` and index it with `nonuniformEXT(...)`, which
`gbuffer.frag`, `pt_closesthit.rchit`, `pt_anyhit.rahit` and `pt_miss.rmiss` all do.
Three more — `descriptorBindingPartiallyBound`,
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

`initialize()` reads as a flat list of `if (!createX()) return false`, but three of
the steps have real dependencies that a reordering would break, two of them silently
rather than loudly.

- The surface must precede device selection. `pickPhysicalDevice` filters graphics
  queue families by `vkGetPhysicalDeviceSurfaceSupportKHR` only when `m_surface` is
  already set, and `createLogicalDevice` adds `VK_KHR_swapchain` under the same test.
  Create the surface later and the renderer may pick a family that cannot present and
  a device without the swapchain extension; the failure arrives at
  `vkCreateSwapchainKHR`, two steps away.

{{cite ohao/gpu/vulkan/device_setup.cpp "vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &canPresent);"}}

- `BindlessTextureManager` must be handed to `DeferredRenderer` before the deferred
  renderer initialises. The G-buffer pass builds its pipeline layout with the bindless
  set only if it has a manager at that moment; without one the layout has zero sets
  and `gbuffer.frag`'s `set = 0, binding = 0` texture array has nothing behind it.

{{cite ohao/gpu/vulkan/renderer.cpp "m_deferredRenderer->setTextureManager(m_textureManager.get());"}}

- Presentation comes last. `createPresentation` needs only the device, but the
  swapchain it builds is rebuilt through `recreatePresentation`, which calls
  `resize()` on the deferred renderer and the RT profiles — so everything `resize()`
  touches must already exist by the first frame.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_presentTarget && !createPresentation()) {"}}

A fourth step is notable for its absence rather than its position. `initialize()`
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
- With a `PresentTarget`, the surface must exist before `pickPhysicalDevice` and
  `createLogicalDevice`, or neither present support nor `VK_KHR_swapchain` is
  guaranteed.
- `DeferredRenderer::setTextureManager` must run before `DeferredRenderer::initialize`,
  or the G-buffer pipeline layout has no bindless set.
- `validationEnabled() == false` means validation did not run, not that it passed.
- Only one queue is created. A second needs a change to the queue create info
  (`queueCount`, or another `VkDeviceQueueCreateInfo`), not just a `vkGetDeviceQueue`
  call.
