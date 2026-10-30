---
module: shaders
id: postprocess-chain
title: Postprocess shader chain
standard: v2
---

## Nine files, one calling convention and two exceptions

What makes `shaders/postprocess/` a family is not that the passes run near each
other — it is that they share an interface. Across its seven GLSL files there is
not a single uniform block and not a single storage buffer. Every parameter arrives
in a `push_constant` block, every input is a combined image sampler, and every
output is the one colour attachment of a fullscreen draw. Each shader is a pure
function of its bindings.

The other three, `ssr.slang`, `ssr_resolve.slang` and `sss.slang`, are Slang compute
modules and keep only half of that. Their parameters are still push constants, but
the blocks are `SSRPush`, `SSRResolvePush` and `SSSPush` from
`shaders/shared/layout.json`, generated into Slang and C++ so the two sides cannot
drift; their images are separate textures read with `Load` (SSR's trace adds one
`SamplerState` for its lit-scene lookup); they write storage images, two of them the
lit image in place; and SSS reads a storage buffer, the G-buffer pass's tile lists.

{{cite shaders/postprocess/sss.slang "[[vk::binding(3, 0)]] StructuredBuffer<uint> tiles;   // the G-buffer pass's classification"}}

Six of the ten are fragment shaders and a seventh is the vertex stage they all
share. `fullscreen.vert` takes no vertex input and no descriptors: it indexes two
three-element constant arrays by `gl_VertexIndex` and emits a single oversized
triangle at (-1,-1), (3,-1), (-1,3).

{{cite shaders/postprocess/fullscreen.vert "vec2( 3.0, -1.0),"}}

The paired texture coordinates are $(0,0)$, $(2,0)$, $(0,2)$ — exactly $(p+1)/2$ at the same three
vertices, the same affine map a quad would interpolate, so the region inside the
viewport still receives UV $[0,1]^2$ and the overhang is clipped. Nothing in the chain ever flips the
convention that falls out: vertex 0 sits at clip $(-1,-1)$, which in Vulkan's Y-down clip space is the *top* left of the
framebuffer, and it carries UV $(0,0)$, the top-left texel of a sampled image.
No pass in the chain sets a negative-height viewport, so nothing re-flips it
downstream. The convention also survives producers that never run this vertex
shader, because it is Vulkan's and not the shader's: framebuffer rows and image
texel indices are counted from the same corner. The G-buffer these passes sample
is rasterised from scene geometry by `gbuffer.vert`, and the SSR and SSS outputs
have no vertex stage at all; all of them still put row 0 where this triangle puts it.

{{cite ohao/render/deferred/gbuffer_pass.cpp "core_gbuffer.vert.spv"}}

The list of pipelines built on it is longer than "post" suggests: bloom
threshold, bloom downsample, bloom upsample, TAA resolve, tonemapping, sky — and
deferred lighting, which is not a post pass at all but is also a fullscreen
triangle over the G-buffer.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "postprocess_fullscreen.vert.spv"}}

## The push-constant ceiling

Push constants are the whole parameter interface, so the family's headroom is
whatever the device allows — and Vulkan only guarantees 128 bytes.
`sky.frag` is authored precisely to that budget: its block is annotated with
hand-computed byte offsets and lands its last field, `starSeed`, at offset 124.

{{cite shaders/postprocess/sky.frag "// Push constants (128 bytes)"}}

The bloom and TAA blocks are 12 to 20 bytes as their GLSL declares them, 16 to 32
as the C++ structs actually push them — `TAAParams` ends in a `float padding[3]`
that no field of the shader's block corresponds to.

{{cite ohao/render/deferred/taa_pass.hpp "float padding[3];"}}

SSR's block is one `float4x4` and three 16-byte vectors, 112 bytes, inside the
floor; it used to carry a second, never-read inverse matrix that took it to 160.
SSS's is 48. The generated C++ structs assert both sizes:

{{cite ohao/gpu/shared_layout.hpp "static_assert(sizeof(SSRPush) == 112);"}}

Nothing checks the budget against the device. The pipeline layout is created
straight from `sizeof` of the struct with no query of `maxPushConstantsSize`, and
the path tracer's 256-byte block simply assumes a device that allows it. Outside this directory the same helper builds SSAO, whose
block is held to the 128-byte guarantee at compile time: its 16 noise rotations travel
as 4-bit strata indices, two uints.

