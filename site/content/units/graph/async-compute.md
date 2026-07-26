---
module: graph
id: async-compute
title: Async compute queue
standard: v2
---

## There is no second queue to be asynchronous against

Async compute earns its name from hardware: a GPU exposes queue families that
consume command buffers concurrently, so a compute workload can occupy shader
cores a raster pass leaves idle. OHAO's device asks for exactly one queue, from
the first graphics-capable family it finds:

{{cite ohao/gpu/vulkan/device_setup.cpp "queueCount = 1,"}}

`AsyncComputeQueue::initialize` takes a family index and a queue index and fetches
whatever handle they name — it does not search for a compute-only family itself:

{{cite ohao/render/async/async_compute_queue.cpp "vkGetDeviceQueue(m_device, computeQueueFamily, computeQueueIndex"}}

The only code in the tree that constructs one is
`tests/renderer/renderer_pipeline_tests.cpp`, and it passes the graphics family,
with the reason in the comment:

{{cite tests/renderer/renderer_pipeline_tests.cpp "g_context->graphicsQueueFamily,  // Use graphics queue if no dedicated compute"}}

That file is in no build target: `tests/renderer/CMakeLists.txt` declares
`renderer_test`, `env_cdf_test`, `sobol_test` and `denoise_parse_test`, and names
no fifth source. It could not compile if it were wired in, either — five of its
includes name headers that are not in the tree at all.

{{cite tests/renderer/renderer_pipeline_tests.cpp "render/deferred/ssgi_pass.hpp"}}

So this is a submission harness that the source glob compiles into `ohao_renderer`
and that has never executed — no frame path uses it and no test runs it. The
compute work that does reach a frame — SSAO, SSR, subsurface scattering, the
particle update, the à-trous and NRD denoise chains, DLSS-RR where it is compiled
in — is recorded inline into the command buffer the frame is
already recording and separated from the raster and ray-tracing work by pipeline
barriers, not by a queue. What is worth reading here is the sync design, and the
specific things that gate promoting it.

## Why a monotone counter beats a fence per task

Vulkan's classic completion primitive is the binary fence: one `VkFence` per
submit, signalled once, reset by hand before reuse. N tasks in flight means a pool
of N fences, a reset pass, and an ownership rule for the fence of a task nobody
remembers. `AsyncComputeQueue` replaces that with a single timeline semaphore — a
64-bit device-side counter that only increases. Each submit claims the next value
under the task mutex and asks the queue to signal it when the command buffer
retires:

{{cite ohao/render/async/async_compute_queue.cpp "task.signalValue = ++m_timelineSemaphore.currentValue;"}}

Completion is then one non-blocking host query and one integer compare, with no
reset and no per-task object:

{{cite ohao/render/async/async_compute_queue.cpp "return semValue >= it->second.signalValue;"}}

`>=` rather than `==` is what makes the scheme tolerant of gaps. A timeline signal
only has to be strictly greater than the current value, not consecutive, and the
counter is advanced *before* `vkQueueSubmit` runs — so a submit that fails burns
the value it claimed, and the next successful task signals straight past it with
no bookkeeping.

The quieter property is load-bearing: because every task goes to the same
`VkQueue`, signal operations retire in submission order, which makes the one
counter a total order over tasks. Split these tasks across two real queues and a
low-numbered task could still be executing when a high-numbered one signals — at
which point `semValue >= signalValue` reports the earlier task complete while its
writes are still in flight. The single shared semaphore is correct precisely
because the queue is single.

