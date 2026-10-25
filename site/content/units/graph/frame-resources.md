---
module: graph
id: frame-resources
title: Frame resources ring
standard: v2
---

## Three slots, and what each one fences

The header justifies the ring depth as textbook triple buffering: one frame executing on the GPU, one being recorded by the CPU, one on screen.

{{cite ohao/render/frame/frame_resources.hpp "3 frames allows for optimal pipelining: one being rendered by GPU,"}}

The slots themselves are small. Each holds a primary command buffer, a fence created signalled, and a host-visible staging buffer for pixel readback — nothing else.

{{cite ohao/render/frame/frame_resources.hpp "VkFence renderFence{VK_NULL_HANDLE};"}}

What a slot really guards is larger than what it owns. `VulkanRenderer` indexes everything that a submitted frame may still be reading by the same `m_currentFrame`: the slot's command buffer and staging buffer, the swapchain acquire semaphore `m_imageAcquired[slot]`, and — handed down as `frameIndex` — the per-slot descriptor sets that lighting, SSS, SSR, TAA, bloom and the tonemap rewrite at record time. Waiting on one fence retires all of them together. That is the ring's whole contract: after `waitForFrame(slot)` returns, nothing the GPU did the last time that slot ran is still in flight, so every slot-indexed object can be rewritten.

{{cite ohao/gpu/vulkan/renderer.hpp "std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAcquired{};"}}

The "one on screen" in the header is not a slot. Displayed images belong to the swapchain, whose image count is chosen independently — the surface minimum plus one — and whose render-finished semaphores are indexed by image, not by slot:

{{cite ohao/gpu/vulkan/swapchain.hpp "// Render-finished semaphores are one PER IMAGE, indexed by the acquired image,"}}

:::why
A per-slot render-finished semaphore would be wrong even though per-slot acquire semaphores are right. The slot fence proves the *submission* that waited on `m_imageAcquired[slot]` has completed, so that semaphore is free to reuse. Nothing proves the *presentation* that waited on a render-finished semaphore has consumed it — no fence tracks `vkQueuePresentKHR` — so a slot-indexed one can still be pending on a different image when the slot comes round. Indexing by image ties its reuse to reacquiring that image, which the presentation engine hands back only once the previous present of it is done.
:::

## The frame body, in the order that keeps the fence honest

`renderDeferred` rebuilds a dirty swapchain *before* it takes a slot: recreation resizes the renderer, which idles the device and resets `m_currentFrame` to 0, and doing that after choosing a slot would wait on one slot's fence and reset another's. Then it waits, acquires, and only then resets the fence:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Every early return below happens BEFORE resetFrame, so the slot's fence stays"}}

The ordering matters because `vkResetFences` is a promise to submit. A frame that resets the fence and then bails — an out-of-date acquire, a minimised window — leaves it unsignalled with nothing queued to signal it, and the next `waitForFrame` on that slot blocks forever. Every exit before `resetFrame` leaves the fence as it found it.

From there the body records into the slot's command buffer and ends. Windowed, it blits the deferred output into the acquired swapchain image, submits waiting on `m_imageAcquired[slot]` at the transfer stage and signalling that image's render-finished semaphore, and presents. Headless, it ends with the image. Either way, only a requested frame writes the slot's staging buffer: the whole output by a copy, or, for a preview request, the `capture.preview` compute pass's downscaled image and nothing else. `readback()` (or `readbackPreview()`) reads it once the slot's fence has proved the write finished, and `readback()` refuses a slot that holds a preview:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!m_readbackReady || m_previewWidth != 0) return {};"}}

## One constant, read everywhere

Everything the ring sizes derives from `MAX_FRAMES_IN_FLIGHT` rather than a literal: the command-buffer count, the fence loop, the `std::array` of slots, the acquire semaphores, and every per-slot descriptor-set array in the deferred passes. No driver depends on the depth any more. Each reads back the frame it asked for, so a loop renders the frames it needs and requests the last.

{{cite examples/model_viewer.cpp "for (int i = 0; i < frames; i++) renderFrame(i == frames - 1);"}}

## Three allocations the engine's own allocator never sees

Each slot's staging buffer is built by a private `createBuffer` calling `vkCreateBuffer`, `vkGetBufferMemoryRequirements`, `vkAllocateMemory` and `vkBindBufferMemory` directly: three buffers backed by three separate `VkDeviceMemory` allocations, each `width * height * 4` bytes, persistently mapped at creation and unmapped only on resize. They exist in both modes (`initializeFrameResources` always asks for them), and only a requested frame writes one. Their usage is `TRANSFER_DST` for the frame copy and `STORAGE_BUFFER` for the preview shader, which writes into the same mapped memory:

