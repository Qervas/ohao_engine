---
module: path-tracer
id: host
title: Host lifecycle
standard: v2
---

## Five steps that do not commute

`PathTracer` is the largest single owner of GPU state in the engine: some two dozen
images, a descriptor set per frame in flight, an RT pipeline, a shader
binding table, and five optional denoise/post backends held by pointer. `init()` runs five build
steps whose order is forced by data dependencies, not taste: `createRTPipeline`
needs the descriptor set layout to build a pipeline layout, and
`createShaderBindingTable` needs the finished pipeline to query group handles plus
`vkGetBufferDeviceAddress` to publish the SBT's strided regions — which is itself
resolved in step one.

Step one exists for three of the four pointers it fetches.
`vkCreateRayTracingPipelinesKHR`, `vkGetRayTracingShaderGroupHandlesKHR` and
`vkCmdTraceRaysKHR` are `KHR` extension entry points that no loader exports as core
symbols; they are looked up by name on the device and the whole init aborts if any
comes back null — the cheapest place to discover that a device advertised the
extension but the driver did not deliver it.

{{cite ohao/render/rt/path_tracer.cpp "return vkCreateRayTracingPipelinesKHR && vkGetRayTracingShaderGroupHandlesKHR &&"}}

The fourth pointer the same function loads is not a ray-tracing entry point and does
not share that justification. It is fetched under the unsuffixed name
`vkGetBufferDeviceAddress` — core Vulkan since 1.2 — and the instance is created with
`apiVersion = VK_API_VERSION_1_3`, so on a conforming loader it cannot come back null
and its term in the `&&` chain is dead.

{{cite ohao/render/rt/path_tracer.cpp "(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(m_device,"}}

{{cite ohao/gpu/vulkan/device_setup.cpp ".apiVersion = VK_API_VERSION_1_3,"}}

One number in the pipeline create-info is worth pausing on. It declares a maximum
ray recursion depth of two, with a comment that path tracing "needs bounce
recursion" — but the bounce loop in the raygen is iterative, and no closest-hit,
any-hit or miss entry point of `rt/path_tracer.slang` calls `TraceRay`. Every ray is
launched from the raygen, so the pipeline never recurses past depth one.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "pipelineInfo.maxPipelineRayRecursionDepth = 2;"}}

## Render resolution is not output resolution

The caller passes display dimensions; `PathTracer` keeps two pairs. `m_outW/m_outH`
is what was asked for, `m_width/m_height` is what the raygen dispatch and every AOV
are sized to. They are equal in every mode except a DLSS upscaling preset, where
`computeRenderResolution()` shrinks the render pair.

{{cite ohao/render/rt/path_tracer.cpp "computeRenderResolution();   // sets m_width/m_height (render res)"}}

The shrink is not a plain multiply: DLSS wants even dimensions, so the scaled value
is masked down to even and floored at two — the render resolution is therefore
almost never exactly `scale × output`.

{{cite ohao/render/rt/path_tracer.cpp "v &= ~1u;                 // align down to even"}}

Two images escape the render pair, and both are sized to *output*. The RGBA8 beauty
is the first: under upscaling it is the full-res target the DLSS tonemap writes, so
the raygen's own beauty store into it at render size is discarded.

{{cite ohao/render/rt/path_tracer_images.cpp "imageInfo.extent = {m_outW, m_outH, 1};"}}

The second is the DLSS-RR `COLOR_OUT` image — RGBA16F, allocated at the end of the
same `createImages()` behind `#ifdef OHAO_DLSS_ENABLED`. It is what DLSS upscales the
render-res guide buffers *into*, which is exactly why it cannot be render-sized; the
DLSS tonemap compute then reads it and writes the RGBA8 beauty. Both exceptions
therefore exist for the same reason, and both are live in the one mode where render
and output differ.

{{cite ohao/render/rt/path_tracer_images.cpp "if (vkCreateImage(m_device, &imageInfo, nullptr, &m_dlssColorOutImage) != VK_SUCCESS) return false;"}}

`init()` runs before any denoise mode has been applied, so the scale there is always
1. A later `setRenderSettings()` that switches into DLSS-RR is what triggers the
device-idle, destroy-all-images, recreate-all-images pass.

## Why dead members are compiled into every build

:::why
The NRD members — `unique_ptr<NrdDenoiser>`, `NrdCompositor`, `NrdCinematicPost` —
are declared unconditionally in the header even when NRD is off; only the method
bodies that touch them are `#ifdef`-guarded. The rejected alternative, wrapping the
members in `#ifdef OHAO_NRD_ENABLED` like the DLSS block below them, is an ODR
violation here: that macro is `PRIVATE` to one library, while a second library also
instantiates `PathTracer`'s layout through `rt_profile_renderer.hpp`.

{{cite ohao/render/CMakeLists.txt "target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)"}}

The DLSS members *can* be conditional precisely because that macro is defined on both
targets, keeping the layout identical. The asymmetry in the header is a faithful
mirror of the build graph; flipping either side silently changes `sizeof(PathTracer)`
in half the program.

