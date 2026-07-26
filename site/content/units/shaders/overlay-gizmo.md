---
module: shaders
id: overlay-gizmo
title: Overlay gizmo shaders
standard: v2
---

## An overlay is defined by where it runs

Between them the two stages are twenty-six lines of GLSL with no lighting, no
texture fetch and no descriptor set. Everything that makes them interesting is
the contract they sit inside. `GizmoPass` runs as step 6 of the deferred frame,
after post-processing has already tonemapped HDR down to an LDR image:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_gizmoPass && m_gizmoEnabled) {"}}

That position is what makes the shader trivial. No tonemap curve, no bloom and
no TAA history run after it, and the handles never appear in the motion-vector
buffer — nothing downstream reshapes what the fragment stage writes. The one
thing still standing between that output and the image is the attachment's own
transfer function, and that is exactly where this pass is broken; see below. The
pass reuses the post-processing pipeline's own output image as its colour
attachment and loads rather than clears it, so it composites onto the finished
frame:

{{cite ohao/render/deferred/gizmo_pass.cpp "colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;"}}

There is also no depth attachment anywhere in the render pass — one colour
attachment, and depth test and write both off:

{{cite ohao/render/deferred/gizmo_pass.cpp "depthStencil.depthTestEnable = VK_FALSE;"}}

Transform handles therefore draw over scene geometry unconditionally. That is a
decision, not an oversight: a handle hidden behind the object it moves is a
handle the user cannot grab.

## What the vertex stage is allowed to know

The pipeline layout create-info is value-initialised and only its push-constant
fields are ever assigned, so `setLayoutCount` stays zero — the pass binds no
descriptor sets at all. Everything the shader sees arrives in one push-constant
range declared for the vertex stage alone:

{{cite ohao/render/deferred/gizmo_pass.cpp "VkPipelineLayoutCreateInfo layoutInfo{};"}}

Per vertex there are exactly two attributes, both `R32G32B32_SFLOAT`: an
object-space position and an RGB colour baked in by the mesh generator — red for
X, green for Y, blue for Z.

{{cite ohao/render/deferred/gizmo_pass.cpp "attrDescs[1].offset = offsetof(GizmoVertex, color);"}}

The geometry work is one concatenation of two matrices the shader is handed
separately, and the fragment stage is a pass-through that pins alpha to 1:

{{cite shaders/overlay/gizmo.vert "gl_Position = params.viewProj * params.model"}}
{{cite shaders/overlay/gizmo.frag "outColor = vec4(fragColor, 1.0);"}}

Alpha blending is enabled on the attachment with `SRC_ALPHA` /
`ONE_MINUS_SRC_ALPHA`, so a constant alpha of 1 collapses the blend equation to a
plain replace. The blend state is live but inert; translucent handles are one
line of fragment change away and are not what ships.

## 144 bytes, and the limit it sits above

Keeping `viewProj` and `model` separate costs two `mat4`; with the highlight
`vec4` the mirrored C++ struct is 64 + 64 + 16 = 144 bytes, and that whole size
is what the pipeline layout requests:

{{cite ohao/render/deferred/gizmo_pass.cpp "pushConstant.size = sizeof(GizmoPushConstants);"}}

Vulkan's required-limits table only guarantees `maxPushConstantsSize` of 128
bytes. 144 is above that floor, so this pipeline layout is only creatable on
implementations that expose more than the minimum — it is not a portable block.

:::why
A premultiplied MVP would be 64 bytes and fit any conformant device, at the cost
of one matrix multiply per frame on the CPU instead of one per vertex on the GPU
— for a gizmo whose largest mode is 192 line vertices (three 32-segment circles)
that trade is free. What the split buys is two independent setters:
`setViewProjection` is fed from the frame loop, `setGizmoTransform` from nowhere
in the tree at all, so `model` is still the identity it was constructed with on
every draw this pass could record.
Nothing forms the product; nothing prevents forming it either. The convenience is
real; so is the 128-byte cliff it walks off.
:::

What happens on a device at the floor is not a defined error return. Exceeding
`maxPushConstantsSize` is a valid-usage violation of `vkCreatePipelineLayout`,
so nothing requires an implementation to report it — and the renderer's
non-fatal branch only runs if `createPipeline` actually returns false.

{{cite ohao/render/deferred/deferred_renderer.cpp "GizmoPass failed (non-fatal)"}}

The nearest failure that *is* deterministic never reaches that branch.
`createPipeline` opens with a guard for a null shader module, but
`RenderPassBase::loadShaderModule` throws instead of returning one:

{{cite ohao/render/deferred/render_pass_base.cpp "+ pathStr);"}}

so a missing `overlay_gizmo.vert.spv` makes the guard unreachable, unwinds
through `DeferredRenderer::initialize`, which has no handler, and is caught only
by the try block wrapping the whole of `VulkanRenderer::initialize` — the entire
renderer reports failure rather than one overlay pass being dropped:

{{cite ohao/gpu/vulkan/renderer.cpp "VulkanRenderer initialization failed: "}}

## The highlight that cannot fire

The vertex shader's second line lerps the baked axis colour toward a highlight
colour by a factor carried in `highlightColor.w`:

{{cite shaders/overlay/gizmo.vert "fragColor = mix(inColor, params.highlightColor.xyz, params.highlightColor.w);"}}

The CPU writes that `w` as a literal `0.0f` on every draw, so the `mix` is an
identity and the yellow highlight colour is uploaded but never reached:

{{cite ohao/render/deferred/gizmo_pass.cpp "pc.highlightColor = glm::vec4(GizmoMeshes::HIGHLIGHT_COLOR, 0.0f);"}}

The hovered axis is plumbed all the way from `DeferredRenderer` into the pass and
then stops — `m_highlightedAxis` has a setter and a declaration and no reader:

{{cite ohao/render/deferred/gizmo_pass.hpp "GizmoAxis m_highlightedAxis{GizmoAxis::NONE};"}}

The reason it stops there is structural, not an omission of one line. All three
axes are drawn by a single `vkCmdDrawIndexed`, a push constant is per-draw, and
the vertex format carries no axis identifier — only a colour. Lighting one axis
means splitting into three draws with three push-constant updates, or adding a
third attribute, or having the fragment stage compare its interpolated
`fragColor` against the three axis constants. The first changes the draw loop and
the second the vertex layout; the third touches neither, but the push-constant
range is declared for the vertex stage alone and would have to be opened to the
fragment stage:

{{cite ohao/render/deferred/gizmo_pass.cpp "pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;"}}

And every route needs the host to stop writing a literal `0.0f`. None of them is
a shader-only fix.

## A render pass whose format no reachable target has

The colour attachment format is hard-coded sRGB:

{{cite ohao/render/deferred/gizmo_pass.cpp "colorAttachment.format = VK_FORMAT_R8G8B8A8_SRGB;"}}

Neither image that can reach `setTargetImage` has that format. The
post-processing final output is UNORM, deliberately, because the tonemap shader
applies gamma itself:

{{cite ohao/render/deferred/post_processing_pipeline.cpp "imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // LDR output"}}

and the fallback used when post-processing did not execute is the lighting pass's
HDR target, `R16G16B16A16_SFLOAT`:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_hdrOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

Vulkan requires each framebuffer attachment view to be created with a format
*equal* to the corresponding render-pass attachment description — sRGB and UNORM
are format-compatible for view creation but are not equal here. So
`vkCreateFramebuffer` inside `setTargetImage` is invalid usage on both paths.
That is a valid-usage rule, not an error return: validation layers will say so,
a driver is free to hand back `VK_SUCCESS` anyway, and the pass's null-framebuffer
branch only fires when the call itself reports failure. It is not a detector.

The mismatch also settles a question the first section left open. The
post-processing output is UNORM *because* the tonemap shader has already applied
gamma; give this pass an sRGB view instead and the hardware would apply the
linear-to-sRGB transfer function a second time on every write. The axis colours
are constants pushed straight through both stages, so whichever format the two
sides end up agreeing on is what decides whether a 0.9 red is the red that lands
in the image. Fixing this means taking the target format as a parameter and
building the render pass lazily, not editing the constant.

Format is not the only thing the framebuffer takes on faith. Its extent comes
from `m_width`/`m_height`, which `initialize` hard-codes and only `onResize`
corrects — never from the image the framebuffer is being built against:

{{cite ohao/render/deferred/gizmo_pass.cpp "m_width = 1920;"}}

:::key
This has never fired in a shipping run: `m_gizmoEnabled` defaults to false,
nothing in the tree calls `setGizmoEnabled`, and `execute` returns on its first
line without it. The pipeline is created at startup and the draw is never
recorded. Read this unit as a working shader pair behind a host-side path that
has not been exercised since the editor layer was stripped, and check three
things before wiring a UI to it: the attachment format, the 144-byte
push-constant block, and the framebuffer extent.
:::

## Two smaller things worth knowing before you touch it

The rasteriser asks for a 2.0 line width, and that is legal only because the
device is created with the `wideLines` feature enabled; this is the one pipeline
in the tree that requests a width above 1.0.

{{cite ohao/gpu/vulkan/device_setup.cpp "deviceFeatures2.features.wideLines = VK_TRUE;"}}

And the index buffer buys nothing. Every primitive in every gizmo mode is built
through one helper that appends two fresh vertices and then indexes them,
so the index array is always the sequence 0, 1, 2, … with no vertex reuse at all:

{{cite ohao/render/gizmo/gizmo_meshes.cpp "indices.push_back(base + 1);"}}

Geometry is regenerated in `setGizmoMode`, outside any command recording. What
sits inside `execute` is the destroy-and-reallocate of both buffers that follows
it, with no wait for in-flight frames — the only `vkDeviceWaitIdle` in the pass
is in `cleanup`. It has not bitten because it has never run: `setGizmoMode` has
no callers, so the dirty flag `createVertexBuffers` clears during `initialize` is
never raised again, and `execute` returns before reaching it in any case. It is a
use-after-free waiting for the first frame that
is both enabled and mid-mode-switch, and it is the first thing to fix if the
gizmo is ever driven from live input.
