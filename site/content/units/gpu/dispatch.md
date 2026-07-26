---
module: gpu
id: dispatch
title: Render dispatch
standard: v2
---

## Four bodies that share a skeleton

`render_dispatch.cpp` is where "render one frame" becomes recorded Vulkan commands. It defines eight functions, four of which are whole-frame bodies — `renderDeferred`, `renderRTPipeline`, `renderMultiFrame` (the forward path) and `renderLegacy` — and `render()` picks exactly one of those four per call. The other four are support: `copyDeferredOutputToPixelBuffer` (the subject of the next section), `resize`, `readbackHDRBuffers`, and a `readTerrainHeights` stubbed to `false`. The first three bodies open with the same five steps in the same order: take the current ring slot, wait on its fence, memcpy its staging buffer into the CPU pixel vector, reset the fence, then reset and re-record that slot's command buffer. Reading before recording is what makes slot reuse safe: the staging buffer is both the destination of the copy this frame records and the source of the readback this frame performs, and the fence guarantees the older copy has landed.

Those three prologues are copies, not calls to a shared helper.

What they do not share is how much of `FrameResources` they consume. The struct replicates six things per slot — command buffer, fence, camera UBO, light UBO, descriptor set, staging buffer. Only the forward path touches the middle three: it writes the per-frame UBOs through `updateUniformBuffer(frameIndex)` / `updateLightBuffer(frameIndex)` and binds `frame.descriptorSet` for both the shadow pass and the main pass.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "renderShadowPass(cmd, frame.descriptorSet);"}}

The deferred and RT bodies use three fields only — `commandBuffer`, `renderFence`, `stagingBuffer` — because `DeferredRenderer` and `PathTracer` own their own descriptors and uniform storage. Half of `FrameResources` therefore exists for the one mode nothing in the repo ships.

## Three ways to reach TRANSFER_SRC_OPTIMAL

Every body ends in `vkCmdCopyImageToBuffer`. The three pipelined bodies copy into the current ring slot's staging buffer; `renderLegacy` copies into `m_stagingBuffer`, a single non-ring allocation created alongside the offscreen framebuffer.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_stagingBuffer, 1, &region);"}}

Four bodies, but only three mechanisms for getting the source image into `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`. The forward path gets it for free: the offscreen render pass declares the colour attachment's `finalLayout` as `TRANSFER_SRC_OPTIMAL`, so `vkCmdEndRenderPass` performs the transition and the copy needs no barrier. `renderLegacy` draws into the same colour image through the same render pass and inherits the same transition — hence four bodies, three mechanisms.

{{cite ohao/gpu/vulkan/framebuffer.cpp "colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;"}}

The RT path delegates: `PathTracer::render` ends with its own barrier on the output image, and the dispatch code records the assumption in a comment instead of re-checking it.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Output is already in TRANSFER_SRC_OPTIMAL from render()"}}

The deferred path is the only one that pays for the transition itself, with the previous layout written as a constant.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;"}}

That constant is a cross-module contract compiled into a barrier. It holds today because the tonemap render pass ends in `SHADER_READ_ONLY_OPTIMAL`. Any post-process step appended after tonemapping that leaves the final image in `GENERAL` turns this into an invalid transition with no compile-time warning.

## The eight-byte image the readback cannot hold

`DeferredRenderer::getFinalOutputImage()` returns the post-processing output only when post-processing actually executed; otherwise it falls back to the lighting pass's own image.

{{cite ohao/render/deferred/deferred_renderer.cpp "        return m_lightingPass->getOutputImage();"}}

Those two images do not have the same format. The tonemapped output is `R8G8B8A8_UNORM`; the lighting pass output is the HDR target.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_hdrOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

Every staging buffer in the ring is sized for four bytes per pixel.

{{cite ohao/gpu/vulkan/device_setup.cpp "size_t stagingBufferSize = m_width * m_height * 4; // RGBA"}}

