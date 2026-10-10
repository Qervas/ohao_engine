---
module: gpu
id: dispatch
title: Render dispatch
standard: v2
---

## Two bodies that share a skeleton

`render_dispatch.cpp` is where "render one frame" becomes recorded Vulkan commands. It defines seven functions, two of which are whole-frame bodies — `renderDeferred` and `renderRTPipeline` — and `render()` picks exactly one of them per call: the RT body when the current mode has an RT pipeline, a created RT renderer and a render world that reports ray tracing available, the deferred body otherwise. The other five are support: `blitDeferredOutputToSwapchain` and `copyDeferredOutputToPixelBuffer` (the two ends a deferred frame can reach, next section), `resize`, `readbackHDRBuffers`, and a `readTerrainHeights` stubbed to `false`.

{{cite ohao/gpu/vulkan/renderer.cpp "rtPipeline && getRTRenderer(m_renderMode) && isRTSupported()) {"}}

Both bodies open with the same steps in the same order: take the current ring slot, wait on its fence, memcpy its staging buffer into the CPU pixel vector, tick the render world's retire queue, reset the fence, then reset and re-record that slot's command buffer. Reading before recording is what makes slot reuse safe: the staging buffer is both the destination of the copy this frame records and the source of the readback this frame performs, and the fence guarantees the older copy has landed. The tick sits after every early return, so each tick matches a frame that is actually submitted on the slot, and `MAX_FRAMES_IN_FLIGHT` ticks cover every slot's fence:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Retire-queue tick. After the early returns above: every tick then matches a"}}

The first command each body records is `RenderScene::recordUpdates` for the slot: the staged table copies and the in-frame TLAS build or refit, ahead of any pass that reads them. The two prologues are copies, not calls to a shared helper.

`FrameResources` holds exactly what those prologues touch — a command buffer, a fence and a mapped staging buffer per slot — and nothing else. `DeferredRenderer` and `PathTracer` own their own descriptors and uniform storage.

{{cite ohao/render/frame/frame_resources.hpp "void* stagingBufferMapped{nullptr};"}}

## Two ends for a deferred frame

A deferred frame ends in one of two places, decided by whether `initialize(const PresentTarget&)` built a swapchain. Headless, it is copied into the slot's staging buffer and read back `MAX_FRAMES_IN_FLIGHT` frames later; windowed, it is blitted into an acquired swapchain image and presented, and nothing is read back.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "else             copyDeferredOutputToPixelBuffer(cmd);   // headless readback"}}

The windowed path changes the prologue in two places. Swapchain recreation runs first, before a slot is chosen, because recreating calls `resize()`, which idles the device and resets `m_currentFrame`; recreating after the slot was taken would wait on one slot's fence and reset another's.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Rebuild the swapchain BEFORE taking a frame slot. Recreating resizes the"}}

Image acquisition then sits between the fence wait and the fence reset, so every early return — out-of-date, acquire error — leaves the slot's fence signalled and the next wait on it cannot deadlock. The acquire signals that slot's own image-acquired semaphore; the submit waits on it at the transfer stage and signals the render-finished semaphore of the *image*, not the slot. A slot-indexed render-finished semaphore could still be pending on a presentation of a different image when the slot comes round again, so `Swapchain` creates one per image.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "renderFinished = m_swapchain->renderFinished(imageIndex);"}}

`vkQueuePresentKHR` returning `OUT_OF_DATE` or `SUBOPTIMAL` only marks the swapchain dirty; the rebuild happens at the top of the next frame, as does one requested by `setPresentExtent` when the window's drawable size changes.

The RT body has no present path. It always records the staging copy and never acquires, so a windowed renderer switched to `RTRealtime` or `RTOffline` renders frames nobody sees. The windowed host does not offer the choice: `Application::initialize` pins the mode to Deferred.

{{cite ohao/runtime/application.cpp "m_renderer->setRenderMode(RenderMode::Deferred);"}}

## Three ways to reach a transfer layout

Every frame's last command reads the final image as a transfer source — `vkCmdCopyImageToBuffer` into the ring slot's staging buffer, or `vkCmdBlitImage` into the swapchain. The RT path delegates the layout: `PathTracer::render` ends with its own barrier on the output image, and the dispatch code records the assumption in a comment instead of re-checking it.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Output is already in TRANSFER_SRC_OPTIMAL from render()"}}

The deferred path pays for the transition itself, once in each of its two ends, with the previous layout written as a constant both times.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;"}}

{{cite ohao/gpu/vulkan/render_dispatch.cpp "b[0].image = src;    // same layout assumption copyDeferredOutputToPixelBuffer makes"}}