{{cite ohao/render/frame/frame_resources.cpp "// Storage too: capture.preview writes a preview straight into it."}}

 The memory type chosen is the first index that is *both* allowed by the buffer's own `memoryTypeBits` and carries every requested property; the loop tests the two conditions together.

{{cite ohao/render/frame/frame_resources.cpp "if ((typeFilter & (1 << i)) &&"}}

Failing to find one is the subsystem's only `throw`, in a file where every other failure returns `false`. At startup that is survivable — `VulkanRenderer::initialize()` wraps its whole init sequence in a `try`/`catch`. The resize path reaches the same function with no handler anywhere above it; see below.

{{cite ohao/render/frame/frame_resources.cpp "throw std::runtime_error("}}

The traffic is CPU *reads* of a whole frame, so all three ask for `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` and fall back to coherent memory only where the device offers no cached type. Uncached host-visible memory read one 1280x720 frame in about 20 ms; cached memory reads at memory speed.

{{cite ohao/render/frame/frame_resources.cpp "// Readback buffers: the CPU reads them, so host-cached memory where the device offers it"}}

Each buffer is also zeroed at creation rather than left as whatever the driver returned.

:::why
The engine ships a VMA-backed `GpuAllocator` whose usage enum includes a dedicated readback mode, and the ring uses none of it.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "case AllocationUsage::GpuToCpu:"}}

`AllocationUsage::GpuToCpu` has no caller anywhere in the tree, and both entry points that would accept a `GpuAllocator*` are handed nothing: `BindlessTextureManager::initialize` gets an explicit `nullptr`, and `RenderGraph::initialize` is called with its defaulted allocator argument omitted.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

{{cite ohao/render/deferred/deferred_renderer.cpp "m_renderGraph.initialize(m_device, m_physicalDevice);"}}

The hand-rolled path keeps this file self-contained and VMA-free, which makes it cheap to read and cheap to move. The cost is that it chooses the readback memory type itself, the cached-then-coherent preference above, rather than letting an allocator choose it.
:::

## The fence starts signalled, and nobody checks it afterwards

Every slot's fence is created already signalled, so the first pass around the ring waits on fences that have never been submitted against and returns immediately instead of deadlocking.

{{cite ohao/render/frame/frame_resources.cpp "fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;"}}

`waitForFrame` reports success as a `bool` and defaults its timeout to `UINT64_MAX`.

{{cite ohao/render/frame/frame_resources.hpp "bool waitForFrame(uint32_t frameIndex, uint64_t timeoutNs = UINT64_MAX);"}}

The two frame bodies in `render_dispatch.cpp`, deferred and RT, take that default and discard the result. With an infinite timeout a `false` return cannot mean "timed out": it means `vkWaitForFences` returned something other than `VK_SUCCESS`, such as device loss or running out of memory. The frame bodies do not change course; they go on to record into the slot. `readback()` is the one caller that checks, and returns an empty span instead of a buffer the GPU may still be writing. An uninitialised manager is not a live case: `initializeFrameResources()` failing fails `VulkanRenderer::initialize()`.

{{cite ohao/render/frame/frame_resources.cpp "VkResult result = vkWaitForFences"}}

## Two bounds policies for the same index

`getFrame` accepts any `uint32_t` and wraps it into range.

{{cite ohao/render/frame/frame_resources.cpp "getFrame(uint32_t frameIndex) {"}}

`waitForFrame` and `resetFrame` do the opposite: an index at or past `MAX_FRAMES_IN_FLIGHT` is rejected — the first returns `false` without waiting, the second returns without resetting. The per-slot descriptor arrays in the deferred passes reject it too, skipping the pass. The policies agree only while the caller feeds a pre-wrapped index, which `VulkanRenderer` does by advancing through the static `nextFrame` helper.

Hand this class a monotonically increasing global frame counter instead — the obvious "cleanup" once the modulo in `getFrame` is read as the API's contract — and from the fourth frame on `getFrame` still returns the right slot while the wait and the reset both bail on the range check. The CPU then records into a command buffer the GPU may still be executing and submits against a fence that is already signalled. The ring reports none of it: `waitForFrame`'s `false` goes into a discarded value and `resetFrame` returns `void`. One layer down it is loud — resetting a pending command buffer and submitting with a signalled fence are both validation errors — but the layers are opt-in through `OHAO_VALIDATION`, so an ordinary run stays quiet.