`vkCmdCopyImageToBuffer` derives its byte count from the source format and extent, not from the destination buffer, so on the fallback branch it would read eight bytes per pixel out of the image and write them into an allocation sized for four — an overrun of the destination, not a benign truncation. The copy is invalid on the source side as well: the lighting pass creates its HDR image with colour-attachment and sampled usage only, and `vkCmdCopyImageToBuffer` requires `TRANSFER_SRC_BIT` on the image it reads.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;"}}

The place a four-byte assumption degrades quietly rather than corrupting is the CPU side, where each pipelined body memcpys exactly `m_width * m_height * 4` bytes out of the mapped staging buffer no matter what was copied in.

Nothing in this file guards the fallback. The tonemap step that produces the four-byte image is skipped when `m_tonemappingEnabled` is false, when the HDR input view is null, and when any of `m_tonemapPipeline`, `m_tonemapFramebuffer` or `m_tonemapDescSet` is null; `getFinalOutputImage` additionally falls back when `m_postProcessing` itself is null. Every one of those reaches the fallback with tonemapping still nominally on, and the one flag that reaches it deliberately is not private either — it is bound into the `ohao_renderer` Python module as `set_tonemapping_enabled`.

{{cite tests/python/renderer_bindings.cpp "&PostProcessingPipeline::setTonemappingEnabled"}}

## Three cameras for one scene

The dispatcher is also where camera state becomes GPU state, and it does that three different ways. The forward path never consults the camera's projection at all: it rebuilds one from a hard-coded 45° vertical FOV and the framebuffer aspect, then flips Y for Vulkan clip space.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "void VulkanRenderer::updateUniformBuffer(uint32_t frameIndex) {"}}

The deferred path hands `DeferredRenderer` the camera's own matrix, which that class flips internally.

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

Here $\theta_v$ is the vertical FOV — 45° hard-coded on the forward path, `Camera::getFov()` on the other two — and $a$ is the aspect the pixel is finally shaded with, not necessarily the one written at the call site. Deferred inherits $a$ from the matrix `Camera` built, and `VulkanRenderer` constructs its `Camera` with all default arguments; the default aspect is 16:10, and neither `setAspectRatio` nor `setPerspectiveProjection` is called anywhere in `ohao/`, `examples/` or `tests/`.

{{cite ohao/render/camera/camera.hpp "float aspect = 16.0f / 10.0f,"}}

The RT raygen takes $a$ from the framebuffer instead. Every example renders 16:9 — 1920×1080 in `cornell_box`, `model_viewer` and `env_demo`, 1280×720 in `turntable` and `interactive` — so RT's horizontal frustum comes out $(16/9)/(16/10) \approx 1.11\times$ wider than deferred's: the same scene, framed 11% differently, decided by nothing but which mode was selected. Correcting the `glm::perspective` argument would not close the gap. Either the raygen stops deriving its own aspect, or `Camera` is told the real one.

## The far plane the cascades believe in

The `setCameraData` call cited above passes two scalars beside the matrix — `0.1f, 1000.0f` — written as literals rather than read from the camera that produced the matrix. `Camera`'s constructor defaults the far plane to 100, and since `setPerspectiveProjection` has no callers, that default is what every deferred frame rasterises with.

{{cite ohao/render/camera/camera.hpp "float farPlane = 100.0f"}}

Those scalars are not decoration. `DeferredRenderer` forwards them to `CSMPass::setCameraData`, which recomputes the cascade splits from them on the spot, blending a logarithmic and a linear subdivision of the near-to-far interval.

{{cite ohao/render/deferred/csm_pass.cpp "float ratio = m_farPlane / m_nearPlane;"}}

The four cascades are therefore distributed over a range ten times deeper than the projection can draw. The deepest split is the far plane itself, so the last cascade's entire extent — from wherever cascade three ends out to 1000 — lies past the point at which geometry is already clipped, and the shadow-map resolution budgeted for it is spent on nothing. This is a sharper defect than the aspect mismatch above, and it has the same cause: a scalar written at the call site instead of read from the object that owns it.

## The light that goes nowhere

The RT dispatch closes its `render` call with four arguments that read like the scene's key light — a position four units up, intensity 30, warm white, radius 1.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "glm::vec3(0.0f, 4.0f, 0.0f), 30.0f,"}}

