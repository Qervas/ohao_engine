---
module: gpu
id: renderer-facade
title: VulkanRenderer facade
standard: v2
figures: [gpu-renderer-facade-ring-lag]
---

## A renderer that never owns a window

`VulkanRenderer` creates its own instance, picks its own physical device, and creates a logical device, command pool and offscreen framebuffer — and never creates a swapchain. Its delivery surface is a `std::vector<uint8_t>` of RGBA8 pixels. That is not a limitation the interactive viewer escapes — the GLFW viewer renders headless too, then blits the CPU buffer into an OpenGL texture every frame.

{{cite examples/interactive.cpp "const uint8_t* pixels = renderer.getPixels();"}}

So the PNG writers, the golden smoke test, the inverse-rendering fitter and the realtime viewer all drive the identical `render()` → `getPixels()` pair. `gpu_module.hpp` is not a second entry point: it is a seven-include umbrella whose own comment tells you to prefer the specific headers.

{{cite ohao/gpu/gpu_module.hpp "Umbrella for the GPU / Vulkan subsystem"}}

## Four modes, three of which can be refused

`RenderMode` splits into two raster modes (Forward, Deferred) and two ray-traced ones (RTRealtime, RTOffline). The RT branch of `render()` is guarded by three separate conditions — a pipeline descriptor for the mode, a live renderer profile, and an acceleration-structure manager — and falls through to raster if any is missing.

{{cite ohao/gpu/vulkan/renderer.cpp "if (const auto* rtPipeline = getRTPipeline(m_renderMode);"}}

`setRenderMode` returns `void` and is allowed to refuse: if deferred initialisation failed — which `initialize()` treats as non-fatal — or if the device has no RT extensions, it logs and leaves the previous mode in place. Only Forward is unconditionally available.

{{cite ohao/gpu/vulkan/renderer.cpp "Deferred rendering not available, staying in Forward mode"}}
{{cite ohao/gpu/vulkan/renderer.cpp "Path tracing not available, staying in current mode"}}

The mode you asked for is therefore not necessarily the mode you got, and the field starts at Forward, not Deferred.

{{cite ohao/gpu/vulkan/renderer.hpp "RenderMode m_renderMode{RenderMode::Forward};"}}

Anything that reports "rendering in mode X" from its own local variable rather than from `getRenderMode()` can print a lie.

## Two RT abstractions that are not duplicates

The facade holds *both* `IRTRenderPipeline` objects and `IRTRendererProfile` objects for each RT mode, which reads like redundancy until you look at their storage class. The pipelines are plain value members with no GPU state — pure constant policy, always answerable — so `getRTPipeline(mode)` can report "this is an RT mode, and here are its default settings" before a single Vulkan object exists. The profiles are `unique_ptr`s, each owning a whole `PathTracer`, and they are the thing that might be null.

The two profiles are not the same tracer with different knobs; they load different raygen SPIR-V. `RTProfileRendererBase::init` installs the profile's shader set before `PathTracer::init` runs, so the shader set the `PathTracerShaderSet` struct declares as its own default is never the one that reaches the pipeline through this facade.

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"}}

Reading `rt_pt_raygen.rgen` and concluding "that is what RTOffline runs" is the mistake this indirection invites; `RTOfflineRenderer` names `rt_pt_raygen_offline.rgen.spv` explicitly.

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_offline.rgen.spv"}}

## Why the path tracer is not built during initialize()

:::why
`initialize()` builds the device, the deferred renderer and the acceleration-structure manager, but no `PathTracer`. Each profile owns one, and a `PathTracer` declares nineteen scalar `VkImage` members: beauty and accumulation; the albedo and normal guide pair; nine denoiser guide AOVs (motion, depth, roughness, diffuse and specular radiance, diffuse albedo, specular colour, packed normal-roughness, specular hit distance); NRD's denoised diffuse and specular and its composed HDR; the cinematic chain's pre-DoF and final tonemapped LDR; and the DLSS-RR HDR colour output — plus four image *arrays*: three bloom mips, ping-pong surface and shading history, and a 3×2 GI reservoir set. Only the DLSS output is `#ifdef`-guarded; the other eighteen are unconditional.

{{cite ohao/render/rt/path_tracer.hpp "VkImage        m_dlssColorOutImage"}}

Constructing both profiles eagerly doubles all of it. The rejected alternative is stated in the code: eager dual init can exhaust device memory at 4K.

{{cite ohao/gpu/vulkan/renderer.cpp "// Lazy-create realtime/offline PathTracers on setRenderMode(). Eager dual"}}

