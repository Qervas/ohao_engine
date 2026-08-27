---
module: graph
id: frame-resources
title: Frame resources ring
standard: v2
---

## Three slots for a renderer that presents nothing

The header justifies the ring depth as textbook triple buffering: one frame executing on the GPU, one being recorded by the CPU, one on screen.

{{cite ohao/render/frame/frame_resources.hpp "3 frames allows for optimal pipelining: one being rendered by GPU,"}}

The third of those does not exist in this engine. There is no `VkSwapchainKHR` and no `vkQueuePresentKHR` anywhere under `ohao/`, `examples/` or `tests/`; nothing is ever "being displayed" by a presentation engine, and no submission in the frame path carries a semaphore. The consumer at the end of the pipe is a host `memcpy` out of a slot's mapped staging buffer into a CPU pixel vector.

So the slot the comment assigns to the display is really the slot that lets the CPU read a *finished* image without asking the GPU to stop. Each frame body takes the slot it is about to reuse, waits on its fence, copies the staging buffer the GPU filled the last time that slot ran, resets the fence, and only then re-records into it. Depth 3 sets how much GPU work stands between that read and the write that would overwrite it.

## One constant, and the drivers that hardcode it anyway

Everything the ring sizes derives from `MAX_FRAMES_IN_FLIGHT` rather than a literal: the command-buffer count, the fence loop, the descriptor-set count, the `std::array` of slots, and — outside this file — the descriptor pool, which budgets one extra set for the legacy non-ring path.

{{cite ohao/gpu/vulkan/pipeline.cpp "const uint32_t totalSets = framesInFlight + 1; // +1 for legacy compatibility"}}

The depth leaks back out as a bare `3` wherever a driver flushes the readback lag before saving an image — five sites, none of which reads the constant. Four are the offline examples; the fifth is inside `ohao/`, in the inverse-rendering session.

{{cite ohao/inverse/render_session.hpp@223ff7f "const int frames = budget.spp + 3;"}}

{{cite examples/model_viewer.cpp "const int frames = cli.useDeferred ? 30 : (samples + 3);"}}

`cornell_box` and `model_viewer` write `samples + 3`, `turntable` writes `spp + 3`, and `env_demo` splits its three across the `--pan-x` branch — `samples + 2`, then one more `render()` after the camera moves. Raise the constant to 4 and all five capture an image one accumulation pass earlier than they meant to.

## Nine allocations the engine's own allocator never sees

Each slot owns three buffers — camera UBO, light UBO, readback staging — built by a private `createBuffer` calling `vkCreateBuffer`, `vkGetBufferMemoryRequirements`, `vkAllocateMemory` and `vkBindBufferMemory` directly. Three slots therefore mean nine buffers backed by nine separate `VkDeviceMemory` allocations, persistently mapped at creation and never unmapped except on resize. The memory type chosen is the first index that is *both* allowed by the buffer's own `memoryTypeBits` and carries every requested property; the loop tests the two conditions together.

{{cite ohao/render/frame/frame_resources.cpp "if ((typeFilter & (1 << i)) &&"}}

Failing to find one is the subsystem's only `throw`, in a file where every other failure returns `false`. At startup that is survivable — `VulkanRenderer::initialize()` wraps its whole init sequence in a `try`/`catch`. The resize path reaches the same function with no handler anywhere above it; see below.

{{cite ohao/render/frame/frame_resources.cpp "throw std::runtime_error("}}

All nine ask for `HOST_VISIBLE | HOST_COHERENT`. Coherent memory needs no flush or invalidate, which is right for the two uniform buffers the CPU writes each frame. The staging buffer is the opposite traffic — the CPU *reads* `width * height * 4` bytes out of it — yet gets the same mask, never requesting `HOST_CACHED`, the property that would let those reads go through the CPU cache.

It is zeroed at creation rather than left as whatever the driver returned, so the first pass around the ring reads black instead of garbage.

{{cite ohao/render/frame/frame_resources.cpp "// Initialize staging buffer to black"}}

:::why
The engine ships a VMA-backed `GpuAllocator` whose usage enum includes a dedicated readback mode, and the ring uses none of it — not even the suballocation that would collapse nine device allocations into a handful.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "case AllocationUsage::GpuToCpu:"}}

`AllocationUsage::GpuToCpu` has no caller anywhere in the tree, and both entry points that would accept a `GpuAllocator*` are handed nothing: `BindlessTextureManager::initialize` gets an explicit `nullptr`, and `RenderGraph::initialize` is called with its defaulted allocator argument omitted.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

{{cite ohao/render/deferred/deferred_renderer.cpp "m_renderGraph.initialize(m_device, m_physicalDevice);"}}

The hand-rolled path keeps this file self-contained and VMA-free, which makes it cheap to read and cheap to move. The cost is that the readback buffer, the one allocation whose access pattern differs, is allocated exactly like the two that do not.
:::

