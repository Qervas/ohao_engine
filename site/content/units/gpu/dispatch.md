---
module: gpu
id: dispatch
title: Render dispatch
standard: v2
---

## Two bodies that share a skeleton

`render_dispatch.cpp` is where "render one frame" becomes recorded Vulkan commands. It defines thirteen functions, two of which are whole-frame bodies — `renderDeferred` and `renderRTPipeline` — and `render()` picks exactly one of them per call: the RT body when the current mode has an RT pipeline, a created RT renderer and a render world that reports ray tracing available, the deferred body otherwise. The other eleven are support: `blitDeferredOutputToSwapchain` and `copyDeferredOutputToStaging` (the window's end of a deferred frame, and a requested headless frame's copy; next section), `readback`, `readbackPreview`, `recordPreview` and `finishReadbackRequest` (readback on request, same section), `framesInFlight` and `waitForSubmitted` (the frames still on the GPU), `resize`, `readbackHDRBuffers`, and a `readTerrainHeights` stubbed to `false`.

{{cite ohao/gpu/vulkan/renderer.cpp "rtPipeline && getRTRenderer(m_renderMode) && isRTSupported()) {"}}

Both bodies open with the same steps in the same order: take the current ring slot, wait on its fence, tick the render world's retire queue, reset the fence, then reset and re-record that slot's command buffer. The fence wait is what makes slot reuse safe: the frame that last ran on the slot, and any copy it made into the slot's staging buffer, has finished. The tick sits after every early return, so each tick matches a frame that is actually submitted on the slot, and `MAX_FRAMES_IN_FLIGHT` ticks cover every slot's fence:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Retire-queue tick. After the early returns above: every tick then matches a"}}

The first command each body records is `RenderScene::recordUpdates` for the slot: the staged table copies and the in-frame TLAS build or refit, ahead of any pass that reads them. The two prologues are copies, not calls to a shared helper.

`FrameResources` holds exactly what those prologues touch — a command buffer, a fence and a mapped staging buffer per slot — and nothing else. `DeferredRenderer` and `PathTracer` own their own descriptors and uniform storage.

{{cite ohao/render/frame/frame_resources.hpp "void* stagingBufferMapped{nullptr};"}}

## Two ends for a deferred frame

A deferred frame ends in one of two places, decided by whether `initialize(const PresentTarget&)` built a swapchain. Windowed, it is blitted into an acquired swapchain image and presented. Headless, it ends with its final image and does nothing more. In both, nothing is copied to the CPU unless the frame was asked for.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A window frame presents (and copies too when asked); a headless frame copies only"}}

Asking is `requestReadback()` before the `render()` call. The request belongs to that one frame: `renderDeferred` takes it with `std::exchange`. A headless frame then copies its final image into the slot's staging buffer (`copyDeferredOutputToStaging`). A windowed frame copies the same image, already a transfer source for the blit, inside `blitDeferredOutputToSwapchain`. `readback()` waits on that slot's fence and returns the bytes: the frame just rendered. The RT body copies its output only for a requested frame too. Every copy is a requested one, which `requestedReadbackCount()` counts.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A request belongs to this frame; a frame that is not rendered leaves no readback."}}

A preview request (`requestPreviewReadback`) is taken the same way. After the deferred passes, and before the present blit or the headless copy, `recordPreview` records `capture.preview`, which writes the downscaled preview into the slot's staging buffer; the end of the frame then copies nothing (the blit gets a null staging buffer). When the pass cannot run — no pipeline, no RGBA8 final image, a rectangle outside the frame — the frame falls back to the full copy:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "const bool previewed = wantReadback && preview && recordPreview(cmd, *preview);"}}

The RT body drops a preview request and reads back in full: the path tracer has no `capture.preview`. Each body stamps its slot with the running count of submitted frames as it submits, which `waitForSubmitted(n)` uses to find the slot whose fence ends frame n.

The windowed path changes the prologue in two places. Swapchain recreation runs first, before a slot is chosen, because recreating calls `resize()`, which idles the device and resets `m_currentFrame`; recreating after the slot was taken would wait on one slot's fence and reset another's.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Rebuild the swapchain BEFORE taking a frame slot. Recreating resizes the"}}

