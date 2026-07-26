---
module: deferred
id: gizmo
title: Gizmo pass
standard: v2
---

## A pass that is fully wired and never runs

Start with the fact a hostile reader finds in ten seconds of grep, because
everything else here depends on it. `GizmoPass` is constructed, its pipeline and
render pass are created, its shaders are compiled into the build — and in the
current tree nothing ever turns it on. The enable flag defaults false:

{{cite ohao/render/deferred/deferred_renderer.hpp "bool m_gizmoEnabled{false};"}}

and the only function that can flip it, `DeferredRenderer::setGizmoEnabled`, has
no callers anywhere in `ohao/`, `examples/`, or `tests/`.

{{cite ohao/render/deferred/deferred_renderer.cpp "void DeferredRenderer::setGizmoEnabled(bool enabled) {"}}

That is what "no editor" looks like from the renderer's side: the handle-drawing
machinery exists and waits for a selection UI nobody has written. Everything
below describes a loaded gun, not a fired one — which is exactly why the three
latent defects below have never been caught by running the engine.

## Compositing after the frame is already finished

The gizmo block is the last thing `DeferredRenderer::render` records — it runs
after post-processing and owns no render target. It borrows whichever image the
frame ended on:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_gizmoPass->setTargetImage(finalImage, finalView);"}}

Normally that is post-processing's LDR output. The fallback, taken when
`PostProcessingPipeline::didExecute` reports false, is the deferred lighting
pass's HDR image:

{{cite ohao/render/deferred/deferred_renderer.cpp "finalImage = m_lightingPass->getOutputImage();"}}

Calling that "the lighting output" would be wrong by this point in the frame.
`SkyPass::setHDROutput` is wired to exactly that view and image, so the Preetham
sky is already in it, and `createParticleFramebuffer` builds the forward particle
framebuffer from the same view:

{{cite ohao/render/deferred/deferred_renderer.cpp "VkImageView hdrView = m_lightingPass->getOutputView();"}}

What is *not* in it is the subsurface blur — post-processing prefers
`m_sssPass->getOutputView()` whenever the SSS pass has one, and this image is the
unblurred alternative. The choice is not arbitrary, though: it mirrors
`DeferredRenderer::getFinalOutputImage` exactly, so the handles land on whatever
image the frame is actually going to hand out.

Compositing rather than clearing is the whole trick — the attachment uses
`LOAD_OP_LOAD`, so the beauty frame survives and only drawn pixels change:

{{cite ohao/render/deferred/gizmo_pass.cpp "colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;"}}