The cost of laziness is a re-upload. Scenes are normally uploaded by `setScene()` long before any profile exists, and the upload paths fan out through `forEachRTRenderer`, which was a no-op at the time. So `ensureRTRenderer` marks the acceleration structure dirty and re-runs the whole scene upload when it creates a profile.

{{cite ohao/gpu/vulkan/renderer.cpp "if (created && m_scene && m_initialized) {"}}
:::

`ensureRTRenderer` is public but has no caller outside the class: `setRenderMode` is its only invocation in the tree. The header documents what it does and why it is lazy, and says nothing about who else is meant to call it.

{{cite ohao/gpu/vulkan/renderer.hpp "Lazy-create the PathTracer for"}}

## The settings that are thrown away every frame

Every RT frame begins by discarding the current settings and reloading the active pipeline's compile-time defaults.

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = pipeline.getDefaultSettings();"}}

That keeps the pipeline authoritative over tracer behaviour, but it means every user-settable field has to be rescued by hand across the reset. Three are stashed in locals and written back immediately: anisotropy strength, anisotropy rotation, subsurface strength.

{{cite ohao/gpu/vulkan/renderer.cpp "float preservedAnisoStrength = m_rtSettings.anisotropyStrength;"}}

A fourth, the realtime per-frame sample count driven by the viewer's `+`/`-` keys, is re-injected one level down in `applyRTRenderSettings` instead.

{{cite ohao/gpu/vulkan/renderer.cpp "// Re-inject the interactive per-frame sample count: prepareRTSceneForFrame"}}

:::key
`RTRenderSettings` is not a settings object in the usual sense — it is a per-frame scratch struct with a hand-maintained rescue list. Adding a field and a setter is not enough to make it stick; the field must also be named in `prepareRTSceneForFrame` or `applyRTRenderSettings`, or it silently reverts to the profile default on the very next frame.
:::

## Who wins the argument about the denoiser

`setDenoiseMode` latches a `m_denoiseModeOverridden` bit that does two things. It stops the per-frame settings sync from overwriting the user's choice with the profile default, and it gates a narrow injection back into the tracer's own settings — scoped to Atrous and DLSS-RR, and not to NRD or OIDN.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseModeOverridden &&"}}

The comment above that condition explains the scoping as GPU-side dispatch versus everything else. That is not the real distinction — NRD is dispatched GPU-side from inside the same `PathTracer` frame. Its block is gated on `enableAuxiliaryAOVs`, a profile flag, not on `denoiseMode`, so REBLUR runs on every RT frame of a profile that produces AOVs and nothing needs injecting for it to fire.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs)"}}

The Atrous and DLSS-RR blocks branch on `m_renderSettings.denoiseMode` directly, and the per-frame reload sets that field to `None` (realtime) or `OIDN` (offline) — never Atrous, never DLSS-RR. Without the injection those two branches are unreachable from the command line. OIDN needs nothing either: it never enters the tracer at all.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser)"}}

Widening that condition to "always inject" would look like a cleanup and would clobber each profile's default. Those defaults matter: with no `--denoise` flag at all, RTOffline denoises with OIDN because its settings constant says so, while RTRealtime starts at None.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

## The pixel buffer is three frames behind

The pipelined paths use a ring of three frame slots.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

Each `render()` waits on its slot's fence, copies that slot's staging buffer into `m_pixelBuffer`, and only *then* records and submits this frame's work into the same slot. The copy therefore delivers the image submitted three calls ago, not the one just issued.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Read back previous frame"}}

{{figure gpu-renderer-facade-ring-lag "Conceptual timing of the read-before-record ordering in render(); slot assignment and the depth of three are read from the code, not measured. The first three calls leave m_pixelBuffer unwritten."}}

Offline loops in this repo render `spp + 3` frames rather than `spp` — cornell_box, model_viewer, turntable and the inverse-rendering session driver; env_demo drops to `samples + 2` only when `--pan-x` makes it add one final frame after moving the camera.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : (samples + 3);"}}
{{cite ohao/inverse/render_session.hpp "const int frames = budget.spp + 3;"}}

Under `--denoise=none` that arithmetic lands exactly: after `spp + 3` calls the last copy delivered call `spp - 1`, the frame that had accumulated `spp` samples. No comment at any of those sites ties the `+ 3` to the ring depth, though, and it cannot be the whole story — the default offline configuration is `DenoiseMode::OIDN`, whose `getPixels()` branch never reads `m_pixelBuffer`. There the three extra frames buy three extra samples and compensate no lag.

The one path without the lag is the legacy forward fallback used when frame resources fail to initialise: it blocks on a fence and maps the staging memory in the same call.

## getPixels() is four branches wearing one signature

