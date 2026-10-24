---
module: gpu
id: renderer-facade
title: VulkanRenderer facade
standard: v2
figures: []
---

## A renderer that does not own its window

`VulkanRenderer` creates its own instance, picks its own physical device, and creates a logical device and command pool. What a frame ends with depends on which `initialize` overload ran: the headless one ends it with the final image, the windowed one, which takes a `PresentTarget`, by presenting into a swapchain. In both modes the CPU reads a frame one way only. `requestReadback()` before the `render()` whose pixels are wanted, `readback()` after it. The shared capture procedure (`runtime::captureFrame`, behind `ohao run --render` and `render.capture`), the examples, the PNG-writing tests and `ohao doctor`'s frame check all do exactly that.

{{cite ohao/runtime/frame_capture.cpp "if (i == frames - 1) renderer.requestReadback();"}}

{{cite ohao/gpu/vulkan/renderer.hpp "[[nodiscard]] bool initialize(const PresentTarget& target);"}}

Neither overload makes the renderer a windowing client. The window belongs to `ohao/runtime`: `LiveHost` creates it, builds the renderer at the window's framebuffer size, passes the window's `PresentTarget` in, and forwards drawable-size changes through `setPresentExtent`. The renderer sees a surface-creation callback and an extent, never a window handle.

{{cite ohao/runtime/live_host.cpp "m_renderer->setPresentExtent(m_window.framebufferExtent());"}}

The two modes are distinguishable from outside: `isPresenting()` reports whether a swapchain exists. No frame in either mode copies anything to the CPU unless it was requested, so the one counter, `requestedReadbackCount()`, counts copies that were all asked for. The window host checks that against its own count of requests, and `ohao_game` turns any excess into an exit code.

{{cite examples/ohao_game.cpp "if (r.readbackCopies != 0) return 3;"}}

## Three modes, two of which can be refused

`RenderMode` is one raster mode (Deferred) and two ray-traced ones (RTRealtime, RTOffline). The RT branch of `render()` is guarded by three separate conditions — a pipeline descriptor for the mode, a live renderer profile, and ray-tracing support reported by the render world — and falls through to deferred if any is missing.

{{cite ohao/gpu/vulkan/renderer.cpp "if (const auto* rtPipeline = getRTPipeline(m_renderMode);"}}

`setRenderMode` returns `void` and is allowed to refuse: if the device has no RT support, or the profile for the requested mode fails to initialise, it logs and leaves the previous mode in place. Deferred is never refused, because `initialize()` treats a failed deferred renderer as fatal — a renderer that initialised has one.

{{cite ohao/gpu/vulkan/renderer.cpp "Path tracing not available, staying in current mode"}}

The mode you asked for is therefore not necessarily the mode you got. The field starts at Deferred, so a refused RT request leaves the renderer rasterising.

{{cite ohao/gpu/vulkan/renderer.hpp "RenderMode m_renderMode{RenderMode::Deferred};"}}

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
`initialize()` builds the device, the deferred renderer and the render world (which owns the acceleration structures), but no `PathTracer`. Each profile owns one, and a `PathTracer` declares nineteen scalar `VkImage` members: beauty and accumulation; the albedo and normal guide pair; nine denoiser guide AOVs (motion, depth, roughness, diffuse and specular radiance, diffuse albedo, specular colour, packed normal-roughness, specular hit distance); NRD's denoised diffuse and specular and its composed HDR; the cinematic chain's pre-DoF and final tonemapped LDR; and the DLSS-RR HDR colour output — plus four image *arrays*: three bloom mips, ping-pong surface and shading history, and a 3×2 GI reservoir set. Only the DLSS output is `#ifdef`-guarded; the other eighteen are unconditional.

{{cite ohao/render/rt/path_tracer.hpp "VkImage        m_dlssColorOutImage"}}

Constructing both profiles eagerly doubles all of it. The rejected alternative is stated in the code: eager dual init can exhaust device memory at 4K.

{{cite ohao/gpu/vulkan/renderer.cpp "// created lazily on setRenderMode() (eager dual init at 4K can OOM)."}}