Image acquisition then sits between the fence wait and the fence reset, so every early return — out-of-date, acquire error — leaves the slot's fence signalled and the next wait on it cannot deadlock. The acquire signals that slot's own image-acquired semaphore; the submit waits on it at the transfer stage and signals the render-finished semaphore of the *image*, not the slot. A slot-indexed render-finished semaphore could still be pending on a presentation of a different image when the slot comes round again, so `Swapchain` creates one per image.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (m_swapchain) {   // the same per-slot semaphores as the deferred path"}}

`vkQueuePresentKHR` returning `OUT_OF_DATE` or `SUBOPTIMAL` only marks the swapchain dirty; the rebuild happens at the top of the next frame, as does one requested by `setPresentExtent` when the window's drawable size changes.

The RT body presents the same way. It rebuilds the swapchain and acquires an image at the same points, blits the tracer's RGBA8 output into the image with a requested readback copy in the same pass (`pt.present`, after `pt.render` in the registry's frame order), and submits and presents with the same semaphores. A headless RT frame copies only when asked (`pt.readback`).

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool VulkanRenderer::blitRTOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkImage src, VkBuffer staging) {"}}

## Three ways to reach a transfer layout

A presented or requested frame's last command reads the final image as a transfer source: `vkCmdBlitImage` into the swapchain, or `vkCmdCopyImageToBuffer` into the ring slot's staging buffer. The RT path delegates the layout: `PathTracer::render` ends with its own barrier on the output image, and the dispatch code records the assumption in a comment instead of re-checking it.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// The path tracer's output (RGBA8, tonemapped, left in TRANSFER_SRC_OPTIMAL by render()):"}}

The deferred path pays for the transition itself, once in each of its two ends, with the previous layout written as a constant both times.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;"}}

{{cite ohao/gpu/vulkan/render_dispatch.cpp "b[0].image = src;    // same layout assumption copyDeferredOutputToStaging makes"}}

That constant is a cross-module contract compiled into a barrier. It holds today because both render passes that can produce the final image, tonemap and lighting, end in `SHADER_READ_ONLY_OPTIMAL`. Any post-process step appended after tonemapping that leaves the final image in `GENERAL` turns both barriers into invalid transitions with no compile-time warning. Both ends put the image back in `SHADER_READ_ONLY_OPTIMAL` after reading it, so every deferred frame ends in the same state whether it was presented, read, or neither.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Back to where the frame left it, as the window's blit does: a frame ends in the same"}}

The third mechanism is the swapchain image, which is taken from `UNDEFINED` — its old contents discarded — to `TRANSFER_DST_OPTIMAL` for the blit and to `PRESENT_SRC_KHR` after it. When there is no final image at all, the same branch clears the swapchain image to black instead of presenting whatever it last held.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// No deferred output this frame: present black rather than garbage."}}

The blit is a blit and not a copy because the two images need not match: the deferred extent and the swapchain extent can differ for a frame while a resize is pending, and `VK_FILTER_LINEAR` scales across the difference. The swapchain prefers `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` and takes the surface's first format only when neither is offered, because the tonemap shader has already gamma-encoded the image and an sRGB swapchain would encode it a second time on the blit.

{{cite ohao/gpu/vulkan/swapchain.cpp "// UNORM, not sRGB: the deferred output is already gamma-encoded by the tonemap"}}

## The eight-byte image the readback cannot hold

`DeferredRenderer::getFinalOutputImage()` returns the post-processing output only when post-processing actually executed; otherwise it falls back to the lighting pass's own image.

{{cite ohao/render/deferred/deferred_renderer.cpp "        return m_lightingPass->getOutputImage();"}}

Those two images do not have the same format. The tonemapped output is `R8G8B8A8_UNORM`; the lighting pass output is the HDR target.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "target.format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

Every staging buffer in the ring is sized for four bytes per pixel.

{{cite ohao/gpu/vulkan/device_setup.cpp "const size_t stagingBufferSize = static_cast<size_t>(m_width) * m_height * 4;   // RGBA8"}}

`vkCmdCopyImageToBuffer` derives its byte count from the source format and extent, not from the destination buffer, so on the fallback branch a headless copy would read eight bytes per pixel out of the image and write them into an allocation sized for four: an overrun of the destination, not a benign truncation. `copyDeferredOutputToStaging` therefore asks which image it got before recording anything, and refuses the HDR one with a single warning; the request then goes unserved, and `readback()` returns an empty span.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!m_deferredRenderer->finalOutputIsRGBA8()) {"}}

The windowed end is format-tolerant where the copy is not (its requested-readback copy makes the same RGBA8 check and is skipped otherwise): `vkCmdBlitImage` converts between formats, and `createPresentation` refuses to start unless both candidate formats support `BLIT_SRC` on the device.

{{cite ohao/gpu/vulkan/renderer.cpp "for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {"}}

What the blit shows on the fallback is linear, un-tonemapped HDR clamped into eight bits — the wrong picture, but a valid command: the lighting pass creates its HDR image with transfer-source usage precisely so it can be the blit's source.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "VK_IMAGE_USAGE_TRANSFER_SRC_BIT |   // final image when post-processing is off: readback copy / swapchain blit"}}

The fallback is reachable from more than one switch. The tonemap step that produces the four-byte image is skipped when `m_tonemappingEnabled` is false, when the HDR input view is null, and when `m_tonemapPipeline`, `m_tonemapFramebuffer` or this frame's entry of `m_tonemapDescSets` is null; `getFinalOutputImage` additionally falls back when `m_postProcessing` itself is null. The one flag that reaches it deliberately is public — it is bound into the `ohao_renderer` Python module as `set_tonemapping_enabled`.

{{cite tests/python/renderer_bindings.cpp "&PostProcessingPipeline::setTonemappingEnabled"}}

## Two cameras for one scene

The dispatcher is also where camera state becomes GPU state, and it does that two different ways. The deferred path hands `DeferredRenderer` the camera's own matrix, which that class flips internally.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);"}}

The RT path discards the camera's matrix and builds a fresh one from the camera's FOV, deliberately unflipped because the raygen shader resolves image-space Y itself.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);"}}