{{cite ohao/render/deferred/ssao_pass.cpp "static_assert(sizeof(shared::SSAOPush) <= 128);"}}

{{cite ohao/render/deferred/ssr_pass.cpp "sizeof(gpu::shared::SSRPush), m_pipelineLayout, m_pipeline) ||"}}
{{cite ohao/render/rt/path_tracer.hpp "};  // total = 256 bytes"}}

:::why
Pushing everything, rather than giving each pass a small uniform buffer, is what
keeps these shaders stateless and the passes free of per-frame buffer ring
management — a UBO would need its own descriptor, its own update path and its own
in-flight-frame discipline for what is often four floats. The price is a ceiling
that is invisible until it is exceeded: `sky.frag` cannot gain a single `vec4`
without breaking on a 128-byte device.
:::

## Three of the ten are compute, and they pay for their own barriers

No compute shader covers the frame the way a fullscreen triangle does. SSR's trace
runs 8×8 workgroups over the half-resolution image, rounded up, and its resolve over
the full frame, both opening with an explicit bounds guard — the fragment passes get
that for free from the viewport. SSS runs one
16×16 workgroup per flagged tile, dispatched indirectly, and a tile on the frame's
right or bottom edge overhangs it, so it guards too:

{{cite shaders/postprocess/sss.slang "return all(p < int2(pc.extent.xy));"}}

The deeper difference is layout. A fragment pass declares its attachment's initial
and final layouts in the render pass and never writes a barrier; the compute passes
write theirs by hand. SSR transitions its output from `UNDEFINED` (the `discard`
argument of `rtc::transition`) to `GENERAL` before the dispatch and to
`SHADER_READ_ONLY_OPTIMAL` after, so last frame's contents are gone and it must store
every in-range pixel. It does: zero for sky, a rough dielectric and a miss, and the
traced result for a hit.

{{cite ohao/render/deferred/ssr_pass.cpp "rtc::transition(cmd, m_output, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT,"}}

The resolve is the in-place case, like SSS's vertical pass below: it takes the lit
image to `GENERAL` without discarding and back, and returns early from every pixel no
trace sample reflects anything at, which must keep its value.

SSS keeps a different invariant. Its scratch image is discarded every frame too, and
`blurH` writes it only in the flagged tiles; `blurV` reads it only within the
`kSSSRadiusTiles` rows of a skin tile, which are exactly the tiles `blurH` covered.
The lit image is the opposite case: SSS takes it from `SHADER_READ_ONLY_OPTIMAL` to
`GENERAL` and back *without* discarding, because the vertical pass writes only skin
pixels and every other pixel must survive — so there a "nothing to do here" `return`
is correct, and in `blurH`, whose non-skin pixels are later read, it is not:

{{cite shaders/postprocess/sss.slang "hOut[p] = centre;"}}

## One depth buffer, three conventions

The G-buffer's `D32_SFLOAT` depth is sampled by three shaders in this unit, and
each treats it differently.

`sky.frag` uses it as a stencil: anything the G-buffer wrote is discarded. Only the
cleared value, exactly 1.0, is sky, so a surface at any distance short of the far
plane keeps its pixel.

{{cite shaders/postprocess/sky.frag "if (depth < 1.0) {"}}

`ssr.slang` compares against it numerically. Its ray march is uniform in *world*
space — 64 steps over the push block's march length — and each step is projected to
clip space and tested against the sampled depth with a fixed `thickness` tolerance
of 0.05, both sides raw device depth:

{{cite shaders/postprocess/ssr.slang "const float sceneDepth = depthBuffer.Load(int3(sp, 0));"}}

Post-projection depth is hyperbolic in view distance: for any standard
perspective projection,

$$\frac{\partial z_{\text{ndc}}}{\partial z_v} \;\propto\; \frac{1}{z_v^{2}}
\qquad\Longrightarrow\qquad \Delta z_v \;\propto\; \varepsilon\, z_v^{2}$$

where $z_v$ is view-space distance along the ray, $z_{\text{ndc}}$ the value
stored in the depth buffer, and $\varepsilon$ the pushed `thickness`. The
thickness the test actually accepts therefore grows with the square of distance:
a tight test a metre from the camera, a very permissive one across the room.
Linearising both sides would make `thickness` a world-space constant instead.