Laziness costs the scene nothing. Meshes, materials, textures and lights live in the render world from the moment they are registered, whether or not a profile exists, and each RT frame binds the world's buffers, its TLAS and the bindless texture views to the tracer. `ensureRTRenderer` therefore has no upload to repeat: the only thing it passes along is the texture table's capacity, so the tracer's binding 12 is sized to cover every index a material can hold.

{{cite ohao/gpu/vulkan/renderer.cpp "// RenderScene (prepareRTSceneForFrame / renderRTPipeline): nothing to re-upload."}}
:::

`ensureRTRenderer` is public but has no caller outside the class: `setRenderMode` is its only invocation in the tree. The header documents what it does and why it is lazy, and says nothing about who else is meant to call it.

{{cite ohao/gpu/vulkan/renderer.hpp "Lazy-create the PathTracer for"}}

## The settings that are thrown away every frame

Every RT frame begins by discarding the current settings and reloading the active pipeline's compile-time defaults.

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = pipeline.getDefaultSettings();"}}

That keeps the pipeline authoritative over tracer behaviour, but it means every user-settable field has to be rescued by hand across the reset. Three are stashed in locals and written back immediately: anisotropy strength, anisotropy rotation, subsurface strength.

{{cite ohao/gpu/vulkan/renderer.cpp "float preservedAnisoStrength = m_rtSettings.anisotropyStrength;"}}

A fourth, the realtime per-frame sample count set through `setRealtimeSamplesPerFrame`, is re-injected one level down in `applyRTRenderSettings` instead. Nothing in the tree calls that setter today, so the rescue keeps the value at its default of 1; it exists for a realtime host that exposes the sample count.

{{cite ohao/gpu/vulkan/renderer.cpp "// Re-inject the realtime per-frame sample count: prepareRTSceneForFrame"}}

:::key
`RTRenderSettings` is not a settings object in the usual sense — it is a per-frame scratch struct with a hand-maintained rescue list. Adding a field and a setter is not enough to make it stick; the field must also be named in `prepareRTSceneForFrame` or `applyRTRenderSettings`, or it silently reverts to the profile default on the very next frame.
:::

## Who wins the argument about the denoiser

`setDenoiseMode` latches a `m_denoiseModeOverridden` bit that does two things. It stops the per-frame settings sync from overwriting the user's choice with the profile default, and it gates an injection back into the tracer's own settings — scoped to the three denoisers the tracer dispatches itself, Atrous, DLSS-RR and NRD, and not to OIDN.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseModeOverridden &&"}}

NRD joined in sub-project 2b step 5. Its block used to be gated on `enableAuxiliaryAOVs` alone, a profile flag, so REBLUR ran on every RT frame of a profile that produces AOVs and nothing needed injecting for it to fire; gated on the mode too, it needs the injection like the other two.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD)"}}

All three blocks branch on `m_renderSettings.denoiseMode`, and the per-frame reload sets that field to `None` (realtime) or `OIDN` (offline) — never one of the three. Without the injection those branches are unreachable from the command line. OIDN needs nothing: it never enters the tracer at all.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser)"}}

Widening that condition to "always inject" would look like a cleanup and would clobber each profile's default. Those defaults matter: with no `--denoise` flag at all, RTOffline denoises with OIDN because its settings constant says so, while RTRealtime starts at None.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

## The frame you asked for, and no other

Frames go round a ring of three slots, each with its fence, command buffer and persistently mapped staging buffer.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

A request marks the next frame `render()` records. Only that frame copies its final image into its slot's staging buffer; a headless frame nobody asked for ends with its image and copies nothing.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A window frame presents (and copies too when asked); a headless frame copies only"}}

`readback()` then waits on that frame's fence alone, never the queue, and returns a view of its slot's buffer. So an example that renders ten frames and asks for the tenth writes the tenth, as a capture does. Path-traced loops render `spp` frames and read the last, which has accumulated `spp` samples. Until sub-project 2b step 2 every headless frame copied, a frame's copy was read three calls later, and the examples rendered `spp + 3` frames to land on the right one. `engine_tests`' `sp2b_t03` holds the line: a spinning cube's frame N read back equals a fresh renderer's single capture of step N, max_abs 0, where step N−3 differs by 243.