That constant is a cross-module contract compiled into a barrier. It holds today because both render passes that can produce the final image — tonemap and lighting — end in `SHADER_READ_ONLY_OPTIMAL`. Any post-process step appended after tonemapping that leaves the final image in `GENERAL` turns both barriers into invalid transitions with no compile-time warning. The two ends also disagree about where they leave the image: the blit transitions it back to `SHADER_READ_ONLY_OPTIMAL`, the headless copy leaves it in `TRANSFER_SRC_OPTIMAL`. That is harmless only because both render passes declare `initialLayout = UNDEFINED` and discard whatever layout they find.

The third mechanism is the swapchain image, which is taken from `UNDEFINED` — its old contents discarded — to `TRANSFER_DST_OPTIMAL` for the blit and to `PRESENT_SRC_KHR` after it. When there is no final image at all, the same branch clears the swapchain image to black instead of presenting whatever it last held.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// No deferred output this frame: present black rather than garbage."}}

The blit is a blit and not a copy because the two images need not match: the deferred extent and the swapchain extent can differ for a frame while a resize is pending, and `VK_FILTER_LINEAR` scales across the difference. The swapchain prefers `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` and takes the surface's first format only when neither is offered, because the tonemap shader has already gamma-encoded the image and an sRGB swapchain would encode it a second time on the blit.

{{cite ohao/gpu/vulkan/swapchain.cpp "// UNORM, not sRGB: the deferred output is already gamma-encoded by the tonemap"}}

## The eight-byte image the readback cannot hold

`DeferredRenderer::getFinalOutputImage()` returns the post-processing output only when post-processing actually executed; otherwise it falls back to the lighting pass's own image.

{{cite ohao/render/deferred/deferred_renderer.cpp "        return m_lightingPass->getOutputImage();"}}

Those two images do not have the same format. The tonemapped output is `R8G8B8A8_UNORM`; the lighting pass output is the HDR target.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_hdrOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

Every staging buffer in the ring is sized for four bytes per pixel.

{{cite ohao/gpu/vulkan/device_setup.cpp "const size_t stagingBufferSize = static_cast<size_t>(m_width) * m_height * 4;   // RGBA8"}}

`vkCmdCopyImageToBuffer` derives its byte count from the source format and extent, not from the destination buffer, so on the fallback branch a headless copy would read eight bytes per pixel out of the image and write them into an allocation sized for four — an overrun of the destination, not a benign truncation. `copyDeferredOutputToPixelBuffer` therefore asks which image it got before recording anything, and refuses the HDR one with a single warning; the pixel buffer keeps its previous contents.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!m_deferredRenderer->finalOutputIsRGBA8()) {"}}

The windowed end is format-tolerant where the copy is not: `vkCmdBlitImage` converts between formats, and `createPresentation` refuses to start unless both candidate formats support `BLIT_SRC` on the device.

{{cite ohao/gpu/vulkan/renderer.cpp "for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT}) {"}}

What the blit shows on the fallback is linear, un-tonemapped HDR clamped into eight bits — the wrong picture, but a valid command: the lighting pass creates its HDR image with transfer-source usage precisely so it can be the blit's source.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // final image when post-processing is off: readback copy / swapchain blit"}}

The fallback is reachable from more than one switch. The tonemap step that produces the four-byte image is skipped when `m_tonemappingEnabled` is false, when the HDR input view is null, and when `m_tonemapPipeline`, `m_tonemapFramebuffer` or this frame's entry of `m_tonemapDescSets` is null; `getFinalOutputImage` additionally falls back when `m_postProcessing` itself is null. The one flag that reaches it deliberately is public — it is bound into the `ohao_renderer` Python module as `set_tonemapping_enabled`.

{{cite tests/python/renderer_bindings.cpp "&PostProcessingPipeline::setTonemappingEnabled"}}

## Two cameras for one scene

The dispatcher is also where camera state becomes GPU state, and it does that two different ways. The deferred path hands `DeferredRenderer` the camera's own matrix, which that class flips internally.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);"}}

The RT path discards the camera's matrix and builds a fresh one from the camera's FOV, deliberately unflipped because the raygen shader resolves image-space Y itself.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);"}}

The comment above that line says it uses the same FOV and aspect as deferred, for consistent framing. Only the FOV half of that reaches a ray. `PathTracer::render` inverts the matrix into the 256-byte push-constant block, and the ray-generation code in both raygens the two RT profiles bind — `pt_raygen_realtime.rgen` for `RTRealtime`, `pt_raygen_offline.rgen` for `RTOffline` — takes exactly one number out of `invProj`: element `[1][1]`, which for a `glm::perspective` matrix is $\tan(\theta_v/2)$ and carries no aspect at all. The horizontal extent is rebuilt inside the shader from the framebuffer dimensions the push constant already carries in `params.xy`.

{{cite shaders/rt/pt_raygen_realtime.rgen "float tanFovX = tanFovY * aspect;"}}