The accessor branches on denoise mode into genuinely different work, and only two of the four branches touch the GPU at all.

None, Atrous and DLSS-RR return the lagged buffer directly, for one shared reason: whatever denoising ran, ran GPU-side into `m_outputImage`, the RGBA8 beauty the ring already stages. Atrous filters that image in place. DLSS-RR runs `NGX_VULKAN_EVALUATE_DLSSD_EXT` into an RGBA16F colour target and then tonemaps that target into the same beauty image, so the standard readback picks it up unchanged.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseMode == DenoiseMode::None || m_denoiseMode == DenoiseMode::Atrous ||"}}
{{cite ohao/render/rt/path_tracer_render.cpp "m_dlssRR->tonemap(cmd, m_dlssColorOutView, m_outputView"}}

The comment on that branch still says DLSS-RR does not dispatch denoising; it dates from the Phase 1 foundation commit and was not updated when the evaluate-and-tonemap path landed. The branch is right, its stated reason is not. What *is* true is that the tonemap runs only when NGX feature creation succeeded — where that fails, the same branch returns raw beauty.

The second branch is a cache hit: `render()` clears `m_denoiseCacheValid`, so repeat calls between two renders return the stored buffer without submitting anything.

Only the last two do work. NRD reads back its own tonemapped RGBA8 image. OIDN takes a different route again: it waits for the device to go idle and copies the *live* RGBA32F accumulation image plus the albedo and normal AOVs, so it sees the current frame rather than the three-frames-old one, filters them on the host and caches the result until the next `render()`. The filter is created on `oidn::DeviceType::Default` — OIDN picks the device, and nothing here pins it to the CPU — but the round trip through host memory and `vkDeviceWaitIdle` is unconditional.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readbackImage(rtRenderer->getAccumImage(), beauty);"}}
{{cite ohao/render/rt/denoise/oidn_denoise.cpp "oidn::newDevice(oidn::DeviceType::Default)"}}

Reading the live image means the OIDN path needs no warm-up, and the in-tree smoke test depends on that: it sets RTOffline, whose profile default latches `DenoiseMode::OIDN`, calls `render()` exactly once, and writes a PNG straight from `getPixels()`.

{{cite tests/renderer/renderer_test.cpp "renderer.setRenderMode(RenderMode::RTOffline);"}}

Two consequences follow. First, the method is declared `const` but casts away constness to allocate a command buffer, submit it and wait — it is safe only because every consumer is a single-threaded loop that calls it after `render()` returns. Second, the RGBA8 and HDR paths do not observe the same frame, so a golden test that compares `--denoise=none` against `--denoise=oidn` output is comparing different points on the accumulation curve unless the loop has already converged.

Below that sits the bulk of the translation unit — more than half of `renderer.cpp`: eleven near-identical readback helpers, seven pulling debug AOVs and four pulling NRD outputs, each open-coding a staging buffer, a one-shot command buffer, a pair of layout transitions and a `vkQueueWaitIdle`. Ten are instrumentation, reachable only from env_demo's dump flags and interactive's motion-vector key. The eleventh is not: `readbackNrdTonemapped` is how `--denoise=nrd` gets its picture at all, and because `render()` invalidates the cache every frame, that full queue stall runs once per displayed frame.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!self->readbackNrdTonemapped(rgba, rw, rh))"}}

## Contracts

- On the `m_pixelBuffer` paths — None, Atrous, DLSS-RR — `render()` must be called at least `MAX_FRAMES_IN_FLIGHT` times before `getPixels()` returns real content; a single render then read yields an unwritten buffer. The NRD and OIDN branches never touch `m_pixelBuffer` and are correct after one call.
- `setRenderMode` may silently keep the previous mode. Read back `getRenderMode()` rather than trusting the argument.
- `resize()` destroys and recreates every RT image, so descriptors bound before the resize are stale — the inverse-rendering session forces a full scene rebind after resizing for exactly this reason.
- Any new `RTRenderSettings` field a caller can set must be added to the preserve list in `prepareRTSceneForFrame` or re-injected in `applyRTRenderSettings`, or the per-frame reload will discard it.
- `getPixels()` is not reentrant with GPU work in flight: on the NRD and OIDN branches it allocates a command buffer, submits and waits on the graphics queue. It skips that whenever `m_denoiseCacheValid` still holds, so only the first call after each `render()` costs the stall.
- `getDevice()` / `getPhysicalDevice()` currently have no callers in the tree; the header describes them as wiring for sibling differentiable-rendering pipelines.

{{cite ohao/gpu/vulkan/renderer.hpp "VkDevice getDevice() const noexcept { return m_device; }"}}
