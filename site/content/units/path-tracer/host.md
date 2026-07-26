---
module: path-tracer
id: host
title: Host lifecycle
standard: v2
---

## Six steps that do not commute

`PathTracer` is the largest single owner of GPU state in the engine: roughly thirty
images, a host-visible material SSBO, one descriptor set, an RT pipeline, a shader
binding table, and five optional denoise/post backends held by pointer. `init()` runs six build
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
recursion" — but the bounce loop in every raygen variant is iterative, and no
closest-hit, any-hit or miss shader in `shaders/rt/` calls `traceRayEXT`. Every ray
is launched from the raygen, so the pipeline never recurses past depth one.

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

## One descriptor set, rewritten every frame

There is no descriptor caching and no per-frame-in-flight duplication: the pool is
created with `maxSets = 1` and `render()` rebuilds up to 29 writes from scratch
before every trace. The array is sized exactly to the worst case, so adding a
binding without growing it overruns a stack buffer.

{{cite ohao/render/rt/path_tracer_render.cpp "VkWriteDescriptorSet writes[29] = {};"}}

Several of those slots are geometry SSBOs the scene may not have uploaded yet.
Rather than skip the write and leave a descriptor undefined, every null buffer handle
falls back to the material buffer — a 256-entry host-visible allocation that always
exists after `init()` and therefore doubles as the universal dummy.

{{cite ohao/render/rt/path_tracer_render.cpp "normalBufInfo.buffer = m_normalBuffer != VK_NULL_HANDLE ? m_normalBuffer : m_materialBuffer;"}}

Rewriting one shared set is only defined if no pending submission still references
it, and nothing here establishes that. `renderRTPipeline()` waits on the fence of the
frame slot it is about to reuse — frame *N−3* — and `MAX_FRAMES_IN_FLIGHT` is 3, so
frames *N−1* and *N−2* can still be executing when `vkUpdateDescriptorSets` lands on
the same `VkDescriptorSet`. Of the 36 bindings only binding 12 carries
`UPDATE_AFTER_BIND`; the other 35 do not. Treat this as an open hazard rather than a
contract: the fixes are a per-frame-in-flight set, or a wait that covers every
in-flight submission.

{{cite ohao/render/frame/frame_resources.hpp "waitForFrame(currentFrame) - blocks until frame N-3 completes"}}

## 256 bytes, and the word that had to hold two numbers

The push-constant block is asserted at 256 bytes — the device maximum on most RT
GPUs — so when a genuine per-frame sample count was needed there was no room to
append a field. It went into the high half of the word that already held the bounce
limit.

{{cite ohao/render/rt/path_tracer_render.cpp "const uint32_t packedBouncesAndSpf = (spf << 16) | (m_maxBounces & 0xFFFFu);"}}

Only the realtime raygen unpacks the high half and loops on it; the base and offline
raygens mask it off and trace one path per dispatch. The same push constant means two
different things depending on which SPIR-V the profile loaded.

{{cite shaders/rt/pt_raygen_realtime.rgen "uint samplesPerFrame = max(pc.params.w >> 16, 1u);"}}

## The jitter is computed on the host

No mode samples at pixel centre. Every raygen already draws its own stochastic
sub-pixel offset per sample — `getSample2D(dimIdx) - 0.5` — and adds it to the
primary-ray UV in all denoise modes.

{{cite shaders/rt/pt_raygen_realtime.rgen "vec2 uv = (vec2(pixel) + 0.5 + jitter + pc.jitter.xy) / vec2(pc.params.xy);"}}

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
other denoise mode gets `pc.jitter.xy = 0` and keeps only the sampler's own random
offset.

{{cite ohao/render/rt/path_tracer_render.cpp "const uint32_t idx = (m_haltonIndex % 16u) + 1u;"}}

{{cite ohao/render/rt/path_tracer_render.cpp "camera.jitter     = {m_jitterCurrent.x, m_jitterCurrent.y};"}}

{{cite ohao/render/rt/denoise/dlss_rr.cpp "evalParams.InJitterOffsetX = -in.jitterX;"}}

