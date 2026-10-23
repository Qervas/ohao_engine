---
module: gpu
id: renderer-facade
title: VulkanRenderer facade
standard: v2
figures: [gpu-renderer-facade-ring-lag]
---

## A renderer that does not own its window

`VulkanRenderer` creates its own instance, picks its own physical device, and creates a logical device and command pool. What it delivers depends on which `initialize` overload ran. The headless one delivers a `std::vector<uint8_t>` of RGBA8 pixels through `getPixels()`; the PNG writers, the golden smoke test and every example except `ohao_game` drive that `render()` → `getPixels()` pair. The windowed overload takes a `PresentTarget` and ends each frame by presenting into a swapchain, reading nothing back unless the caller asked for that frame with `requestReadback()`. That request works in both modes and is what the shared capture procedure (`runtime::captureFrame`, behind `ohao run --render` and `render.capture`) uses: it asks for its last frame and takes it from `readback()`, which returns that frame and not one from three calls earlier.

{{cite ohao/runtime/frame_capture.cpp "if (i == frames - 1) renderer.requestReadback();"}}

{{cite ohao/gpu/vulkan/renderer.hpp "[[nodiscard]] bool initialize(const PresentTarget& target);"}}

Neither overload makes the renderer a windowing client. The window belongs to `ohao/runtime`: `LiveHost` creates it, builds the renderer at the window's framebuffer size, passes the window's `PresentTarget` in, and forwards drawable-size changes through `setPresentExtent`. The renderer sees a surface-creation callback and an extent, never a window handle.

{{cite ohao/runtime/live_host.cpp "m_renderer->setPresentExtent(m_window.framebufferExtent());"}}

The two modes are distinguishable from outside. `isPresenting()` reports whether a swapchain exists, and `readbackCopyCount()` counts the headless deferred staging copies recorded since initialisation — zero in a windowed deferred renderer, by construction; copies a window makes because a frame was requested are counted apart, by `requestedReadbackCount()`. `ohao_game` turns the first into an exit code: a windowed run that recorded any unrequested readback copy fails.

{{cite examples/ohao_game.cpp "if (r.readbackCopies != 0) return 3;"}}

The counter is incremented only in `copyDeferredOutputToPixelBuffer`; the RT body's staging copy is not counted, so it proves nothing about an RT frame.

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

`setDenoiseMode` latches a `m_denoiseModeOverridden` bit that does two things. It stops the per-frame settings sync from overwriting the user's choice with the profile default, and it gates a narrow injection back into the tracer's own settings — scoped to Atrous and DLSS-RR, and not to NRD or OIDN.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseModeOverridden &&"}}

The comment above that condition explains the scoping as GPU-side dispatch versus everything else. That is not the real distinction — NRD is dispatched GPU-side from inside the same `PathTracer` frame. Its block is gated on `enableAuxiliaryAOVs`, a profile flag, not on `denoiseMode`, so REBLUR runs on every RT frame of a profile that produces AOVs and nothing needs injecting for it to fire.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs)"}}

The Atrous and DLSS-RR blocks branch on `m_renderSettings.denoiseMode` directly, and the per-frame reload sets that field to `None` (realtime) or `OIDN` (offline) — never Atrous, never DLSS-RR. Without the injection those two branches are unreachable from the command line. OIDN needs nothing either: it never enters the tracer at all.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser)"}}

Widening that condition to "always inject" would look like a cleanup and would clobber each profile's default. Those defaults matter: with no `--denoise` flag at all, RTOffline denoises with OIDN because its settings constant says so, while RTRealtime starts at None.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

## The pixel buffer is three frames behind

Headless rendering uses a ring of three frame slots.

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

Each `render()` waits on its slot's fence, copies that slot's staging buffer into `m_pixelBuffer`, and only *then* records and submits this frame's work into the same slot. The copy therefore delivers the image submitted three calls ago, not the one just issued.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Read back previous frame"}}

{{figure gpu-renderer-facade-ring-lag "Conceptual timing of the read-before-record ordering in render(); slot assignment and the depth of three are read from the code, not measured. The first three calls leave m_pixelBuffer unwritten."}}

Offline loops in this repo render `spp + 3` frames rather than `spp` — cornell_box, model_viewer and turntable; env_demo drops to `samples + 2` only when `--pan-x` makes it add one final frame after moving the camera.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : (samples + 3);"}}

Under `--denoise=none` that arithmetic lands exactly: after `spp + 3` calls the last copy delivered call `spp - 1`, the frame that had accumulated `spp` samples. No comment at any of those sites ties the `+ 3` to the ring depth, though, and it cannot be the whole story — the default offline configuration is `DenoiseMode::OIDN`, whose `getPixels()` branch never reads `m_pixelBuffer`. There the three extra frames buy three extra samples and compensate no lag.

A windowed deferred renderer has no lag to account for: the swapchain image is written by the same submission that recorded the frame, and `m_pixelBuffer` is never filled at all. `getPixels()` on a presenting renderer returns a buffer of the right size that nothing writes. A requested frame has no lag either, in both modes: `readback()` waits on the requested frame's own slot and returns that slot's staging buffer, which is why a capture keeps the last frame it renders where an example's `getPixels()` keeps the one three calls earlier.

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

Below that sits the bulk of the translation unit — more than half of `renderer.cpp`: eleven near-identical readback helpers, seven pulling debug AOVs and four pulling NRD outputs, each open-coding a staging buffer, a one-shot command buffer, a pair of layout transitions and a `vkQueueWaitIdle`. Ten are instrumentation, reachable only from env_demo's dump flags. The eleventh is not: `readbackNrdTonemapped` is how `--denoise=nrd` gets its picture at all, and because `render()` invalidates the cache every frame, that full queue stall runs once per displayed frame.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!self->readbackNrdTonemapped(rgba, rw, rh))"}}

## Contracts

- On the `m_pixelBuffer` paths — None, Atrous, DLSS-RR — a headless `render()` must be called at least `MAX_FRAMES_IN_FLIGHT` times before `getPixels()` returns real content; a single render then read yields an unwritten buffer. The NRD and OIDN branches never touch `m_pixelBuffer` and are correct after one call.
- A presenting renderer's deferred frames never reach `m_pixelBuffer`. Read pixels from a headless renderer, or request the frame (`requestReadback()` before `render()`, then `readback()`); `readbackCopyCount()` stays 0 in windowed deferred mode and a nonzero count there is a bug.
- `setRenderMode` may silently keep the previous mode. Read back `getRenderMode()` rather than trusting the argument.
- `resize()` destroys and recreates every RT image. The tracer rewrites its image bindings at the top of every `render()`, so nothing needs rebinding afterwards; a descriptor cached anywhere else would be stale.
- Any new `RTRenderSettings` field a caller can set must be added to the preserve list in `prepareRTSceneForFrame` or re-injected in `applyRTRenderSettings`, or the per-frame reload will discard it.
- `getPixels()` is not reentrant with GPU work in flight: on the NRD and OIDN branches it allocates a command buffer, submits and waits on the graphics queue. It skips that whenever `m_denoiseCacheValid` still holds, so only the first call after each `render()` costs the stall.
- `getDevice()` / `getPhysicalDevice()` currently have no callers in the tree; the header describes them as wiring for out-of-tree consumers.

{{cite ohao/gpu/vulkan/renderer.hpp "VkDevice getDevice() const noexcept { return m_device; }"}}
