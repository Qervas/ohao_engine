---
module: gpu
id: renderer-facade
title: VulkanRenderer facade
standard: v2
figures: []
---

## A renderer that does not own its window

`VulkanRenderer` creates its own instance, picks its own physical device, and creates a logical device and command pool. What a frame ends with depends on which `initialize` overload ran: the headless one ends it with the final image, the windowed one, which takes a `PresentTarget`, by presenting into a swapchain. In both modes the CPU reads a frame one way only: a request before the `render()` whose pixels are wanted, a read after it. The request is `requestReadback()` for the whole frame, read by `readback()`, or `requestPreviewReadback({rect, maxSide})` for a downscaled preview made on the GPU, read by `readbackPreview()`. The shared capture procedure (`runtime::captureFrame`, behind `ohao run --render` and `render.capture`), the examples, the PNG-writing tests and `ohao doctor`'s frame check all work this way; the capture picks the form in `requestCaptureReadback`.

{{cite ohao/runtime/frame_capture.cpp "bool requestCaptureReadback(VulkanRenderer& renderer, const api::RenderRequest& r, bool needPixels) {"}}

{{cite ohao/gpu/vulkan/renderer.hpp "[[nodiscard]] bool initialize(const PresentTarget& target);"}}

Neither overload makes the renderer a windowing client. The window belongs to `ohao/runtime`: `LiveHost` creates it, builds the renderer at the window's framebuffer size, passes the window's `PresentTarget` in, and forwards drawable-size changes through `setPresentExtent`. The renderer sees a surface-creation callback and an extent, never a window handle.

{{cite ohao/runtime/live_host.cpp "m_renderer->setPresentExtent(m_window.framebufferExtent());"}}

The two modes are distinguishable from outside: `isPresenting()` reports whether a swapchain exists. No frame in either mode copies anything to the CPU unless it was requested, so the one counter, `requestedReadbackCount()`, counts copies that were all asked for. The window host checks that against its own count of requests, and `ohao_game` turns any excess into an exit code.

{{cite examples/ohao_game.cpp "if (r.readbackCopies != 0) return 3;"}}

## Three modes, two of which can be refused

`RenderMode` is one raster mode (Deferred) and two ray-traced ones (RTRealtime, RTOffline). The RT branch of `render()` is guarded by three separate conditions — default settings for the mode, a live renderer profile, and ray-tracing support reported by the render world — and falls through to deferred if any is missing.

{{cite ohao/gpu/vulkan/renderer.cpp "if (const auto rtPipeline = rtDefaults(m_renderMode);"}}

`setRenderMode` returns `void` and is allowed to refuse: if the device has no RT support, or the profile for the requested mode fails to initialise, it logs and leaves the previous mode in place. Deferred is never refused, because `initialize()` treats a failed deferred renderer as fatal — a renderer that initialised has one.

{{cite ohao/gpu/vulkan/renderer.cpp "Path tracing not available, staying in current mode"}}

The mode you asked for is therefore not necessarily the mode you got. The field starts at Deferred, so a refused RT request leaves the renderer rasterising.

{{cite ohao/gpu/vulkan/renderer.hpp "RenderMode m_renderMode{RenderMode::Deferred};"}}

Anything that reports "rendering in mode X" from its own local variable rather than from `getRenderMode()` can print a lie.

## Defaults always, a tracer only on demand

The facade answers two questions about an RT mode separately. `rtDefaults(mode)` returns the profile's settings, or nothing for a raster mode, so "is this an RT mode, and what are its defaults" is answerable before any `PathTracer` exists. It is a member, not a static: the realtime default reads the physical device, and picks DLSS-RR where the build has the SDK and the GPU is NVIDIA's, else NRD. The profiles are `unique_ptr`s, each owning a whole `PathTracer`, and they are the thing that might be null.

{{cite ohao/gpu/vulkan/renderer.hpp "[[nodiscard]] std::optional<RTRenderSettings> rtDefaults(RenderMode mode) const;"}}

The two profiles are the same tracer with different settings: both install `PathTracerShaderSet`'s default, the path tracer in `rt/path_tracer.slang` (four named entry points in one file), before `PathTracer::init` runs.

{{cite ohao/render/rt/path_tracer.hpp "rt_path_tracer.slang.raygen.spv"}}

## Why the path tracer is not built during initialize()

:::why
`initialize()` builds the device, the deferred renderer and the render world (which owns the acceleration structures), but no `PathTracer`. Each profile owns one, and a `PathTracer` owns some two dozen images: the accumulation pair and the beauty; the albedo and normal guide pair; nine denoiser guide AOVs (motion, depth, roughness, diffuse and specular radiance, diffuse albedo, specular colour, packed normal-roughness, specular hit distance); NRD's denoised diffuse and specular and its composed HDR; the camera ray's emission guide; and the DLSS-RR HDR colour output. Only the DLSS output is `#ifdef`-guarded.