## The fence starts signalled, and nobody checks it afterwards

Every slot's fence is created already signalled, so the first pass around the ring waits on fences that have never been submitted against and returns immediately instead of deadlocking.

{{cite ohao/render/frame/frame_resources.cpp "fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;"}}

`waitForFrame` reports success as a `bool` and defaults its timeout to `UINT64_MAX`.

{{cite ohao/render/frame/frame_resources.hpp "bool waitForFrame(uint32_t frameIndex, uint64_t timeoutNs = UINT64_MAX);"}}

All three call sites in `render_dispatch.cpp` take that default and discard the result. With an infinite timeout a `false` return cannot mean "timed out": it means `vkWaitForFences` returned something other than `VK_SUCCESS` — device loss, out of memory — or that the manager was never initialised.

{{cite ohao/render/frame/frame_resources.cpp "VkResult result = vkWaitForFences"}}

In none of those cases does the caller change course, but the consequence splits by which failure it was. Each site copies only when `frame.stagingBufferMapped` is non-null. After a `vkWaitForFences` failure it *is* non-null, so the copy runs against a staging buffer the GPU may still be writing. On an uninitialised manager `getFrame` hands back a default-constructed slot whose pointer is `nullptr`, so the copy is skipped — and the frame body walks on to `vkResetCommandBuffer` with a `VK_NULL_HANDLE` command buffer instead.

{{cite ohao/render/frame/frame_resources.hpp "void* stagingBufferMapped{nullptr};"}}

That second path is reachable. A failed `initializeFrameResources()` is explicitly non-fatal, and only the forward `renderMultiFrame` body is gated on `isInitialized()` — the deferred and RT bodies enter the ring unconditionally.

{{cite ohao/gpu/vulkan/renderer.cpp "// Not a fatal error - we can still use single-frame rendering"}}

## Two bounds policies for the same index

`getFrame` accepts any `uint32_t` and wraps it into range.

{{cite ohao/render/frame/frame_resources.cpp "getFrame(uint32_t frameIndex) {"}}

`waitForFrame` and `resetFrame` do the opposite: an index at or past `MAX_FRAMES_IN_FLIGHT` is rejected — the first returns `false` without waiting, the second returns without resetting. The two policies agree only while the caller feeds a pre-wrapped index, which `VulkanRenderer` does by advancing through the static `nextFrame` helper.

Hand this class a monotonically increasing global frame counter instead — the obvious "cleanup" once the modulo in `getFrame` is read as the API's contract — and from the fourth frame on `getFrame` still returns the right slot while the wait and the reset both bail on the range check. The CPU then records into a command buffer the GPU may still be executing and submits against a fence that is already signalled. The ring reports none of it: `waitForFrame`'s `false` goes into a discarded value and `resetFrame` returns `void`. One layer down it is loud — resetting a pending command buffer and submitting with a signalled fence are both validation errors — but the layers are opt-in, so an ordinary run stays quiet.

{{cite ohao/gpu/vulkan/device_setup.cpp "Validation layers ENABLED"}}

## Two names the frame path does not use

`FrameCameraUBO` declares view, proj and `viewPos` with a `static_assert` pinning it as a GPU POD.

{{cite ohao/render/frame/frame_resources.hpp "static_assert(GpuPod<FrameCameraUBO>"}}

Nothing constructs it. The buffers this manager allocates are sized from `CameraUniformBuffer` in `renderer.hpp` — a field-for-field, alignment-for-alignment twin declared in another header.

{{cite ohao/gpu/vulkan/device_setup.cpp "size_t cameraBufferSize = sizeof(CameraUniformBuffer);"}}

The *layout* very much ships; it just ships under the other name. Deleting `FrameCameraUBO` removes a duplicate, but "re-deriving" the buffer size from it would decouple the ring from the struct the descriptor and the shader actually agree on.

`cameraMappedAs<T>()` and `lightMappedAs<T>()` are unused with a different consequence: they wrap the raw `void*` in a `std::span<T>` and have no callers, while the real writes `memcpy` into the bare pointer.

{{cite ohao/render/frame/frame_resources.hpp "return as_mapped_span<T>(cameraBufferMapped, count);"}}

Adopting them would buy no bounds check — `as_mapped_span` is a `static_cast` plus a caller-supplied element count, so a wrong count is exactly as unchecked as a wrong `memcpy` size. What ties the two ends together today is the type and nothing else: the allocation asks for `sizeof(CameraUniformBuffer)` and the write copies `sizeof(ubo)` off a `CameraUniformBuffer`. Change the struct and both move; write the size by hand at either end and nothing notices.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "memcpy(frame.cameraBufferMapped, &ubo, sizeof(ubo));"}}

## What shutdown does not destroy