So the aspect argument at the `glm::perspective` call reaches no ray: passing 1.6 there would leave every primary direction bit-identical. It is not wholly dead — the same raygens invert `invProj` back into a view-projection matrix for the motion-vector AOV, and that use is aspect-sensitive — but it has no say in the framing.

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

## The light that goes nowhere

The RT dispatch closes its `render` call with four arguments that read like the scene's key light — a position four units up, intensity 30, warm white, radius 1.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::vec3(0.0f, 4.0f, 0.0f), 30.0f,"}}

`PathTracer::render` accepts all four and uses none of them — they appear in the signature and nowhere in the body; only `view` and `proj` survive, as `invView` / `invProj` in the 256-byte push-constant block. The lights the tracer samples arrive instead as the render world's RT light buffer, whose handle `setSceneBuffers` copies into the tracer right after `recordUpdates`, together with the TLAS and the mesh, material and instance tables. Deleting the four parameters would change no pixel — which is why they are worth naming: they invite someone to brighten the scene by editing a number that has no consumer.

## The sun that overwrites your only light

`renderDeferred` does something no other body does: it edits the frame slot's light buffer after filling it. When the buffer reports exactly one light, it replaces `lights[0].direction` with the sky pass's sun direction, blends that toward a cool moon direction at 8% intensity as `nightFactor` rises, recomputes the shadow matrix, and lerps ambient intensity from 0.12 down to 0.04.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (ld->numLights == 1) {"}}

The intent is stated where the buffer is filled: a scene with no lights gets one fabricated directional light, and the comment says its direction is synced by `renderDeferred`.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "// If no lights in scene, add a default directional light (direction synced by renderDeferred)"}}

But `numLights == 1` cannot tell a fabricated light from an authored one. A scene with a single directional light — the ordinary outdoor case — has its authored direction silently overwritten by the sky's sun on every deferred frame. The fix is a flag on the fallback light, not a count; the count is genuinely ambiguous.

:::why
Both bodies pipeline instead of blocking. Neither waits for its own submission: the headless readback hands back the frame finished `MAX_FRAMES_IN_FLIGHT` submissions ago, and the windowed path never reads back at all. A blocking design — submit, wait on the fence, map and copy in the same call — would make the pixels correct on the very first call, and would cost a full CPU-GPU round trip per frame with the CPU idle for the whole GPU pass. The ring trades correct-on-first-call for throughput and accepts a readback that lags by the ring depth; a headless caller that wants the frame it just recorded has to call `render()` until the ring has turned over. Presentation removes the lag rather than hiding it, because the image the swapchain shows is written by the same submission that recorded it.
:::

:::key
Nothing in this file checks that the modes agree with each other. Dispatch is the last point where camera, layout and light state are translated into GPU state, and it translates them differently per mode: two projection paths, owners of the same layout transition on both sides of a module boundary, near/far literals that contradict the matrix they travel with, one mode that rewrites the light buffer behind the scene's back, and one mode that can present while the other cannot. Differences between modes that look like renderer bugs are usually decided here, not in a shader.
:::

## Contracts

- The image returned by `getFinalOutputImage()` must already be in `SHADER_READ_ONLY_OPTIMAL`, and for the headless copy it must also be four bytes per pixel. The layout half holds on both branches — the lighting pass's own render pass also ends in `SHADER_READ_ONLY_OPTIMAL` — but the format half holds only when post-processing's tonemap step ran, and nothing checks it.
- `resize()` must call `resizeStagingBuffers` and reset `m_currentFrame` to 0 before the next `render()`. `resizeStagingBuffers` destroys, re-creates and re-maps all three slot buffers at the new size; skip it and the per-frame `memcpy` reads `width * height * 4` bytes from an allocation still sized for the old dimensions.
- Swapchain recreation must stay ahead of `getFrame(m_currentFrame)` in `renderDeferred`. `recreatePresentation` calls `resize()`, and `resize()` resets `m_currentFrame`; any recreation point after the slot is taken waits on one slot's fence and resets another's.
- Every early return between `waitForFrame` and `resetFrame` leaves the fence signalled; every return after `resetFrame` must submit with that fence, or the next frame on the slot waits forever.
- The ring permits up to three overlapping submissions, and `PathTracer` keeps one descriptor set per slot: `render()` rewrites only `m_descriptorSets[frameSlot]`, after that slot's fence wait, so a set is never rewritten while a frame that read it is in flight. That discipline carries the ping-ponged history bindings (13-16) as well as the idempotent ones. A new binding written into a shared object instead of the slot's set would reintroduce the hazard.
- Semaphores order only acquire, render and present on the single graphics queue, and only in windowed deferred frames; the per-slot fence is the only CPU-GPU synchronisation in the frame path. Introducing a second queue (async compute, a transfer queue for readback) requires new semaphores, because nothing here would order that work.