{{cite ohao/gpu/vulkan/device_setup.cpp "Validation layers ENABLED"}}

## Names the frame path does not use

The `FrameResources` docblock still lists per-slot uniform buffers and a descriptor set; the struct holds neither. Per-frame descriptor sets live in the passes that rewrite them, sized by the same constant.

{{cite ohao/render/frame/frame_resources.hpp "* - Descriptor set (bound to this frame's uniform buffers)"}}

`FrameCameraUBO` declares view, proj and `viewPos` with a `static_assert` pinning it as a GPU POD, and nothing constructs it. The slot's `valid` flag is cleared in `shutdown()` and set nowhere, so its `explicit operator bool` is false for every slot of a working ring — test a slot with it and the test always fails.

{{cite ohao/render/frame/frame_resources.hpp "[[nodiscard]] explicit operator bool() const noexcept { return valid; }"}}

## What shutdown does not destroy

`shutdown()` idles the device, then destroys three fences and the staging buffer/memory pairs — deliberately not the command buffers, which return to `VulkanRenderer`'s pool when that pool is destroyed.

{{cite ohao/render/frame/frame_resources.cpp "// Command buffers are freed when command pool is destroyed"}}

`VulkanRenderer` tears down the presentation side — acquire semaphores and swapchain — first, then the ring. Teardown is order-independent with respect to the pool but not the device, which every destroy call and the `vkDeviceWaitIdle` need alive. The destructor calls `shutdown()` again; that second call short-circuits, because `shutdown()` returns immediately unless `m_initialized` is set *and* `m_device` is non-null.

{{cite ohao/render/frame/frame_resources.cpp "void FrameResourceManager::shutdown() {"}}

That two-condition guard is also what carries the move semantics, which nothing in the tree exercises. `m_frames` is a `std::array` of trivially-copyable handles, so a move copies the `VkFence` and `VkBuffer` values into the destination and nulls nothing in the source. What stops the moved-from object from destroying handles the destination now owns is that both move operations clear `m_initialized` *and* null `m_device` — two independent guards, either of which alone would be enough.

{{cite ohao/render/frame/frame_resources.cpp "m_frames = std::move(other.m_frames);"}}

## Resizing without rebuilding

A resolution change affects one thing in the ring: the staging buffers. `resizeStagingBuffers` idles the device, unmaps and destroys all three, then allocates three new ones at the new size and zeroes each.

{{cite ohao/render/frame/frame_resources.cpp "// Create new staging buffers with new size"}}

The caller pairs that with `m_currentFrame = 0`. After the device idle every fence is signalled — no frame body leaves a fence reset without a submission — and every slot's staging buffer is new, so where the ring restarts changes nothing about what it holds. The reset does matter to the windowed body, which is why the swapchain is rebuilt before a slot is chosen.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_frameResources.resizeStagingBuffers(static_cast<size_t>(width) * height * 4);"}}

What this path *does* add is an unguarded route to the `throw`. `resizeStagingBuffers` → `createStagingBuffers` → `createBuffer` → `findMemoryType` is the same chain `initialize()` walks, but nothing between `VulkanRenderer::resize` and `main` installs a handler — and in the windowed host, `resize` runs from inside `renderDeferred` whenever the swapchain is rebuilt. The failure that returns `false` from `initialize()` calls `std::terminate` here.

:::key
`initialize()` returns `true` immediately if it has already run. There is no re-initialise path: the ring's depth and shape are fixed for the renderer's lifetime, and the only mutable dimension is the staging buffer size.
:::

## Contracts

- `waitForFrame(slot)` must precede every rewrite of a slot-indexed object — command buffer, staging buffer, acquire semaphore, and the per-slot descriptor sets the deferred passes select with `frameIndex`. The deferred renderer must be recorded with the slot whose fence was just waited.
- `resetFrame` must come after the last early return of a frame body. A reset fence with no submission behind it deadlocks the next wait on that slot.
- Indices given to `waitForFrame` / `resetFrame` must already lie in `[0, MAX_FRAMES_IN_FLIGHT)`. `getFrame` wraps; those two reject, and the rejection is a skipped synchronisation that this class never reports.
- `resize()` must call `resizeStagingBuffers`, or the next headless readback copies the new `width * height * 4` bytes out of a buffer still sized for the old dimensions.
- Bumping `MAX_FRAMES_IN_FLIGHT` is safe for everything sized from the constant and wrong for every drain loop that hardcodes 3 — `cornell_box`, `model_viewer`, `env_demo` and `turntable`. Four hand-copied literals; none of them reads the constant.