{{cite ohao/render/rt/path_tracer.hpp "VkImage        m_dlssColorOutImage"}}

Constructing both profiles eagerly doubles all of it. The rejected alternative is stated in the code: eager dual init can exhaust device memory at 4K.

{{cite ohao/gpu/vulkan/renderer.cpp "// created lazily on setRenderMode() (eager dual init at 4K can OOM)."}}

Laziness costs the scene nothing. Meshes, materials, textures and lights live in the render world from the moment they are registered, whether or not a profile exists, and each RT frame binds the world's buffers, its TLAS and the bindless texture views to the tracer. `ensureRTRenderer` therefore has no upload to repeat: the only thing it passes along is the texture table's capacity, so the tracer's binding 12 is sized to cover every index a material can hold.

{{cite ohao/gpu/vulkan/renderer.cpp "// RenderScene (prepareRTSceneForFrame / renderRTPipeline): nothing to re-upload."}}
:::

`ensureRTRenderer` is public but has no caller outside the class: `setRenderMode` is its only invocation in the tree. The header documents what it does and why it is lazy, and says nothing about who else is meant to call it.

{{cite ohao/gpu/vulkan/renderer.hpp "Lazy-create the PathTracer for"}}

## The settings that are thrown away every frame

Every RT frame begins by discarding the current settings and reloading the active profile's compile-time defaults.

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = defaults;"}}

That keeps the profile constants authoritative over tracer behaviour, but it means every user-settable field has to be re-applied across the reset. Two are, in `applyRTRenderSettings`: a denoise mode chosen through `setDenoiseMode`, and a bounce limit set through `setMaxBouncesOverride`.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_maxBouncesOverride) m_rtSettings.maxBounces = *m_maxBouncesOverride;"}}

:::key
`RTRenderSettings` is not a settings object in the usual sense — it is a per-frame scratch struct with a hand-maintained list of overrides. Adding a field and a setter is not enough to make it stick; the field must also be re-applied in `applyRTRenderSettings`, or it silently reverts to the profile default on the very next frame.
:::

## Who wins the argument about the denoiser

`setDenoiseMode` latches a `m_denoiseModeOverridden` bit that does two things. It stops the per-frame settings sync from overwriting the user's choice with the profile default, and it injects the choice back into the tracer's own settings, whatever it is, None and OIDN included.

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings.denoiseMode = m_denoiseMode;"}}

The injection is not only for the denoisers the tracer dispatches itself (à-trous, DLSS-RR and NRD, each behind a branch on `m_renderSettings.denoiseMode`). The trace's own behaviour follows the mode too: whether it keeps a running mean and whether the firefly clamp is on (`tracerAccumulates`, `rt_meta.hpp`). Without the injection, `--denoise none` on the realtime profile would still trace one sample a frame for an NRD that never runs.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser) {"}}

With no `--denoise` flag at all, the defaults decide: RTOffline denoises with OIDN; RTRealtime with DLSS-RR on an NVIDIA GPU in a build with the SDK, else NRD. A capture names its own denoiser and defaults to none.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}
{{cite ohao/gpu/vulkan/renderer.cpp "if (props.vendorID == 0x10DE) s.denoiseMode = DenoiseMode::DLSSRR;   // NVIDIA"}}

## The frame you asked for, and no other

Frames go round a ring of three slots, each with its fence, command buffer and persistently mapped staging buffer.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

A request marks the next frame `render()` records. Only that frame copies its final image into its slot's staging buffer; a headless frame nobody asked for ends with its image and copies nothing.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A window frame presents (and copies too when asked); a headless frame copies only"}}

`readback()` then waits on that frame's fence alone, never the queue, and returns a view of its slot's buffer. So an example that renders ten frames and asks for the tenth writes the tenth, as a capture does. Path-traced loops render `spp` frames and read the last, which has accumulated `spp` samples. Until sub-project 2b step 2 every headless frame copied, a frame's copy was read three calls later, and the examples rendered `spp + 3` frames to land on the right one. `engine_tests`' `sp2b_t03` holds the line: a spinning cube's frame N read back equals a fresh renderer's single capture of step N, max_abs 0, where step N−3 differs by 243.

A preview request is the same contract with less to read. In Deferred mode, when the `capture.preview` pipeline was created, the frame box-filters a rectangle of its final image down to a long side of at most `maxSide` and writes it straight into the staging buffer; it copies nothing else, and `readback()` on that frame returns an empty span. `previewOnGpu()` says whether a request will be served so; in an RT mode the request reads back the whole frame instead:

{{cite ohao/gpu/vulkan/renderer.hpp "return m_previewPass != nullptr && m_renderMode == RenderMode::Deferred;"}}

Three counters let a caller reason about the frames ahead of the one it reads. `framesInFlight()` asks the slot fences without waiting. `framesSubmitted()` counts frames that reached the queue, and `waitForSubmitted(n)` returns once the n-th has finished, by waiting on the slot that holds it (at once if that slot has been reused since):

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (m_slotSubmitted[s] == n) m_frameResources.waitForFrame(s);"}}

## readback() and the CPU denoiser

Most denoise modes need nothing from `readback()`. None, Atrous, DLSS-RR and NRD all end with the RGBA8 frame in `m_outputImage`, which is the image the requested frame copies and the one a window presents. Atrous filters that image in place. DLSS-RR runs `NGX_VULKAN_EVALUATE_DLSSD_EXT` into an RGBA16F colour target and then writes it into the same image with the tracer's curve; NRD's compose writes it there too, so the standard copy picks either up unchanged. The DLSS write runs only when NGX feature creation succeeded; where that fails, the copy is the raw trace.

{{cite ohao/render/rt/path_tracer_render.cpp "m_dlssRR->tonemap(cmd, frameSlot, m_dlssColorOutView, m_outputView, m_width, m_height);"}}

One mode does its work after the frame. On a path-traced frame under OIDN, `readback()` hands off to `denoiseRequestedFrame()` once the frame's fence has signalled. Nothing has been recorded since, so the images it reads belong to the requested frame.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "denoiseRequestedFrame() && m_denoisedPixelBuffer.size() == bytes)"}}

OIDN waits for the device to go idle, copies the RGBA32F accumulation image plus the albedo and normal AOVs, filters them on the host and caches the result until the next `render()`. The filter is created on `oidn::DeviceType::Default`: OIDN picks the device, and nothing here pins it to the CPU, but the round trip through host memory and `vkDeviceWaitIdle` is unconditional. If either fails, the log says so and `readback()` returns the noisy frame from the staging buffer.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readImageRGBA(rtRenderer->getAccumImage(), false, VK_IMAGE_LAYOUT_GENERAL, kRT, beauty);"}}
{{cite ohao/render/rt/denoise/oidn_denoise.cpp "oidn::newDevice(oidn::DeviceType::Default)"}}

Below that sits the bulk of the translation unit, about half of `renderer.cpp`: ten near-identical readback helpers pulling debug AOVs and NRD's intermediate images. Each open-codes a staging buffer, a one-shot command buffer, a pair of layout transitions and a `vkQueueWaitIdle`. All ten are instrumentation, reachable only from env_demo's dump flags.

{{cite ohao/gpu/vulkan/renderer.cpp "bool VulkanRenderer::readbackNrdComposed(std::vector<float>& data,"}}

## Contracts

- Call `requestReadback()` before the `render()` whose pixels you want and `readback()` after it. The span is that frame, valid until the next `render()` or `resize()`; it is empty when the frame was not rendered (minimized, an out-of-date swapchain) or had no RGBA8 image (tonemapping off).
- `requestPreviewReadback()` pairs with `readbackPreview()`, and only when `previewOnGpu()` held for the frame; otherwise the frame reads back in full and `readback()` returns it. The two requests replace each other: the last one before `render()` wins.
- `requestedReadbackCount()` counts copies, every one of them requested. A host that counts more copies than it asked for has found a bug.
- `setRenderMode` may silently keep the previous mode. Read back `getRenderMode()` rather than trusting the argument.
- `resize()` destroys and recreates every RT image. The tracer rewrites its image bindings at the top of every `render()`, so nothing needs rebinding afterwards; a descriptor cached anywhere else would be stale.
- Any new `RTRenderSettings` field a caller can set must be re-applied in `applyRTRenderSettings`, or the per-frame reload will discard it.
- `readback()` is not reentrant with GPU work in flight: under OIDN it allocates a command buffer, submits and waits on the graphics queue. It skips that while `m_denoiseCacheValid` holds, so only the first call after each `render()` costs the stall.
- `getDevice()`, `getPhysicalDevice()`, `graphicsQueue()` and `graphicsQueueFamily()` are wiring for consumers outside the renderer; `render_bench`'s GPU calibration kernel is the one in the tree. Work submitted on the queue must finish before the next `render()`.

{{cite ohao/gpu/vulkan/renderer.hpp "VkDevice getDevice() const noexcept { return m_device; }"}}
