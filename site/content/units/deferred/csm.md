---
module: deferred
id: csm
title: CSM shadows
standard: v2
figures: [deferred-csm-frustum-corner-pairing]
---

## The path that only runs when ray tracing is off

`CSMPass` is the deferred pipeline's fallback sun shadow: a real render-graph pass
that allocates a `D32_SFLOAT` image with one 2048² array layer per cascade, renders
depth from the light into each layer in turn, and hands the array view plus a cascade
UBO to the lighting pass.

{{cite ohao/render/deferred/csm_pass.hpp "static constexpr uint32_t SHADOW_MAP_SIZE = 2048;"}}

The lighting shader only consults it when the RT shadow mask is absent.
`DeferredLightingPass::execute` zeroes the flag word every frame and rebuilds it from
the currently bound views: an RT shadow view sets bit 5, and only its absence lets the
CSM array view set bit 2. `DeferredRenderer` clears the RT mask before recording the
graph and hands it back only from an `RTShadow` pass whose `render()` actually traced,
so a frame with no TLAS instances falls back to this page's map rather than sampling
an unwritten mask.

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_rtShadow->rendered() && m_lightingPass)"}}

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 4;   // CSM fallback (bit 2)"}}

`setRTShadowMask` performs the same bit arithmetic at bind time, but that assignment
never reaches the GPU: `execute` overwrites the whole word before pushing it.

That matters for the rest of this page: several defects below survive precisely
because the default configuration on an RT-capable device never exercises this path.

## Where to cut the frustum

A single directional shadow map spends its texels uniformly in *world* space while
the camera needs them uniformly in *screen* space: the near field is starved, the
far field wasted. Cascades cut the view frustum along depth and give each slice its
own full-resolution map. OHAO places the cuts with the practical split scheme (Zhang
et al. 2006), blending a perspective-correct logarithmic split with a uniform one:

$$d_i \;=\; \lambda\, n\!\left(\frac{f}{n}\right)^{i/N} \;+\; (1-\lambda)\!\left(n + (f-n)\frac{i}{N}\right)$$

Here $n$ and $f$ are the camera near and far planes, $N = 4$ is the cascade count,
$i \in [1, N]$ indexes the split, and $\lambda$ weights logarithmic against uniform.
The logarithmic term is what the projective mapping actually wants; the uniform term
exists because pure log placement collapses the first split onto the near plane
whenever $n$ is small.

{{cite ohao/render/deferred/csm_pass.cpp "float d = m_splitLambda * log + (1.0f - m_splitLambda) * linear;"}}

`setSplitLambda`'s only caller is a smoke test in `renderer_pipeline_tests.cpp` — a file no `CMakeLists.txt` builds — and it re-sets the
value the constructor already holds, so $\lambda$ is 0.95 in every frame the engine
renders.

{{cite tests/renderer/renderer_pipeline_tests.cpp "csm.setSplitLambda(0.95f);"}}

{{cite ohao/render/deferred/csm_pass.hpp "float m_splitLambda{0.95f}; // 0.95 = mostly logarithmic"}}

With the renderer's defaults of $n = 0.1$, $f = 1000$ the log term alone puts the
first split at 1 m. Evaluating the full formula gives splits near 13.5, 34.5, 132.5
and 1000 units — the 5 % uniform share does all the work at the near end, which is
why $\lambda$ is not 1.

## Stabilising the cascade

The classic cascade artifact is *swimming*: rotate the camera one degree and every
shadow edge crawls, because the light's projection was re-fit to a slightly
different region and the depth samples landed on different texels. OHAO kills this
in the standard two steps. It bounds each slice with a sphere rather than a box,
then quantises the light view's translation to whole shadow texels — computing
texels-per-world-unit from the sphere and rounding the light-space origin onto that
grid.

{{cite ohao/render/deferred/csm_pass.cpp "float texelsPerUnit = static_cast<float>(SHADOW_MAP_SIZE) / (radius * 2.0f);"}}

:::why
A tight AABB wastes fewer texels, but its extents change as the camera yaws, so the
projection scale changes every frame and no amount of snapping stabilises it. A
bounding sphere's radius is invariant under rigid rotation of the corner set: the
scale is fixed for a given slice, leaving only translation — which is exactly what
snapping can quantise away. Wasted map area is the deliberate price.
:::