The comment above that line says it uses the same FOV and aspect as deferred, for consistent framing. Only the FOV half of that reaches a ray. `PathTracer::render` inverts the matrix into the 256-byte push-constant block, and the ray-generation code in both raygens the two RT profiles bind — `pt_raygen_realtime.rgen` for `RTRealtime`, `rt/path_tracer.slang`'s `raygen` for `RTOffline` — takes exactly one number out of `invProj`: element `[1][1]`, which for a `glm::perspective` matrix is $\tan(\theta_v/2)$ and carries no aspect at all. The horizontal extent is rebuilt inside the shader from the framebuffer dimensions the push constant already carries in `params.xy`.

{{cite shaders/rt/pt_raygen_realtime.rgen "float tanFovX = tanFovY * aspect;"}}
{{cite shaders/rt/path_tracer.slang "const float tanX = tanY * float(size.x) / float(size.y);"}}

So the aspect argument at the `glm::perspective` call reaches no ray: passing 1.6 there would leave every primary direction bit-identical. It is not wholly dead — the realtime raygen inverts `invProj` back into a view-projection matrix for the motion-vector AOV, and that use is aspect-sensitive (the reference reads the previous frame's `prevViewProj` instead) — but it has no say in the framing.

{{cite shaders/rt/pt_raygen_realtime.rgen "mat4 currViewProj = inverse(pc.invProj) * inverse(pc.invView);"}}

The framing still diverges, one layer lower down.

Horizontal field of view follows from the vertical one and whatever aspect the consumer actually applies:

$$\tan\frac{\theta_h}{2} = a\,\tan\frac{\theta_v}{2}$$

Here $\theta_v$ is `Camera::getFov()` on both paths, and $a$ is the aspect the pixel is finally shaded with, not necessarily the one written at the call site. Deferred inherits $a$ from the matrix `Camera` built. `VulkanRenderer` default-constructs its `Camera` — whose signature default is 16:10 — and immediately overwrites the aspect from the extent it was given, then again on every `resize()` that changes the extent.

{{cite ohao/gpu/vulkan/renderer.cpp "m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));"}}

The RT raygen takes $a$ from the framebuffer instead. Both therefore agree whenever the camera is the renderer's own: a 1920×1080 frame is framed identically in Deferred and in either path-tracing profile, headless or windowed. The two only part ways if someone sets an aspect on the camera that is not the extent's — the RT image ignores it, the deferred one follows it.

## The far plane the cascades believe in

The `setCameraData` call cited above passes two scalars beside the matrix — `0.1f, 1000.0f` — written as literals rather than read from the camera that produced the matrix. `Camera`'s constructor defaults the far plane to 100, and since `setPerspectiveProjection` has no callers, that default is what every deferred frame rasterises with.

{{cite ohao/render/camera/camera.hpp "float farPlane = 100.0f"}}

Those scalars are not decoration. `DeferredRenderer` forwards them to `CSMPass::setCameraData`, which recomputes the cascade splits from them on the spot, blending a logarithmic and a linear subdivision of the near-to-far interval.

{{cite ohao/render/deferred/csm_pass.cpp "float ratio = m_farPlane / m_nearPlane;"}}

The four cascades are therefore distributed over a range ten times deeper than the projection can draw. The deepest split is the far plane itself, so the last cascade's entire extent — from wherever cascade three ends out to 1000 — lies past the point at which geometry is already clipped, and the shadow-map resolution budgeted for it is spent on nothing. This is a sharper defect than the aspect mismatch above, and it has the same cause: a scalar written at the call site instead of read from the object that owns it.

## Where the path tracer's light comes from

The RT dispatch hands the tracer a command buffer, the frame slot and the camera matrices, and nothing that describes a light:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->render(cmd, m_currentFrame, ptView, ptProj);"}}

`view` and `proj` reach the shaders as `invView` / `invProj` in the 256-byte push-constant block. The lights the tracer samples arrive as the render world's RT light buffer, whose handle `setSceneBuffers` copies into the tracer right after `recordUpdates`, together with the TLAS and the mesh, material and instance tables.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->setSceneBuffers(*m_renderScene);"}}

The path tracer is therefore lit by exactly the lights the scene holds; there is no number at the call site to edit.

## The lights the deferred renderer sees

`renderDeferred` decides, every frame, which lights the deferred renderer sees: the render world's raster light table with its version, and the scene's shadow-casting lights for RT shadows. Nothing is added to them. A scene without lights hands over none and is lit by its environment, or by the constant ambient without one, as the path tracer lights it:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "m_deferredRenderer->setLights(lights, version, m_ambientOverride.value_or(0.35f));"}}

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setShadowLights(m_renderScene->shadowLights());"}}