Two consequences are easy to miss. On the post-processing path the draw lands
*after* tonemapping, so gizmo colors are literal framebuffer values: the red
`(0.9, 0.2, 0.2)` axis is that red at any scene exposure, which is right for UI
and wrong for anything you might be tempted to reuse this pass for. (On the
fallback path the same constants go into a scene-referred HDR image instead, and
that property is lost.) And although the blend state is configured for standard
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA` blending, the fragment shader hardcodes alpha
to 1.0, so the blend equation degenerates to a plain overwrite — the blending
setup is inert as shipped.

{{cite shaders/overlay/gizmo.frag "outColor = vec4(fragColor, 1.0);"}}

The layout handling around the render pass rewards careful reading. `execute`
barriers the borrowed image from `SHADER_READ_ONLY_OPTIMAL` up to
`COLOR_ATTACHMENT_OPTIMAL`; the render pass declares that same initial layout and
a final layout back to `SHADER_READ_ONLY_OPTIMAL`; and then the *second* barrier
declares old and new layouts that are identical.

{{cite ohao/render/deferred/gizmo_pass.cpp "barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // finalLayout"}}

That is deliberate. The render pass already performed the transition; the
trailing barrier exists only to establish the execution and memory dependency so
a later blit or sampler sees the written pixels. Deleting it as "a no-op layout
change" introduces a real hazard.

## The format the pass promises, and the two it can receive

Here is the first latent defect. The gizmo render pass hardcodes its color
attachment format:

{{cite ohao/render/deferred/gizmo_pass.cpp "colorAttachment.format = VK_FORMAT_R8G8B8A8_SRGB;"}}

Neither candidate target has that format. The post-processing final output is
UNORM, deliberately, because the tonemap shader applies gamma in software instead
of relying on hardware sRGB encode:

{{cite ohao/render/deferred/post_processing_pipeline.cpp "imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // LDR output"}}

and the lighting-pass fallback is a half-float HDR target:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_hdrOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

Framebuffer creation requires each image view's format to match the render pass
attachment it fills, so `setTargetImage` builds an incompatible framebuffer in
both cases. The code does check the result and null the handle on failure, and
`execute` early-outs on a null framebuffer, so on a driver that rejects it the
symptom is a missing gizmo plus one `GizmoPass: Failed to create framebuffer`
line on stderr, not a crash. The pass object itself stays alive and retries every
time the target handles change. Whoever wires up selection must fix the format
first.

## Lines, and the generator that makes them

The geometry is a `LINE_LIST` of 24-byte position+color vertices with no normal,
no UV, and — importantly for the next section — no axis identifier:

{{cite ohao/render/gizmo/gizmo_meshes.hpp "struct GizmoVertex {"}}

Lines rasterise 2 px wide, legal only because the device asked for the optional
`wideLines` feature — unconditionally, with no `vkGetPhysicalDeviceFeatures`
query anywhere in the tree to check it first:

{{cite ohao/gpu/vulkan/device_setup.cpp "deviceFeatures2.features.wideLines = VK_TRUE;"}}

Requesting an unsupported feature makes `vkCreateDevice` fail, so a GPU without
`wideLines` never reaches a degraded gizmo — `createLogicalDevice` returns false
and `VulkanRenderer::initialize` aborts. This pipeline is also the only thing in
the tree that needs the feature: every other `rasterizer.lineWidth` in `ohao/` is
`1.0f`, including the GBuffer wireframe variant whose source comment blames
`wideLines` for a creation failure `lineWidth = 1.0f` cannot cause.

The generator's `addLine` helper appends two brand-new vertices per segment and
never shares them:

{{cite ohao/render/gizmo/gizmo_meshes.cpp "uint32_t base = static_cast<uint32_t>(vertices.size());"}}

so the index buffer is the identity permutation `0,1,2,3,…` and `vkCmdDrawIndexed`
does exactly what `vkCmdDraw` would, at the cost of an extra allocation and bind.
The counts are small enough that it has never mattered: translate is 51 segments
(102 vertices, 2,448 bytes), scale is 39, and rotate — three 32-segment rings —
is the largest at 96 segments and 4,608 bytes.

Cones and circles both need an orthonormal basis perpendicular to a given axis,
and both build it identically. For a unit axis $\mathbf{a}$, base centre
$\mathbf{b}$, radius $r$ and $N$ segments with $\theta_i = 2\pi i / N$:

$$\mathbf{p}_1=\frac{\mathbf{a}\times\mathbf{u}}{\lVert\mathbf{a}\times\mathbf{u}\rVert},\qquad \mathbf{p}_2=\mathbf{a}\times\mathbf{p}_1,\qquad \mathbf{c}_i=\mathbf{b}+r\left(\mathbf{p}_1\cos\theta_i+\mathbf{p}_2\sin\theta_i\right)$$

$\mathbf{u}$ is the reference vector the cross product is taken against; it must
not be parallel to $\mathbf{a}$ or the cross collapses to zero. The code picks
world-up, switching to world-right when the axis comes within about 8° of
vertical — the branch the Y-axis arrow always takes:

{{cite ohao/render/gizmo/gizmo_meshes.cpp "perp1 = glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));"}}

$\mathbf{p}_2$ is not renormalised, and does not need to be: $\mathbf{a}$ and
$\mathbf{p}_1$ are unit and orthogonal, so their cross product already has unit
length.

:::why
The pipeline disables depth test *and* depth write, and its render pass has no
depth attachment at all — the alternative, sampling GBuffer depth so scene
geometry occludes the handles, was not taken.

{{cite ohao/render/deferred/gizmo_pass.cpp "depthStencil.depthTestEnable = VK_FALSE;"}}

A handle you cannot see is a handle you cannot grab; an object embedded in
terrain would become untranslatable. The price is that the rotation gizmo draws
all three rings in full, front and back, with no cue about which half of each
circle is on the near side. Editors that solve this clip each ring to the
camera-facing hemisphere; OHAO does not, and the generator has no view direction
to do it with.
:::

## The highlight that cannot work, and why

`setHighlightedAxis` stores an axis into `m_highlightedAxis`, and nothing reads
that member — the push constant hardcodes the highlight weight to zero:

{{cite ohao/render/deferred/gizmo_pass.cpp "pc.highlightColor = glm::vec4(GizmoMeshes::HIGHLIGHT_COLOR, 0.0f);"}}

so the vertex shader's blend always returns the untouched vertex color:

{{cite shaders/overlay/gizmo.vert "fragColor = mix(inColor, params.highlightColor.xyz, params.highlightColor.w);"}}

Setting `w = 1` would not fix this — it would turn *every* axis yellow at once.
The push constant is uniform across the single indexed draw and `GizmoVertex`
carries no axis id, so the shader cannot tell X segments from Y segments.
Finishing hover highlighting needs one of two structural changes: split the draw
into three index ranges and push a different weight per range, or add a fourth
vertex attribute and compare it in the shader. The stored axis is a placeholder
for a decision nobody has made.

## Two more edges before you enable this

The push constant block is `mat4 + mat4 + vec4` = 144 bytes.

{{cite ohao/render/deferred/gizmo_pass.cpp "pushConstant.size = sizeof(GizmoPushConstants);"}}

Vulkan's guaranteed minimum `maxPushConstantsSize` is 128. Nothing in the tree
queries the actual limit; the only record of it is a comment in the path
tracer, which packs two values into one field rather than exceed 256 bytes:

{{cite ohao/render/rt/path_tracer_render.cpp "// 256-byte device maxPushConstantsSize, so we cannot append a field"}}

So on the development hardware nothing fails, but this layout is not portable to
a minimum-spec device. That failure lands somewhere quite different from the
format mismatch: `vkCreatePipelineLayout` rejects the range at init, so
`createPipeline` returns false, `initialize` prints `GizmoPass: Failed to create
pipeline`, and the caller logs `GizmoPass failed (non-fatal)` and destroys the
whole pass object — one loud message at startup rather than a per-frame
framebuffer retry. The fix costs nothing: no shader here needs `viewProj` and
`model` separately, and premultiplying them on the CPU drops the block to 80
bytes.

Changing gizmo mode marks the geometry dirty, and the rebuild happens inside
`execute`, on the recording path, by destroying and reallocating the buffers:

{{cite ohao/render/deferred/gizmo_pass.cpp "void GizmoPass::updateGizmoBuffers() {"}}

There is no fence wait and no frames-in-flight ring here; `cleanup` calls
`vkDeviceWaitIdle`, this path does not. Switching translate → rotate while
earlier command buffers referencing the old buffers are still in flight is a
use-after-free. It has never fired because mode switching requires an editor.

One piece of resize logic looks redundant and is not. `setTargetImage` memoises
on handle equality and returns early when image and view are unchanged:

{{cite ohao/render/deferred/gizmo_pass.cpp "if (m_targetImage == image && m_targetImageView == imageView) return;"}}

so `onResize` deliberately clears the cached handles, forcing a framebuffer
rebuild next frame even if the driver recycles the same handle values for the
recreated post-processing target.

:::key
A complete, compiled, never-executed overlay stage. Its three latent defects — a
render-pass format matching neither possible target, a 144-byte push-constant
block against Vulkan's 128-byte guaranteed minimum, and a buffer rebuild with no
in-flight synchronisation — are invisible precisely because
`DeferredRenderer::setGizmoEnabled` has no callers. Wiring a selection UI to it
surfaces the format bug on frame one and the sync bug on the first mode switch;
the push-constant block only bites on hardware nobody has run this on.
:::