{{cite ohao/gpu/vulkan/CMakeLists.txt "target_compile_definitions(ohao_gpu_vulkan PRIVATE OHAO_DLSS_ENABLED)"}}
:::

The same incompleteness forces the constructor and destructor out of line: a
`unique_ptr` to a forward-declared denoiser cannot instantiate its deleter in the
header. Both live in `path_tracer.cpp`, the one translation unit that includes every
backend's real header.

## One descriptor set per frame slot, rewritten every frame

There is no descriptor caching: the pool holds one set per frame in flight, and
`render()` rebuilds up to twenty-six writes from scratch into the set of the frame
slot it is given before every trace. The array has fifteen entries of slack above
the worst case, and nothing checks `writeCount` against its size.

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[41] = {};"}}

The scene bindings are not owned here. `setSceneBuffers` copies the handles the
`RenderScene` currently exposes — TLAS, arenas, tables, instance buffers and the
light buffer — and it is called every frame after `recordUpdates`, because table
growth and TLAS storage replacement hand out new handles. `render()` refuses to
trace until every one of them is non-null, so there is no dummy buffer to fall back
to.

{{cite ohao/render/rt/path_tracer_render.cpp "!m_instances || !m_materialRefs || !m_lightBuffer) return;"}}

Rewriting a set is only defined if no pending submission still references it, and
the per-slot layout is what establishes that. `renderRTPipeline()` waits on the
fence of the frame slot it is about to reuse — frame *N−3* — and
`MAX_FRAMES_IN_FLIGHT` is 3, so frames *N−1* and *N−2* can still be executing, but
they read their own sets. The write lands on a set whose last user has completed.

{{cite ohao/render/frame/frame_resources.hpp "waitForFrame(currentFrame) - blocks until frame N-3 completes"}}

Of the 26 bindings only binding 12 carries `UPDATE_AFTER_BIND`; the others rely on
that per-slot ordering. The one place the ordering does not reach is a buffer the
set *points at*: a handle that `RenderScene` replaces mid-flight stays alive in its
retire queue for `MAX_FRAMES_IN_FLIGHT` frames, so a set still bound by an earlier
frame never dangles.

## 256 bytes of push constants