:::why
The standing alternative is a fence pool plus binary semaphores: fences for host
polling, binary semaphores for queue-to-queue ordering, two lifetimes to manage.
One timeline object does host polling, host waiting and cross-queue waiting, and a
wait may be submitted for a value that has not been signalled yet — exactly what
`submitTaskWithWait` needs to express "run after the graphics queue reaches point
X". No comment or commit message records this as a weighed decision, so read it as
a property of the code rather than a documented one. What the code does show is the
cost: every wait it builds is pinned to `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, so
the API cannot express a wait that should block a transfer instead.
:::

## The feature bit the renderer never asks for

Timeline semaphores are core in Vulkan 1.2, but core does not mean enabled —
`timelineSemaphore` is an opt-in feature bit. The renderer's device fills a
`VkPhysicalDeviceVulkan12Features` with buffer-device-address and the descriptor
indexing bits the bindless path needs, and never sets it:

{{cite ohao/gpu/vulkan/device_setup.cpp "VkPhysicalDeviceVulkan12Features features12{};"}}

The one place in the tree that sets the bit is the same unbuilt pipeline-test file,
in a device it constructs for itself alongside `VK_KHR_timeline_semaphore`:

{{cite tests/renderer/renderer_pipeline_tests.cpp "timelineFeatures.timelineSemaphore = VK_TRUE;"}}

Since nothing compiles that file, `createTimelineSemaphore()` has never run at all
— not on an opted-in device, not on any other. Constructing an `AsyncComputeQueue`
on `VulkanRenderer`'s device as it stands would create a timeline semaphore on a
device that never enabled the feature — a validation error that many drivers
service anyway, which is the worst failure mode to inherit. Adopting this class is
a device-creation change first and a scheduling change second.

## The recycler that only failure ever feeds

`allocateCommandBuffer` pops a buffer off a free list and resets it, allocating a
new primary from the pool only when the list is empty. `freeCommandBuffer` pushes
one back, capped:

{{cite ohao/render/async/async_compute_queue.cpp "if (m_freeCommandBuffers.size() < MAX_COMMAND_BUFFERS) {"}}

That cap is never reached, because completing tasks never feed the list.
`freeCommandBuffer` has exactly three call sites, all error paths inside
`submitTaskWithWait`: begin failed, end failed, submit failed. On the success path
the handle is dropped — `AsyncComputeTask` has no `VkCommandBuffer` member to hold
it, despite the comment at the point where the task is filed:

{{cite ohao/render/async/async_compute_queue.cpp "// Store task info (with command buffer for later cleanup)"}}

So every *successful* submit allocates a fresh primary command buffer, released
only when `cleanup()` destroys the pool; `MAX_COMMAND_BUFFERS = 16` bounds the free
list, not the pool. `m_taskMap` has the same shape: entries are inserted and never
erased — `processCompletedTasks` patches a finished task's status in the map and
erases only from `m_activeTasks` — so every task ever submitted keeps both of its
`std::function`s alive, including the record closure, which is copied into the task
*after* the recording has happened and is never invoked again:

{{cite ohao/render/async/async_compute_queue.cpp "task.recordCommands = recordCommands;"}}

Nothing has ever submitted a task, so nothing has ever paid for this. One task per
frame would leak a command buffer and a retained closure per frame until the queue
dies.

## Locking that is right for exactly one submitting thread

`m_taskMutex` is held across the whole of `submitTaskWithWait`, `isTaskComplete`
and `processCompletedTasks`. None of the class's six `const` accessors takes it,
and not by accident: the mutex is not `mutable`, so a `const` method physically
cannot. Four of the six are harmless: two read `std::atomic` counters, two read a
semaphore handle that only `initialize` and `cleanup` write. The other two read
state that submits mutate — `getTaskSignalValue` walks `m_taskMap`:

{{cite ohao/render/async/async_compute_queue.cpp "uint64_t AsyncComputeQueue::getTaskSignalValue(AsyncTaskHandle handle) const {"}}

`waitForTask` calls that before it waits, so a wait racing a submit reads an
`std::map` mid-insert. The other is `getCurrentSemaphoreValue`, which reads a plain
`uint64_t` that submits increment under the lock — while `m_pendingTaskCount` and
`m_completedTaskCount`, which nothing depends on for correctness, are the members
that did get `std::atomic`.

Completion callbacks then fire *while* the lock is held:

{{cite ohao/render/async/async_compute_queue.cpp "it->onComplete();"}}

so an `onComplete` that chains a follow-up task self-deadlocks on a non-recursive
`std::mutex`. And `waitIdle` drains with `vkQueueWaitIdle`, which stalls the whole
queue rather than waiting for this object's tasks — on today's device that queue
is the graphics queue, so destroying an async queue would stall the renderer:

{{cite ohao/render/async/async_compute_queue.cpp "vkQueueWaitIdle(m_computeQueue);"}}

## The barriers assume the queue is not really separate

`AsyncComputeHelper` supplies three barrier helpers. Two of them are image
barriers — `computeToGraphicsBarrier` and its mirror `graphicsToComputeBarrier` —
and both set `srcQueueFamilyIndex` and `dstQueueFamilyIndex` to
`VK_QUEUE_FAMILY_IGNORED`, explicitly *no* queue-family ownership transfer:

{{cite ohao/render/async/async_compute_queue.cpp "void AsyncComputeHelper::computeToGraphicsBarrier(VkCommandBuffer cmd,"}}

The third, `computeBarrier`, cannot express a transfer even in principle: it builds
a `VkMemoryBarrier`, a global barrier whose struct carries no queue-family fields
at all.

{{cite ohao/render/async/async_compute_queue.cpp "VkMemoryBarrier barrier{};"}}

Every image and buffer in the engine is created with exclusive sharing, the
GBuffer attachments included:

{{cite ohao/render/deferred/gbuffer_pass.cpp "imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;"}}

Under exclusive sharing, a resource written on one family and read on another
needs a release barrier submitted on the source queue and a matching acquire on
the destination, both naming real family indices. One `vkCmdPipelineBarrier` with
ignored families cannot express that. The helpers are correct for exactly one
configuration — a single family, where "async" would mean a second submission
stream rather than a second engine — and would need rewriting as release/acquire
pairs the day a real compute family is used.

Nothing in the tree calls them, and no single shipping pattern matches them. Three
passes do end on the COMPUTE→FRAGMENT transition the image helper encodes; SSAO
gets there through `RenderPassBase::transitionImage`, the shared deferred helper
that fills in the same ignored families, so the call site names only stages:

{{cite ohao/render/deferred/ssao_pass.cpp "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,"}}

SSR and SSS build the `VkImageMemoryBarrier` inline instead, SSS widening the
destination to `COMPUTE | FRAGMENT` because one helper closes both of its blur
passes — the horizontal one, whose output the vertical pass reads in COMPUTE, and
the vertical, whose output leaves for post-processing. The rest of the compute work
ends elsewhere: the particle update publishes to
`VERTEX_SHADER | DRAW_INDIRECT` so the indirect draw can read what it wrote, the
à-trous passes chain COMPUTE→COMPUTE, and `nrd_compose.cpp` and `dlss_rr.cpp`
contain no `vkCmdPipelineBarrier` at all.

:::key
Read this as a sync design, not a shipped frame path. Its timeline scheme is sound
*because* there is one queue; three things gate promoting it — the device must
enable `timelineSemaphore`, the success path must return command buffers to the
free list, and the barrier helpers must become release/acquire pairs naming real
family indices.
:::

## Contracts

- `AsyncComputeQueue` has no caller at all — the one file that constructs it is in no build target. Real frame compute in OHAO is recorded inline into the frame command buffer on the single graphics queue.
- The device must enable `timelineSemaphore` before an instance is created against it. `VulkanRenderer`'s device does not.
- All tasks must go to one `VkQueue`: the shared counter plus `semValue >= signalValue` misreports completion as soon as tasks are spread across queues that retire out of order.
- An `onComplete` callback must not call back into the queue — it runs under the non-recursive `m_taskMutex`.
- `getTaskSignalValue` and `getCurrentSemaphoreValue` do not lock, so the class is safe only with a single submitting thread.
- A successful submit never returns its command buffer to the free list, so pool usage grows with total submissions until `cleanup()`.
