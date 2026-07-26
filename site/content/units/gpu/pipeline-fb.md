---
module: gpu
id: pipeline-fb
title: Legacy pipeline & framebuffer
standard: v2
---

## A renderer that has never owned a window

Grep the tree for `VkSwapchainKHR` and you get nothing back. That absence is decided in
`framebuffer.cpp` — `pipeline.cpp`, this unit's other file, names `m_renderPass` exactly
once, to build a graphics pipeline against it, and has no opinion about where the frame
goes. `createRenderPass` ends its color attachment in `TRANSFER_SRC_OPTIMAL`, not
`PRESENT_SRC_KHR`:

{{cite ohao/gpu/vulkan/framebuffer.cpp "colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;"}}

and `createOffscreenFramebuffer` finishes by allocating a host-visible, host-coherent
staging buffer of exactly width × height × 4 bytes — inside the same function that
created the images, because the copy target is part of the framebuffer's contract
rather than an afterthought:

{{cite ohao/gpu/vulkan/framebuffer.cpp "bufferInfo.size = m_width * m_height * 4;"}}

Every render mode the engine has — forward, deferred, and both path-tracer profiles —
terminates the same way, each from its own final image: `vkCmdCopyImageToBuffer` into a
staging buffer, then a memcpy into a CPU pixel vector. The GLFW viewer uploads that
vector to an OpenGL texture. Batch PNG output, the smoke test, and the interactive
window are one shape with three consumers.

:::why
Presenting through a `VkSurfaceKHR` swapchain would be cheaper per frame — no
full-resolution image copy, no host-visible allocation. It would also have made every
headless use (`cornell_box`, `model_viewer`, `env_demo`, `turntable`, the smoke test)
the special case, each needing its own readback. OHAO pays the copy in all four modes so
that "the frame" is always a CPU buffer. The bill is visible in the format: the color
attachment is `R8G8B8A8_UNORM`, so the forward path has no HDR intermediate and
`forward.frag` must ACES-tonemap and sRGB-encode before it writes.
:::

## Two render passes, and only one declares its hazards

The shadow render pass installs an explicit dependency in each direction across
`VK_SUBPASS_EXTERNAL`: fragment-shader read before the depth write on entry, and
late-fragment write before the next fragment-shader read on exit.

{{cite ohao/gpu/vulkan/framebuffer.cpp "dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;"}}

That loop is load-bearing, not boilerplate. The single 2048×2048 `D32_SFLOAT` image is
written by the shadow pass and then sampled at set 0, binding 2 by `forward.frag`
during the main pass of the *same* command buffer, so both hazards are real within one
submit. `createRenderPass` by contrast declares no dependencies at all —
`dependencyCount` is left at its value-initialized zero — and hands the color image to
the following `vkCmdCopyImageToBuffer` on the implicit external dependency plus the
`finalLayout` transition alone.

The depth attachment is more restricted than a glance suggests: its store op is
`DONT_CARE`, and the image is created with depth-attachment usage and nothing else.

{{cite ohao/gpu/vulkan/framebuffer.cpp "imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;"}}

Forward depth is therefore write-only, alive just long enough for the in-pass depth
test. Any depth-consuming effect on this path — SSAO, fog, soft particles — needs the
store op *and* the usage flags changed and the image recreated; binding it is not
enough. The shadow sampler makes the opposite kind of choice: `compareEnable` stays
false because the comparison is done by hand in the PCF loop, and all three address
modes are `CLAMP_TO_BORDER` with an opaque-white border:

{{cite ohao/gpu/vulkan/framebuffer.cpp "samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;"}}

The comment there reads "Outside shadow = lit", but that is not the path a fragment
outside the light frustum takes. `calculateShadowForLightIndex` bounds-checks the
projected coordinate on all three axes and returns 0.0 — fully lit — before it issues a
single fetch:

{{cite shaders/includes/shadow/shadow_pcf.glsl "// Bounds check - outside light frustum = no shadow"}}

The border texel is therefore only ever reached by the 5×5 PCF taps that step off the
edge from an in-range centre. A tap that lands entirely outside returns 1.0, which fails
`currentDepth - bias > shadowDepth` for every depth the map can hold, so it adds nothing
to the shadow sum. The white border is not what keeps out-of-frustum fragments lit — the
early return is. What it does is bias the outermost two texels of the map toward lit, so
a shadow running into the edge fades over the kernel width instead of ending in a line.

## One push-constant range, and nothing that pushes through it

`createPipeline` declares a single push-constant range visible to both the vertex and
fragment stages, sized straight from the host struct:

{{cite ohao/gpu/vulkan/pipeline.cpp "// Push constant range for per-object transforms and material"}}

`ObjectPushConstants` is three `mat4` followed by three `vec4` — 240 bytes:

{{cite ohao/gpu/vulkan/renderer.hpp "struct ObjectPushConstants {"}}

That already exceeds the Vulkan-guaranteed minimum `maxPushConstantsSize` of 128
bytes. The engine knowingly targets 256-byte-limit hardware and says so where the same
six fields are redeclared for the GBuffer:

{{cite ohao/render/deferred/gbuffer_pass.hpp "240 bytes — fits in 256-byte push constant limit"}}

`core/gbuffer.vert` mirrors that layout field for field. The forward shaders this
pipeline actually loads do not: after the same leading `mat4 model` they declare 32
bytes of material fields and stop, for 96 bytes total. Everything from byte 64 on —
where the host struct puts `viewProj` — is therefore read as something other than what
was written. That drift, its byte map, and what `forward.frag` does with the result are
dissected in *Layout contracts*; what belongs on this page is who pays for it, and when.