The push-constant block is asserted at 256 bytes — the device maximum on most RT
GPUs — and it carries everything per frame that is not a descriptor: three matrices
(inverse view, inverse projection, last frame's view-projection) and four words of
control.

{{cite shaders/rt/path_tracer.slang "uint4 control;   // x flags, y frames already in the running mean, z motion vectors valid, w the environment CDF's width"}}

`params` is the resolution, the sample index that seeds the raygen's PCG stream, and
the bounce limit. `control.y` and `control.z` are the two answers the host's clocks
give (below): how many frames the running mean already holds, and whether last frame's
view-projection is a rendered frame's, so motion vectors can be computed.

{{cite ohao/render/rt/path_tracer_render.cpp "pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces);"}}

## The jitter is computed on the host

No mode samples at pixel centre. The raygen draws a uniform point of the pixel from
its PCG stream per sample and adds it to the primary-ray UV in all denoise modes.

{{cite shaders/rt/path_tracer.slang "const float2 uv = (float2(pixel) + jit + pc.jitter.xy) / float2(size);"}}

What the host contributes, and only when the mode is NRD or DLSS-RR, is the second
term in that sum: `pc.jitter.xy`, a *deterministic* offset. Determinism is the whole
point — the same numbers are handed to the denoiser so it can subtract them during
temporal reprojection. NRD receives them as `cameraJitter`/`cameraJitterPrev`; DLSS-RR
receives them negated as `InJitterOffsetX/Y`. A shift the denoiser could not name
would misalign history rather than widen the footprint. The offset is the radical
inverse of the frame index in bases 2 and 3 — the Halton sequence — recentred on
zero:

$$\Phi_b(i) = \sum_{k \ge 0} d_k\, b^{-(k+1)}, \qquad i = \sum_{k \ge 0} d_k\, b^{k}, \qquad \mathbf{j}_n = \big(\Phi_2(i_n) - \tfrac12,\; \Phi_3(i_n) - \tfrac12\big)$$

$b$ is the base, $d_k$ the digits of index $i$ in that base, and $\mathbf{j}_n$ the
pixel offset for frame $n$. The loop in `render()` is that sum written out, with `f`
accumulating $b^{-(k+1)}$. The index is taken modulo 16 and then incremented, because
$\Phi_b(0) = 0$ would spend one of the sixteen slots on "no jitter at all". Every
other denoise mode gets `pc.jitter.xy = 0` and keeps only the raygen's own random
offset.

{{cite ohao/render/rt/path_tracer_render.cpp "const uint32_t idx = (m_haltonIndex % 16u) + 1u;"}}

{{cite ohao/render/rt/path_tracer_render.cpp "camera.jitter     = {m_jitterCurrent.x, m_jitterCurrent.y};"}}

{{cite ohao/render/rt/denoise/dlss_rr.cpp "evalParams.InJitterOffsetX = -in.jitterX;"}}

## Three counters, and the clear that never happens

`render()` advances three counters once per frame, and they answer different questions:

{{cite ohao/render/rt/path_tracer.hpp "uint32_t m_historyFrameCount = 0;   // frames since resetAccumulation: the denoisers' history"}}

{{cite ohao/render/rt/path_tracer.hpp "uint32_t m_accumFrames = 0;         // frames in the trace's running mean (restarts on a moved view)"}}

`m_sampleIndex` seeds the raygen's random numbers. `m_historyFrameCount` is the
denoisers' clock: NRD's frame index, the à-trous and DLSS-RR reset tests; only
`resetAccumulation()` sends it to zero. `m_accumFrames` is the trace's own running
mean: a moved view (`notifyViewChanged`) restarts it at the top of the next frame
while the denoisers' history carries on and reprojects.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_viewChangedThisFrame) m_accumFrames = 0;"}}

The raygen sees the mean's length as `control.y`, and sees 0 when a temporal denoiser
takes this frame's sample alone (`tracerAccumulates`, `rt_meta.hpp`):

{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.y = tracerAccumulates(m_renderSettings) ? m_accumFrames : 0u;"}}

A reset restores the sample index to a caller-settable seed rather than zero, so a
caller can replay an identical sample sequence across runs that change something
else.

{{cite ohao/render/rt/path_tracer.cpp "m_sampleIndex = m_renderSeed;"}}

Setting the seed while the index still sits at the old one — nothing traced since
the last reset — moves the index too, so a seed set after construction applies to
the very next frame rather than only after the next reset:

{{cite ohao/render/rt/path_tracer.hpp "if (m_sampleIndex == m_renderSeed) m_sampleIndex = seed;"}}

The accumulation is never cleared. It is a ping-pong pair: each frame writes one
image whole, so its barrier declares the old layout `UNDEFINED` every frame,
licensing the driver to discard the contents, and reads the other — the previous
frame's — through a `GENERAL → GENERAL` acquire of that write. Before the pair is
written once, the history image is `UNDEFINED` too, and the raygen reads history
only when `control.y` is above zero.

{{cite ohao/render/rt/path_tracer_render.cpp "accumBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

The post-processing images use the same idiom with per-instance latch booleans instead
of a shared counter — and `createImages()` re-arms them, so a resize re-fires the
`UNDEFINED → GENERAL` transition on the freshly created `VkImage` instead of trusting
a stale "already transitioned".

{{cite ohao/render/rt/path_tracer_images.cpp "m_nrdComposeFirstFrame = true;"}}

## An image nothing reads

`createImages()` still allocates an R32F specular hit-distance image beside the AOVs,
and `destroy()` frees it, but it is in no descriptor binding and the DLSS-RR
evaluation inputs `render()` fills leave the optional specular hit-distance unset, so
nothing writes or reads it.

{{cite ohao/render/rt/path_tracer_images.cpp "if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specHitDistImage) != VK_SUCCESS) return false;"}}

:::key
`PathTracer` holds only *borrowed* handles for scene data. The environment CDF
buffers and the env map view and sampler belong to `VulkanRenderer`; the TLAS,
arenas, tables and light buffer belong to `RenderScene`; `destroy()` nulls the cached CDF handles rather than freeing them,
because freeing them here would double-free.

{{cite ohao/render/rt/path_tracer.cpp "m_envMarginalCDFBuffer = VK_NULL_HANDLE;"}}
:::

## Contracts

- `init()` must not be reordered: function pointers first, descriptor layout before pipeline layout, pipeline before SBT.
- The two profile renderers each own a full `PathTracer`, and each `PathTracer` allocates its own set of some two dozen images, so an eager pair doubles the render-target footprint. They are created lazily on the first `setRenderMode()` into that mode instead.
- Each frame slot's descriptor set is rewritten from the host at the top of `render()`, and bindings 1 and 3 swap between the accumulation pair each frame, so the rewrite is not idempotent. It is safe because the slot's fence was waited first; calling `render()` with a slot whose fence was not waited rewrites a set an earlier submission still reads.
- `setSceneBuffers` must run every frame after `RenderScene::recordUpdates`. A handle cached from an earlier frame may belong to a buffer or TLAS that has since been replaced and retired.
- `destroy()` waits for device idle, tears down the denoisers, nulls the borrowed CDF handles, and only then destroys the SBT, pipeline, descriptors and images. The destructor calls it, so an object outliving its `VkDevice` is a use-after-free.
- Switching to or from a DLSS upscaling preset reallocates every render target, so `setRenderSettings()` is safe only between frames — never inside a command buffer.
- Both profiles use the `PathTracerShaderSet` default (`rt_path_tracer.slang.<entry>.spv`, entries `raygen`, `closestHit`, `anyHit`, `miss`). The pipeline takes each stage's entry name from the set, so a set whose names do not match its SPIR-V fails at pipeline creation.
- `resetAccumulation()` zeroes both clocks; `notifyViewChanged()` restarts only the running mean. A host that resets on every camera move throws away the temporal denoisers' history.