SSR's resolve, which needs a distance to tell surfaces apart when it upsamples the
trace, does not read the depth buffer at all: it measures each surface's distance from
the camera from the G-buffer's world position, which needs no projection terms:

{{cite shaders/postprocess/ssr_resolve.slang "return all(position == float4(0.0)) ? 0.0 : distance(position.xyz, pc.cameraPos.xyz);"}}

`taa_resolve.frag` is the third case: it declares a depth sampler at binding 3
and never executes a single instruction that reads it.

{{cite shaders/postprocess/taa_resolve.frag "layout(set = 0, binding = 3) uniform sampler2D depthBuffer;"}}

The descriptor is still written whenever the sets are rebuilt, with a fallback
view for when no depth is bound.

{{cite ohao/render/deferred/taa_pass.cpp "// Binding 3: Depth (fallback to current frame)"}}

## Where a shader and its own header disagree

The GLSL compute shaders this directory used to hold carried three wrong comments:
SSR claimed the lighting pass composited its result, SSS called its albedo skin test
a copy of one in the lighting shader that never existed, and SSR announced a depth
linearisation it did not perform. The Slang rewrites say where their output goes —
SSR's header names its resolve, SSS's the G-buffer pass that flags its tiles — and
skin is no longer guessed from albedo at all but read from the material's
`subsurface` strength:

{{cite shaders/postprocess/ssr.slang "// at half resolution; postprocess/ssr_resolve.slang upsamples it and puts it in the lit image in"}}

One comment in the chain still describes its own code wrongly. `fullscreen.vert`
labels vertex 0 "bottom left" — the OpenGL reading of clip $(-1,-1)$. Under Vulkan's
Y-down clip space that vertex is the top left, which is the whole reason the chain
never needs a flip.

{{cite shaders/postprocess/fullscreen.vert "// Vertex 0: (-1, -1) - bottom left"}}

:::key
None of these ten shaders can be debugged from its source alone. Each computes
exactly what its bindings and push block say; the rest of the truth — which
image is bound, in what order the passes ran, whether a fallback view is neutral
— is only in the C++. Read the pass that owns a shader before believing its
header comment.
:::

## The constant that ties the moon to the bloom filter

One cross-shader dependency inside this unit is documented. `sky.frag`'s moon
disc is scaled by 4.0, and the comment explains that the number was chosen
against the anti-firefly weighting in `bloom_downsample.frag` — a small, very
bright disc is exactly the input the Karis average exists to suppress.

{{cite shaders/postprocess/sky.frag "// Disc brightness 4.0 is safe with Karis average in bloom downsample."}}

The coupling is indirect: that weighting is enabled by a push-constant flag the
bloom pass raises only for the first downsample step, so the moon's safety margin
depends on a decision made in C++ that neither shader can see. The sun disc in
the same file is scaled by 40 with no equivalent note.

## Contracts

- The origin convention is Vulkan's, not `fullscreen.vert`'s: clip $(-1,-1)$ is top-left and carries UV $(0,0)$, and no pass in the chain flips it or uses a negative-height viewport. An image authored bottom-up needs a flip in its own fragment shader — adding one to the vertex shader flips all seven pipelines built on it at once.
- `sky.frag`'s push block is exactly 128 bytes, the Vulkan floor, and has no room left; SSR's is 112. No pass queries `maxPushConstantsSize`.
- SSR must store every in-range half-resolution pixel: its output is transitioned from `UNDEFINED` each frame, and an early `return` that skips the store leaves garbage, not the previous frame. SSS's `blurH` must store every pixel of the tiles it covers, because `blurV` reads them; `blurV` must *not* store non-skin pixels, because it writes the lit image in place.
- `ssr.slang` compares raw post-projection depth against a constant `thickness`. Changing the near or far plane changes the effective world-space tolerance everywhere in the frame, quadratically with distance.
- SSR's resolve writes the lit image in place and must leave every pixel it has no reflection for untouched; it upsamples by distance from the camera taken from the G-buffer position, so the position's sky value (all zero) must stay distinct from any surface.
- `taa_resolve.frag`'s binding 3 is declared and unread. Removing it from the shader requires dropping write 3 in `TAAPass::updateDescriptorSets` — and the binding-3 entry in `TAAPass::createDescriptors`'s set layout — in the same change.
