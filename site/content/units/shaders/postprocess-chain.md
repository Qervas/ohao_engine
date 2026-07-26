---
module: shaders
id: postprocess-chain
title: Postprocess shader chain
standard: v2
---

## Nine files, one calling convention

What makes `shaders/postprocess/` a family is not that the passes run near each
other — it is that they share an interface. Across all nine files there is not a
single uniform block and not a single storage buffer. Every parameter arrives in
a `push_constant` block, every input is a combined image sampler, and every
output is either the one colour attachment of a fullscreen draw or one
`imageStore` per pixel. Each shader is a pure function of its bindings.

Six of the nine are fragment shaders and a seventh is the vertex stage they all
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
is rasterised from scene geometry by `gbuffer.vert`, and `ssr.comp`'s output has
no vertex stage at all; both still put row 0 where this triangle puts it.

{{cite ohao/render/deferred/gbuffer_pass.cpp "core_gbuffer.vert.spv"}}

The list of pipelines built on it is longer than "post" suggests: bloom
threshold, bloom downsample, bloom upsample, TAA resolve, tonemapping, sky — and
deferred lighting, which is not a post pass at all but is also a fullscreen
triangle over the G-buffer.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "postprocess_fullscreen.vert.spv"}}

## The push-constant ceiling, and the one shader that is over it

Push constants are the whole parameter interface, so the family's headroom is
whatever the device allows — and Vulkan only guarantees 128 bytes.
`sky.frag` is authored precisely to that budget: its block is annotated with
hand-computed byte offsets and lands its last field, `starSeed`, at offset 124.

{{cite shaders/postprocess/sky.frag "// Push constants (128 bytes)"}}

The bloom and TAA blocks are 12 to 20 bytes as their GLSL declares them, 16 to 32
as the C++ structs actually push them — `TAAParams` ends in a `float padding[3]`
that no field of the shader's block corresponds to.

{{cite ohao/render/deferred/taa_pass.hpp "float padding[3];"}}

`ssr.comp` is the outlier: it pushes two full `mat4`s plus a `vec4`, a
`vec2` and two floats: 160 bytes, past the guaranteed floor. Half of that
overflow is dead weight. The second matrix is declared and never read by a single
instruction, while the CPU computes it with a full `glm::inverse` every frame.

{{cite shaders/postprocess/ssr.comp "mat4 invViewProj;"}}
{{cite ohao/render/deferred/ssr_pass.cpp "m_params.invViewProj = glm::inverse(viewProj);"}}

Nothing checks the budget either way. The pipeline layout is created straight
from `sizeof(SSRPushConstants)` with no query of `maxPushConstantsSize`, and the
only place the limit is reasoned about at all is a path-tracer comment that
assumes a 256-byte device.

{{cite ohao/render/deferred/ssr_pass.cpp "pcr.size = sizeof(SSRPushConstants);"}}
{{cite ohao/render/rt/path_tracer_render.cpp "256-byte device maxPushConstantsSize"}}

:::why
Pushing everything, rather than giving each pass a small uniform buffer, is what
keeps these shaders stateless and the passes free of per-frame buffer ring
management — a UBO would need its own descriptor, its own update path and its own
in-flight-frame discipline for what is often four floats. The price is a ceiling
that is invisible until it is exceeded, and nothing prunes what rides under it:
deleting the unused `invViewProj` would take SSR from 160 bytes to 96, back
inside the guarantee, and `sky.frag` cannot gain a single `vec4` without breaking
on a 128-byte device.
:::

## Two of the nine are compute, and they pay for their own barriers

`ssr.comp` and `sss_blur.comp` run 16×16 workgroups over a dispatch rounded up
from the render extent, so both open with an explicit bounds guard — the fragment
passes get that for free from the viewport.

{{cite shaders/postprocess/sss_blur.comp "if (pixel.x >= int(screenSize.x) || pixel.y >= int(screenSize.y)) return;"}}

The deeper difference is layout. A fragment pass declares its attachment's
initial and final layouts in the render pass and never writes a barrier; the two
compute passes write theirs by hand, transitioning the destination from
`UNDEFINED` to `GENERAL` before the dispatch and to `SHADER_READ_ONLY_OPTIMAL`
after — twice for SSS, once per separable direction.