`shutdown()` idles the device, then destroys three fences and the buffer/memory pairs — deliberately destroying neither the command buffers nor the descriptor sets.

{{cite ohao/render/frame/frame_resources.cpp "// Descriptor sets are freed when descriptor pool is destroyed"}}

Both pools belong to `VulkanRenderer`, so teardown is order-independent with respect to them — but not with respect to the device, which every destroy call and the `vkDeviceWaitIdle` need alive. The destructor calls `shutdown()` again; that second call short-circuits, because `shutdown()` returns immediately unless `m_initialized` is set *and* `m_device` is non-null.

{{cite ohao/render/frame/frame_resources.cpp "void FrameResourceManager::shutdown() {"}}

That two-condition guard is also what carries the move semantics, which nothing in the tree exercises. `m_frames` is a `std::array` of trivially-copyable handles, so a move copies the `VkFence` and `VkBuffer` values into the destination and nulls nothing in the source. What stops the moved-from object from destroying handles the destination now owns is that both move operations clear `m_initialized` *and* null `m_device` — two independent guards, either of which alone would be enough.

{{cite ohao/render/frame/frame_resources.cpp "m_frames = std::move(other.m_frames);"}}

## Resizing without rebuilding

A resolution change affects one thing in the ring: the staging buffers. `resizeStagingBuffers` idles the device, unmaps and destroys all three, then allocates three new ones at the new size and zeroes each.

{{cite ohao/render/frame/frame_resources.cpp "// Create new staging buffers with new size"}}

The caller pairs that with `m_currentFrame = 0`, commented as avoiding a read from an old-size buffer. There is nothing left to avoid: every slot was replaced, so the reset changes which slot runs next and nothing about what any of them holds.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_frameResources.resizeStagingBuffers(width * height * 4);"}}

What this path *does* add is an unguarded route to the `throw`. `resizeStagingBuffers` → `createStagingBuffers` → `createBuffer` → `findMemoryType` is the same chain `initialize()` walks, but `VulkanRenderer::resize` has exactly one caller in the tree — the inverse-rendering render session — and neither it nor anything between it and `main` installs a handler. The failure that returns `false` from `initialize()` calls `std::terminate` here.

{{cite ohao/inverse/render_session.hpp@223ff7f "renderer.resize(budget.width, budget.height);"}}

The uniform buffers and descriptor sets survive untouched, correctly: their sizes are resolution-independent, and the shadow-map image view baked into each set at init points at a fixed-size attachment that the framebuffer teardown leaves alone. That descriptor write is hardwired — bindings 0, 1 and 2, two uniform buffers and one combined sampler — regardless of the `VkDescriptorSetLayout` passed in, which is used only to allocate.

{{cite ohao/render/frame/frame_resources.cpp "descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;"}}

The pool those sets come from stocks exactly those two descriptor types and nothing else, which puts a floor under how wrong the layout can be: one asking for a storage buffer or a storage image cannot allocate at all.

{{cite ohao/gpu/vulkan/pipeline.cpp "poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;"}}

:::key
`initialize()` returns `true` immediately if it has already run. There is no re-initialise path: the ring's shape, buffer sizes and descriptor writes are fixed for the renderer's lifetime, and the only mutable dimension is the staging buffer size. Making the camera or light UBO resolution- or scene-dependent needs a new entry point, not a second `initialize()`.
:::

## Contracts

- The layout passed to `initialize()` must be exactly binding 0 = uniform buffer, 1 = uniform buffer, 2 = combined image sampler. Ask for any other descriptor type and it fails loudly — the pool has only those two, `vkAllocateDescriptorSets` returns `VK_ERROR_OUT_OF_POOL_MEMORY`, and `initialize()` returns `false`. Permute the same three descriptors and it allocates; the hardwired writes then land on the wrong bindings, which the validation layers flag as a type/binding mismatch and an unvalidated run does not.
- `initialize()` must run after the command pool, descriptor pool, layout and shadow image view exist, and before any `render()`. A second call with different sizes returns `true` and does nothing.
- Indices given to `waitForFrame` / `resetFrame` must already lie in `[0, MAX_FRAMES_IN_FLIGHT)`. `getFrame` wraps; those two reject, and the rejection is a skipped synchronisation that this class never reports.
- `resize()` must call `resizeStagingBuffers`, or the next readback copies the new `width * height * 4` bytes out of a buffer still sized for the old dimensions. The `m_currentFrame = 0` beside it is not part of the contract: all three slots are replaced and zeroed, so nothing depends on where the ring restarts.
- Bumping `MAX_FRAMES_IN_FLIGHT` is safe for everything the ring itself builds and wrong for every drain loop that hardcodes 3 — `ohao/inverse/render_session.hpp` plus `cornell_box`, `model_viewer`, `env_demo` and `turntable`. Five hand-copied literals; none of them reads the constant.