## The pairing that flattens the split

The slice geometry is built by unprojecting the eight NDC corners of the frustum and
walking each near-plane corner toward its far-plane partner by the split fractions.
The corners are enumerated by a nested `x, y, z` loop with `z` innermost, so the
index is $4x + 2y + z$ — near and far partners are *adjacent* (0↔1, 2↔3, …). The
interpolation loop instead pairs `i` with `i + 4`:

{{cite ohao/render/deferred/csm_pass.cpp "glm::vec3 dist = frustumCorners[i + 4] - frustumCorners[i];"}}

Index `i` and `i + 4` differ in the *x* bit, not the depth bit: the direction being
scaled is a horizontal edge of the near (or far) plane, and the frustum's depth
extent never enters the interpolation.

{{figure deferred-csm-frustum-corner-pairing "Conceptual, not a captured render. Left: the corner pairs the split loop interpolates along, which both lie on the same depth plane. Right: the near-to-far pairs a depth slice requires. Corner indices follow the idx = 4x + 2y + z enumeration in calculateLightViewProj."}}

Far-plane corners therefore stay at the far plane for every cascade, so every sphere
has to enclose almost the whole view frustum. Evaluating the shipped formula for a
45° vertical FOV, 16:9 camera at the default near and far gives cascade radii of
roughly 749, 745, 752 and 946 units — the first three within 1 % of each other, the
last only a quarter larger, where the near-to-far pairing would give 13, 31, 122 and
950. Cascade 0's texel footprint is 73 cm rather than 1.3 cm — the split scheme is
computed correctly, then discarded by the geometry it feeds. Numbers from the
formula, not from a capture.

## Three biases stacked against acne

Shadow acne comes from a receiver sampling its own depth across a texel of finite
size. The pass attacks it three ways at once. Front-face culling makes the recorded
depth the *back* of each occluder, moving self-shadowing error to the far side:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // Front-face culling for shadow"}}

A hardware constant-plus-slope bias adds the term that matters at grazing sun
angles, scaling with the polygon's slope in light space:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.depthBiasSlopeFactor = 1.75f;"}}

The lighting shader then subtracts a further constant `shadowBias` (0.012 by default
in the C++ `CascadeData`). Front-face culling is the strong lever and it has a real
price: single-sided geometry — foliage cards, a ground plane with no underside — has
no back face to record and casts no shadow at all, while thin closed objects
peter-pan. The `normalBias` and `cascadeBlendWidth` fields the C++ struct fills are
read by nothing in the shipping shader.

## The depth convention nobody set

`GLM_FORCE_DEPTH_ZERO_TO_ONE` is not defined anywhere in this repository — the only
occurrence is a comment in the picking system noting its absence. GLM therefore
emits OpenGL-convention matrices with NDC depth in $[-1, 1]$, including the light's
orthographic projection:

{{cite ohao/render/deferred/csm_pass.cpp "glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, radius * 2.0f);"}}

Vulkan clips against $0 \le z_c \le w_c$, so the near half of that volume — the half
containing the occluders closest to the light — would be clipped away entirely. It
survives only because depth clamping is enabled, which also disables z-plane
clipping and flattens those fragments onto depth 0:

{{cite ohao/render/deferred/csm_pass.cpp "rasterizer.depthClampEnable = VK_TRUE; // Clamp depth to [0, 1]"}}

On the sampling side the same $[-1, 1]$ range reaches `calculateShadowCSM`, which
rejects any `projCoords.z` below 0 and returns "lit". The frustum-corner
unprojection makes the mirror-image assumption, using NDC $z \in \{0, 1\}$ with a
comment asserting the Vulkan range. Defining the GLM macro would fix the light
projection and break every camera projection that currently agrees with it — a
repo-wide change, not a one-line edit.

## The sampler that ships, and the one that looks official

No shader `#include`s it, so nothing in it is reachable from any pipeline. Its two
entry points, `sampleCSM` and `sampleCSM_PCSS`, have zero callers; the helpers
`selectCascade`, `getShadowCoords` and `sampleShadowPCF` are called only from those
two dead functions. And unlike the classic "unused function, live formula" case the
math is not inlined anywhere either. What runs instead is written directly into the
deferred lighting shader, which defines its own `selectCascade` over the live UBO and
its own 3×3 box PCF over nine taps — no Poisson disk, no normal bias, no cascade
blend:

{{cite shaders/core/deferred_lighting.frag "float calculateShadowCSM(vec3 fragPos, float viewDepth) {"}}

The two also disagree structurally: the include's `CascadeData` ends in a `float
padding` where the C++ struct and the live UBO both declare a `uint cascadeCount`.

## One render pass per cascade

The raster side is the plain multi-pass form of layered rendering. `createShadowMap`
builds, besides the `2D_ARRAY` view the lighting shader samples, one single-layer
`2D` view per cascade, and `createFramebuffers` wraps each in its own one-layer
framebuffer:

{{cite ohao/render/deferred/csm_pass.cpp "framebufferInfo.layers = 1;"}}

`CSMPass::execute` loops over the four cascades, begins the depth-only render pass on
that cascade's framebuffer, and redraws every `RenderScene` draw. Nothing routes
primitives to a layer: the framebuffer already *is* the layer. The pipeline therefore
has a vertex stage and nothing else — no geometry shader, no fragment shader — and
the vertex shader projects straight into the cascade being rendered. The object's
world matrix is not pushed per draw; it is read from `RenderScene`'s instance table,
indexed by the draw's `firstInstance`, which the pass sets to the instance slot:

{{cite shaders/shadow/shadow_csm.vert "gl_Position = pc.viewProj * instances[gl_InstanceIndex].world * vec4(inPosition, 1.0);"}}

The instance table is the pipeline's only descriptor set: one set per frame slot,
holding the storage buffer, rewritten each `execute` because the table buffer can be
replaced when it grows. The cascade's matrix travels as a single push constant,
pushed once per cascade rather than once per draw, from the `m_cascadeData` the same
`execute` just recomputed:

{{cite ohao/render/deferred/csm_pass.cpp "const ShadowPushConstant push{m_cascadeData.viewProj[cascade]};"}}

:::why
The cascade matrix is the only thing that changes between the four passes, so it is
the only thing that needs to ride in the command buffer: 64 bytes, half of the
128-byte push-constant size every Vulkan device must support, pinned by a
`static_assert`. Everything per-object lives in the instance table, which the G-buffer
pass reads too, so the shadow raster and the main view cannot disagree about where an
object is. A single-pass layered variant (a geometry shader or `gl_Layer` from the
vertex stage broadcasting to all four layers) would cut the draw count by four, but
needs a layered framebuffer and either a GS stage or `shaderOutputLayer`; at four
cascades the plain loop is the portable choice.
:::

{{cite ohao/render/deferred/csm_pass.hpp "static_assert(sizeof(ShadowPushConstant) == 64);"}}

The same matrices also go to the *sampling* side: `execute` memcpys `m_cascadeData`
into the persistently-mapped cascade UBO, and `DeferredRenderer` hands that buffer
to the lighting pass, where binding 12 feeds `calculateShadowCSM`.

{{cite ohao/render/deferred/deferred_renderer.cpp "setCascadeBuffer(m_csmPass->getCascadeBuffer())"}}

The render pass leaves every layer in `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`; the move to
a shader-readable layout before lighting is the render graph's barrier, which covers
all four array layers rather than layer 0 alone.

## Contracts

- `setCameraData` must receive the **unjittered** projection. `DeferredRenderer` passes `m_proj` while the GBuffer pass gets `jitteredProj`; a jittered matrix would nudge every cascade sphere each frame and defeat the texel snapping.
- The lighting shader applies CSM only to lights of type 0 (directional) and only when flag bit 2 is set. Point and spot lights are unshadowed on this path.
- The cascade UBO is a single persistently-mapped allocation written from `execute()` with no per-frame ring buffer, so the *lighting* read of an earlier frame still in flight can see the matrices the CPU wrote for a later one. The shadow raster is immune: it takes its matrix from a push constant recorded into its own command buffer, and its instance-table set is per frame slot.
- The shadow sampler is created with `compareEnable = VK_TRUE` and `VK_COMPARE_OP_LESS_OR_EQUAL`, but binding 6 is declared `sampler2DArray` (not `sampler2DArrayShadow`) and the shader compares depths itself. The comparison state is dead, and Vulkan requires a compare-enabled sampler to be used with depth-compare instructions.