`PathTracer::render` accepts all four and uses none of them — they appear in the signature and nowhere in the body; only `view` and `proj` survive, as `invView` / `invProj` in the 256-byte push-constant block. The lights the tracer samples arrive instead as a GPU SSBO, whose handle is set by `prepareRTSceneForFrame` earlier in `renderRTPipeline`, before the command buffer is even opened. Deleting the four parameters would change no pixel — which is why they are worth naming: they invite someone to brighten the scene by editing a number that has no consumer.

## The sun that overwrites your only light

`renderDeferred` does something no other body does: it edits the light buffer after uploading it. When the buffer reports exactly one light, it replaces `lights[0].direction` with the sky pass's sun direction, blends that toward a cool moon direction at 8% intensity as `nightFactor` rises, recomputes the shadow matrix, and lerps ambient intensity from 0.12 down to 0.04.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (ld && ld->numLights == 1) {"}}

The intent is stated where the buffer is filled: a scene with no lights gets one fabricated directional light, and the comment says its direction is synced by `renderDeferred`.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "// If no lights in scene, add a default directional light (direction synced by renderDeferred)"}}

But `numLights == 1` cannot tell a fabricated light from an authored one. A scene with a single directional light — the ordinary outdoor case — has its authored direction silently overwritten by the sky's sun on every deferred frame. The fix is a flag on the fallback light, not a count; the count is genuinely ambiguous.

:::why
`renderLegacy` is the rejected design, preserved in source. It submits, blocks on `vkWaitForFences`, then maps, copies and unmaps the staging memory in the same call — so its pixels are correct on the very first call, with no ring and no lag. What it costs is the entire point of the other three bodies: a full CPU-GPU round trip per frame with the CPU idle for the whole GPU pass. The pipelined path trades correct-on-first-call for throughput and accepts a readback that lags by the ring depth. It is worth reading and not worth reaching: `render()` only falls back to it inside the forward branch, after the RT and deferred branches have both declined, and every example plus the smoke test selects Deferred, RTRealtime or RTOffline before rendering anything. In the deferred and RT branches a `FrameResources` that failed to initialise is never caught at all — those bodies run against uninitialised slots rather than degrading to the legacy path.
:::

:::key
Nothing in this file checks that the four modes agree with each other. Dispatch is the last point where camera, layout and light state are translated into GPU state, and it translates them differently per mode: three projection paths, three owners of the same layout transition, near/far literals that contradict the matrix they travel with, and one mode that rewrites the light buffer behind the scene's back. Differences between modes that look like renderer bugs are usually decided here, not in a shader.
:::

## Contracts

- The image returned by `getFinalOutputImage()` must be four bytes per pixel and must already be in `SHADER_READ_ONLY_OPTIMAL`. The layout half holds on both branches — the lighting pass's own render pass also ends in `SHADER_READ_ONLY_OPTIMAL` — but the format half holds only when post-processing's tonemap step ran, and nothing checks it.
- `resize()` must call `resizeStagingBuffers` and reset `m_currentFrame` to 0 before the next `render()`. `resizeStagingBuffers` destroys, re-creates and re-maps all three slot buffers at the new size; skip it and the per-frame `memcpy` reads `width * height * 4` bytes from an allocation still sized for the old dimensions.
- The ring permits up to three overlapping submissions, but `PathTracer` owns a single descriptor set that it rewrites at the top of every `render()`, and only binding 12 (bindless textures) is declared `UPDATE_AFTER_BIND`. Most of those writes are idempotent — the same views and buffers every frame — but bindings 13-16 are not: the surface-history and shading-history image pairs are ping-ponged, with `m_surfaceHistoryWriteIndex` and `m_shadingHistoryWriteIndex` flipped at the end of each `render()`, so four descriptors on a non-`UPDATE_AFTER_BIND` set change every frame while two earlier submissions may still be executing against them. This hazard is already shipped, not hypothetical.
- Submissions carry no semaphores; the per-slot fence is the only synchronisation object in the frame path. Introducing a second queue (async compute, a transfer queue for readback) requires adding one, because nothing here would order the work.