The version is the light table's counter, so an unchanged table uploads nothing.

:::why
Both bodies pipeline instead of blocking: `render()` never waits for its own submission, only for the frame that last used its slot, three submissions back. Pixels reach the CPU only through a request, and the wait for them is the caller's choice: `readback()` blocks on that one frame's fence. A loop that never reads pays for no copy and no wait. A capture pays exactly one wait, for the frame it keeps, and an inline-only capture of a deferred frame copies a preview of about 1.3 MB instead of a 3.7 MB frame. The ring this replaced copied every headless frame and handed back the one three calls earlier: correct pixels only after the ring had turned over, and a copy paid on every frame nobody read.
:::

:::key
Nothing in this file checks that the modes agree with each other. Dispatch is the last point where camera, layout and light state are translated into GPU state, and it translates them differently per mode: two projection paths, owners of the same layout transition on both sides of a module boundary, near/far literals that contradict the matrix they travel with, and one mode that can present while the other cannot. Differences between modes that look like renderer bugs are usually decided here, not in a shader.
:::

## Contracts

- The image returned by `getFinalOutputImage()` must already be in `SHADER_READ_ONLY_OPTIMAL`, and for a requested copy it must also be four bytes per pixel. The layout half holds on both branches: the lighting pass's own render pass also ends in `SHADER_READ_ONLY_OPTIMAL`. The format half holds only when post-processing's tonemap step ran, and the copy checks it (`finalOutputIsRGBA8`).
- `resize()` must call `resizeStagingBuffers` and reset `m_currentFrame` to 0 before the next `render()`. `resizeStagingBuffers` destroys, re-creates and re-maps all three slot buffers at the new size; skip it and `readback()` returns `width * height * 4` bytes of an allocation still sized for the old dimensions.
- Swapchain recreation must stay ahead of `getFrame(m_currentFrame)` in `renderDeferred`. `recreatePresentation` calls `resize()`, and `resize()` resets `m_currentFrame`; any recreation point after the slot is taken waits on one slot's fence and resets another's.
- Every early return between `waitForFrame` and `resetFrame` leaves the fence signalled; every return after `resetFrame` must submit with that fence, or the next frame on the slot waits forever.
- The ring permits up to three overlapping submissions, and `PathTracer` keeps one descriptor set per slot: `render()` rewrites only `m_descriptorSets[frameSlot]`, after that slot's fence wait, so a set is never rewritten while a frame that read it is in flight. That discipline carries the ping-ponged history bindings (13-16) as well as the idempotent ones. A new binding written into a shared object instead of the slot's set would reintroduce the hazard.
- Semaphores order only acquire, render and present on the single graphics queue, and only in windowed deferred frames; the per-slot fence is the only CPU-GPU synchronisation in the frame path. Introducing a second queue (async compute, a transfer queue for readback) requires new semaphores, because nothing here would order that work.