## readback() and the CPU denoisers

Most denoise modes need nothing from `readback()`. None, Atrous and DLSS-RR all end with the RGBA8 beauty in `m_outputImage`, which is the image the requested frame copies. Atrous filters that image in place. DLSS-RR runs `NGX_VULKAN_EVALUATE_DLSSD_EXT` into an RGBA16F colour target and then tonemaps that target into the same beauty image, so the standard copy picks it up unchanged. The tonemap runs only when NGX feature creation succeeded; where that fails, the copy is raw beauty.

{{cite ohao/render/rt/path_tracer_render.cpp "m_dlssRR->tonemap(cmd, m_dlssColorOutView, m_outputView"}}

Two modes do their work after the frame. On a path-traced frame under NRD or OIDN, `readback()` hands off to `denoiseRequestedFrame()` once the frame's fence has signalled. Nothing has been recorded since, so the images it reads belong to the requested frame.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "denoiseRequestedFrame() && m_denoisedPixelBuffer.size() == bytes)"}}

NRD reads back its own tonemapped RGBA8 image. OIDN waits for the device to go idle, copies the RGBA32F accumulation image plus the albedo and normal AOVs, filters them on the host and caches the result until the next `render()`. The filter is created on `oidn::DeviceType::Default`: OIDN picks the device, and nothing here pins it to the CPU, but the round trip through host memory and `vkDeviceWaitIdle` is unconditional. If either fails, the log says so and `readback()` returns the noisy frame from the staging buffer.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readbackImage(rtRenderer->getAccumImage(), beauty);"}}
{{cite ohao/render/rt/denoise/oidn_denoise.cpp "oidn::newDevice(oidn::DeviceType::Default)"}}

Below that sits the bulk of the translation unit, more than half of `renderer.cpp`: eleven near-identical readback helpers, seven pulling debug AOVs and four pulling NRD outputs. Each open-codes a staging buffer, a one-shot command buffer, a pair of layout transitions and a `vkQueueWaitIdle`. Ten are instrumentation, reachable only from env_demo's dump flags. The eleventh is not: `readbackNrdTonemapped` is how `--denoise=nrd` gets its picture, a full queue stall per read frame.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!readbackNrdTonemapped(rgba, rw, rh)) {"}}

## Contracts

- Call `requestReadback()` before the `render()` whose pixels you want and `readback()` after it. The span is that frame, valid until the next `render()` or `resize()`; it is empty when the frame was not rendered (minimized, an out-of-date swapchain) or had no RGBA8 image (tonemapping off).
- `requestedReadbackCount()` counts copies, every one of them requested. A host that counts more copies than it asked for has found a bug.
- `setRenderMode` may silently keep the previous mode. Read back `getRenderMode()` rather than trusting the argument.
- `resize()` destroys and recreates every RT image. The tracer rewrites its image bindings at the top of every `render()`, so nothing needs rebinding afterwards; a descriptor cached anywhere else would be stale.
- Any new `RTRenderSettings` field a caller can set must be added to the preserve list in `prepareRTSceneForFrame` or re-injected in `applyRTRenderSettings`, or the per-frame reload will discard it.
- `readback()` is not reentrant with GPU work in flight: under NRD or OIDN it allocates a command buffer, submits and waits on the graphics queue. It skips that while `m_denoiseCacheValid` holds, so only the first call after each `render()` costs the stall.
- `getDevice()`, `getPhysicalDevice()`, `graphicsQueue()` and `graphicsQueueFamily()` are wiring for consumers outside the renderer; `render_bench`'s GPU calibration kernel is the one in the tree. Work submitted on the queue must finish before the next `render()`.

{{cite ohao/gpu/vulkan/renderer.hpp "VkDevice getDevice() const noexcept { return m_device; }"}}