Nobody, so far. `renderSceneObjects` is the only caller that pushes those 240 bytes
through `m_pipelineLayout`, and both of its call sites sit behind the fall-through
branch of `render()` — `renderMultiFrame` if frame resources came up, `renderLegacy` if
they did not. `Forward` is the renderer's default field initialiser, and no
`setRenderMode` call in the tree passes it. Four of the five bundled examples resolve
their mode through a helper that returns `Deferred`, `RTRealtime`, or `RTOffline` and
never `Forward`:

{{cite examples/example_cli.hpp "if (opts.useDeferred) return RenderMode::Deferred;"}}

`env_demo` is the exception: it declares its own mode variable initialised to
`RTOffline` and parses `rt_realtime` / `rt_offline` by hand, so it never reaches the
helper — and cannot select `Deferred` either.

{{cite examples/env_demo.cpp "RenderMode rtMode = RenderMode::RTOffline;"}}

The smoke test sets `RTOffline` outright. Nothing shipped leaves `m_renderMode` at its
default, so the forward draw path is reachable only as a degraded fallback — an RT or
deferred mode selected whose renderer failed to initialise. It is still *built*
unconditionally on every `initialize()`, before any mode is chosen, together with the
full-resolution offscreen color and depth pair and the 2048² shadow map. A path-traced
render pays for all of it.

:::key
A push-constant range is a byte count, not a schema. Vulkan validates its size and its
stage flags; nothing checks that the shader's block and the host struct describe the
same bytes. The reason that has never produced a bad frame here is not a check — it is
that no shipped configuration can select the pipeline holding the mismatch.
:::

## A descriptor pool with exactly zero slack

`createDescriptorPool` sizes itself as frames-in-flight plus one:

{{cite ohao/gpu/vulkan/pipeline.cpp "const uint32_t totalSets = framesInFlight + 1; // +1 for legacy compatibility"}}

and the arithmetic is exact rather than padded: four sets, two uniform buffers and one
combined sampler apiece. `createDescriptorSets` claims one of them — the legacy
single-frame set used by `renderLegacy` — and `FrameResourceManager` is handed this
same layout and this same pool and claims the other three. The pool carries no
`FREE_DESCRIPTOR_SET_BIT`, so nothing can be returned; a fifth allocation fails.

The layout itself is three bindings — camera UBO, light UBO, shadow sampler — and the
forward pipeline layout appends the bindless texture set as set 1, but only if the
texture manager came up:

{{cite ohao/gpu/vulkan/pipeline.cpp "layouts.push_back(m_textureManager->getDescriptorSetLayout());"}}

That conditional is why the texture manager is constructed before `createPipeline` in
`initialize()`, and why shadow resources are created before descriptor sets: the
descriptor writes capture the shadow image view and sampler by value. Resize gets away
with never rewriting a descriptor only because `SHADOW_MAP_SIZE` is a compile-time
constant, so the one image those four sets point at is never recreated.

## The dead file you cannot delete

`createPipeline` loads `core_forward.vert.spv` — the flattened name the shader CMake
produces by replacing `/` with `_` in `shaders/core/forward.vert`:

{{cite ohao/gpu/vulkan/pipeline.cpp "core_forward.vert.spv"}}

That same filename is the probe the renderer's constructor uses to work out where the
compiled shaders live:

{{cite ohao/gpu/vulkan/renderer.cpp "core_forward.vert.spv"}}

and the prefix it finds is pushed unconditionally into the static base path that every
`RenderPassBase` subclass and the particle system resolve against:

{{cite ohao/gpu/vulkan/renderer.cpp "RenderPassBase::setShaderBasePath(m_shaderBasePath);"}}
{{cite ohao/render/deferred/render_pass_base.cpp "s_shaderBasePath + pathStr,"}}

`RenderPassBase` at least falls back to a hard-coded `build/shaders/`, so its passes
degrade to being working-directory dependent. `ParticleSystem::loadShaderModule` has no
fallback at all — it opens `getShaderBasePath() + name` and gives up. Deleting the
forward shaders because nothing renders with them would break particle rendering and
make deferred shader loading cwd-sensitive, in a tree where forward rendering is
otherwise unreachable.

## Contracts

- `createShadowRenderPass` and `createShadowResources` must run before
  `createDescriptorSets` and before `initializeFrameResources`. Binding 2's write copies
  `m_shadowImageView` and `m_shadowSampler` by value into all four sets; recreating
  either handle without rewriting every set leaves dangling descriptors.
- The bindless texture manager must exist before `createPipeline`, or the forward
  pipeline layout is built with one set and any later bind at set 1 is invalid.
- `cleanupFramebuffer` does **not** free the readback staging buffer that
  `createOffscreenFramebuffer` allocates. Both current callers destroy it by hand, in
  opposite orders — `shutdown` before `cleanupFramebuffer`, `resize` after. The order is
  immaterial because `cleanupFramebuffer` touches only the framebuffer, the two images,
  their views and their memory. What it will not do is free the buffer for you: a third
  caller that forgets leaks one width × height × 4 host-visible allocation per call.
- `ObjectPushConstants` is shared byte-for-byte, not field-for-field. A change to it
  must be mirrored in `core/gbuffer.vert`, which declares the same six fields, and in
  `core/forward.vert`, `core/forward.frag`, and `shadow/shadow_depth.vert`, which each
  declare a 96-byte block instead. The three 96-byte blocks are not one declaration:
  `forward.vert` and `shadow_depth.vert` end in `vec2 padding`, `forward.frag` ends in
  `albedoTexIdx` and `normalTexIdx` over the same bytes. `shadow_depth.vert` reads only
  `model`, so it survives layout drift silently.
- The 240-byte range assumes `maxPushConstantsSize >= 256`. Nothing in device selection
  queries that limit; on an implementation reporting the 128-byte Vulkan minimum, both
  pipeline layouts created here are invalid usage.