## Two counters, and the clear that never happens

`m_sampleIndex` and `m_historyFrameCount` both advance once per frame and are not
interchangeable: the first seeds the sampler, the second drives temporal logic. A
reset sends the history counter to zero but restores the sample index to a
caller-settable seed rather than zero, so finite-difference and inverse-rendering
runs replay an identical sample sequence across parameter perturbations.

{{cite ohao/render/rt/path_tracer.cpp "m_sampleIndex = m_renderSeed;"}}

The accumulation buffer is never cleared. On the frame where the history counter is
zero its barrier declares the old layout `UNDEFINED`, licensing the driver to discard
the contents; on every later frame the same barrier is a `GENERAL → GENERAL`
read-modify-write acquire.

{{cite ohao/render/rt/path_tracer_render.cpp "accumBarrier.oldLayout = (m_historyFrameCount == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;"}}

The post-processing images use the same idiom with per-instance latch booleans instead
of a shared counter — and `createImages()` re-arms them, so a resize re-fires the
`UNDEFINED → GENERAL` transition on the freshly created `VkImage` instead of trusting
a stale "already transitioned".

{{cite ohao/render/rt/path_tracer_images.cpp "m_nrdComposeFirstFrame = true;"}}

## Seven images that are allocated and never bound

The descriptor set layout declares 36 bindings. Bindings 29–34 are the ReSTIR GI
reservoir ping-pong and 35 is the DLSS specular hit-distance guide; all seven images
are created and destroyed with the rest, and the realtime raygen declares all seven.
It cannot store to all of them: 29–31 are the *prev* half of the ping-pong, declared
`readonly` and only ever `imageLoad`ed. Stores go to 32–34, the *curr* half declared
`writeonly`, and to 35.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "for (uint32_t b = 29; b <= 34; ++b) {"}}

{{cite shaders/rt/pt_raygen_realtime.rgen "layout(set = 0, binding = 29, rgba32f) uniform readonly  image2D prevGIReservoir0;"}}

No `vkUpdateDescriptorSets` call anywhere in `ohao/` targets those bindings — the
per-frame rewrite stops at 28, and only binding 12 carries `PARTIALLY_BOUND`. Those
descriptors are never written while a shader reads them. Treat the reservoir plumbing
as allocated-but-unwired until a writer appears.

:::key
`PathTracer` holds only *borrowed* handles for scene data. The environment CDF
buffers, the env map view and sampler, and every geometry SSBO belong to
`VulkanRenderer`; `destroy()` nulls the cached CDF handles rather than freeing them,
because freeing them here would double-free.

{{cite ohao/render/rt/path_tracer.cpp "m_envMarginalCDFBuffer = VK_NULL_HANDLE;"}}
:::

## Contracts

- `init()` must not be reordered: function pointers first, descriptor layout before pipeline layout, pipeline before SBT.
- The two profile renderers each own a full `PathTracer`, and each `PathTracer` allocates its own set of roughly thirty images, so an eager pair doubles the render-target footprint. They are created lazily on the first `setRenderMode()` into that mode instead.
- The single descriptor set is rewritten from the host at the top of `render()`, and bindings 13–16 flip between ping-pong views each frame, so the rewrite is not idempotent — and it is not synchronised against the two more recent submissions that may still hold the set.
- `setMaterialAlbedos` / `setMaterialData` copy at most 256 entries and silently drop the rest — the material SSBO is a fixed 256-instance allocation.
- `destroy()` waits for device idle, tears down the denoisers, nulls the borrowed CDF handles, and only then destroys the SBT, pipeline, descriptors, material buffer and images. The destructor calls it, so an object outliving its `VkDevice` is a use-after-free.
- Switching to or from a DLSS upscaling preset reallocates every render target, so `setRenderSettings()` is safe only between frames — never inside a command buffer.
- The `PathTracerShaderSet` defaults name `rt_pt_raygen.rgen.spv`, but both shipped profiles pass explicit sets (`..._realtime`, `..._offline`), so the default never loads in-tree.