{{cite ohao/render/deferred/sss_pass.cpp "toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

`UNDEFINED` discards the previous contents and both outputs are `writeonly`, so
neither shader may skip a store on an in-range pixel. Both respect that:
`ssr.comp` stores zero on its two early rejects, and `sss_blur.comp` stores the
unmodified centre colour for every non-skin pixel. Adding a "nothing to do here"
`return` to either — the natural optimisation — leaves those pixels undefined.

## One depth buffer, three conventions

The G-buffer's `D32_SFLOAT` depth is sampled by three shaders in this unit, and
each treats it differently.

`sky.frag` uses it as a stencil: anything the G-buffer wrote is discarded. The
threshold is a hair under one, so a surface landing exactly on the far plane is
treated as sky.

{{cite shaders/postprocess/sky.frag "if (depth < 0.9999) {"}}

`ssr.comp` compares against it numerically. Its ray march is uniform in *world*
space — 64 steps of `maxDistance/64` — and each step is projected to clip space
and tested against the sampled depth with a fixed `thickness` tolerance of 0.05.
The comment directly above announces a linearisation that does not happen:

{{cite shaders/postprocess/ssr.comp "float rayDepth = ndc.z;"}}

Post-projection depth is hyperbolic in view distance: for any standard
perspective projection,

$$\frac{\partial z_{\text{ndc}}}{\partial z_v} \;\propto\; \frac{1}{z_v^{2}}
\qquad\Longrightarrow\qquad \Delta z_v \;\propto\; \varepsilon\, z_v^{2}$$

where $z_v$ is view-space distance along the ray, $z_{\text{ndc}}$ the value
stored in the depth buffer, and $\varepsilon$ the pushed `thickness`. The
thickness the test actually accepts therefore grows with the square of distance:
a tight test a metre from the camera, a very permissive one across the room.
Linearising both sides — which the comment claims and the code does not do —
would make `thickness` a world-space constant instead.

`taa_resolve.frag` is the third case: it declares a depth sampler at binding 3
and never executes a single instruction that reads it.

{{cite shaders/postprocess/taa_resolve.frag "layout(set = 0, binding = 3) uniform sampler2D depthBuffer;"}}

The descriptor is still written whenever the sets are rebuilt, with a fallback
view for when no depth is bound.

{{cite ohao/render/deferred/taa_pass.cpp "// Binding 3: Depth (fallback to current frame)"}}

## Where a shader and its own header disagree

Four comments in this unit are now wrong. Two of them are the only in-file record
of how a shader relates to another pass.
`ssr.comp` says its result is composited by the lighting pass; `deferred_lighting.frag`
has no SSR sampler, and the add happens in `tonemapping.frag` instead.

{{cite shaders/postprocess/ssr.comp "// Outputs reflection color to an image that the lighting pass blends in."}}

`sss_blur.comp` is the other: its skin classifier is introduced as a copy of one
in the lighting shader.

{{cite shaders/postprocess/sss_blur.comp "// Detect skin material (same heuristic as lighting shader)"}}

Grepping for the predicates themselves — `smoothstep(0.1, 0.35, albedo.r)`,
`step(albedo.g, albedo.r)` — rather than for the function name finds them in
`sss_blur.comp` and nowhere else. There is no second copy that has drifted; there
is no first copy. Skin is classified in one place, from albedo alone.

The remaining two describe the shader's own code and get it wrong. One is the
linearisation comment above. The other is in `fullscreen.vert`, which labels
vertex 0 "bottom left" — the OpenGL reading of clip $(-1,-1)$. Under Vulkan's
Y-down clip space that vertex is the top left, which is the whole reason the
chain never needs a flip.

{{cite shaders/postprocess/fullscreen.vert "// Vertex 0: (-1, -1) - bottom left"}}

:::key
None of these nine shaders can be debugged from its GLSL alone. Each computes
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
- `ssr.comp`'s push block is 160 bytes; Vulkan guarantees only 128. Its unused `invViewProj` is 64 of those bytes. `sky.frag`'s block is exactly 128 and has no room left. Neither pass queries `maxPushConstantsSize`.
- Both compute shaders must `imageStore` to every in-range pixel. Their outputs are transitioned from `UNDEFINED` and declared `writeonly`; an early `return` that skips the store leaves garbage, not the previous frame.
- `ssr.comp` compares raw post-projection depth against a constant `thickness`. Changing the near or far plane changes the effective world-space tolerance everywhere in the frame, quadratically with distance.
- `taa_resolve.frag`'s binding 3 is declared and unread. Removing it from the shader requires dropping write 3 in `TAAPass::updateDescriptorSets` — and the binding-3 entry in `TAAPass::createDescriptors`'s set layout — in the same change.
