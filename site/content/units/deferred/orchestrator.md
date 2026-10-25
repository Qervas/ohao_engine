---
module: deferred
id: orchestrator
title: DeferredRenderer orchestrator
standard: v2
figures: [deferred-orchestrator-frame-schedule]
---

## A schedule written by hand, checked by a graph

Up to fourteen stages run per deferred frame, all recorded into one command buffer by
`DeferredRenderer::render`. Eight go through `RenderGraph`; the rest go straight onto
`cmd`. That split is most of the class, because the graph does far less than its name
implies.

It does not schedule. `topologicalSort` copies passes into the execution list in
`addPass` order and stops:

{{cite ohao/render/graph/render_graph.cpp "// Simple linear ordering for now"}}

So `csm` → `gbuffer` → `ssao` → `rt.shadows` → `rt.shadows.filter` → `rt.gi` → `rt.gi.denoise`
→ `lighting` (the pass registry's names, each timed under it; `csm` only in frames where RT
shadows do not run, `rt.shadows.filter` and `rt.gi.denoise` only in realtime quality, `ssao`
only while the effect is on)
is a decision made by hand
in `render()`, not from declared dependencies. What the graph earns is barriers:
`computeBarriers` walks that order tracking each texture's last access and emits a
`VkImageMemoryBarrier` at the first read following a write.

## declareColorWrite: a promise about a render pass the graph cannot see

Three of the eight are graphics passes — CSM, GBuffer, DeferredLighting — each owning its
`VkRenderPass` and framebuffers, built before the graph existed; SSAO, the RT shadow trace
and filter, and the RT GI trace and denoiser enter through `addComputePass` and own neither. The graph could take those render passes
over, through `useColorAttachment`. It uses the other door:

{{cite ohao/render/graph/render_graph.cpp "// NOT added to colorAttachments"}}

`declareColorWrite` and `declareDepthWrite` record a `ResourceAccess` for barriers and
withhold the attachment, so `vulkanRenderPass` stays `VK_NULL_HANDLE` and `execute`
invokes the lambda with no render pass open around it — which is what lets the pass
begin its own inside.

The price is an invariant nothing checks: the layout handed to `declare*Write` must be
the `finalLayout` of the render pass the graph never sees. GBuffer declares
`DEPTH_STENCIL_READ_ONLY_OPTIMAL` for depth, which has to match attachment 4 of its
five-attachment render pass:

{{cite ohao/render/deferred/gbuffer_pass.cpp "attachments[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;"}}

Change it, forget the declaration, and nothing fails to build — the next barrier carries
an `oldLayout` the image is not in. Of those five attachments only
depth, normal and albedo are imported; position and velocity are outside the graph's
tracking entirely.

:::why
Letting the graph own attachments would mean redrawing every pass into graph-allocated
images, trading hand-tuned load ops and subpass dependencies for barrier bookkeeping.
Demoting it to a barrier calculator bought the missing shadow-map transition without
touching CSM, GBuffer or lighting internals — at the cost of a declared layout kept in
sync by discipline.
:::

## The layout the graph never learns

`PhysicalTexture::currentLayout` is written at import, reset to `UNDEFINED` by `reset()`
each frame, and updated inside `execute` as barriers are issued — never during
compilation. Since `compile()` runs `computeBarriers` first, every tracked texture reads
as `UNDEFINED` while the barriers are computed.

That stays invisible on the anticipated path: a read following a *write* takes its
`oldLayout` from the write's declared layout. A read following another *read* fails
`isWrite()` and falls to the second branch, which sources `oldLayout` from the stale
field and pairs it with `TOP_OF_PIPE` and an empty access mask:

{{cite ohao/render/graph/render_graph.cpp "// Transition from undefined"}}

`gbuffer_normal` is read by the first compute pass (`ssao` when the effect is on, else
`rt.shadows`) and again by each pass after it, so the graph emits exactly that — after
the write, immediately before lighting samples it. `UNDEFINED`
licenses the driver to discard the contents, the source scope carries no dependency, and
both are legal, so validation stays silent.

## One layout per texture, so every layer moves

The CSM shadow map is a four-layer array, 2048 × 2048 per cascade, imported through
`CSMPass::getShadowMapArrayView()`. `PhysicalTexture` tracks a single
`currentLayout` per texture and has no notion of layers, so the only barrier
consistent with that model is one that moves the whole image:

{{cite ohao/render/graph/render_graph.cpp "imageBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;"}}

The `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` → `SHADER_READ_ONLY_OPTIMAL` transition before
deferred lighting therefore covers all four cascades the lighting shader samples
through its array view. The mip range stays hardcoded to level 0, which is all any
imported texture has today; a mipped import would need the same treatment.

## frameIndex selects a descriptor set, and a jitter sample it is wrong for

`render()` hands `frameIndex` — `VulkanRenderer`'s frame-in-flight slot, whose fence
was waited before recording began — to every pass `execute` it calls. Its first job is
to pick descriptor sets. Every pass whose inputs change between frames owns one set
per slot and rewrites only that slot's set at record time: the G-buffer's scene set,
lighting, SSAO, SSS, SSR, TAA, bloom's input set and the tonemap. A single shared set rewritten every
frame would be modified while up to two already-submitted frames still read it;
the slot's set is the one whose previous reader the fence has retired.

{{cite ohao/render/deferred/deferred_lighting_pass.hpp "std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};"}}

CSM, sky and gizmo own no set or write theirs only at setup or when an input view changes, and
leave the parameter commented out. Its one other consumer is
`TAAPass::getJitterOffset`, read at the top of `render()` to perturb the projection.
There it is the wrong number.

Jitter goes into the matrix, not the camera:

{{cite ohao/render/deferred/deferred_renderer.cpp "jitteredProj[2][0] += jitter.x;"}}

GLM matrices are column-major, so `[2][0]` is the entry in row 0, column 2 — call it
$P_{02}$. For a right-handed perspective $w_{\text{clip}} = -z_v$ and $P_{02} = 0$, so
adding $j_x$ there gives, for a view-space point $(x_v, z_v)$:

$$x_{\text{ndc}} = \frac{P_{00}\,x_v + j_x\,z_v}{-z_v} = -\frac{P_{00}\,x_v}{z_v} - j_x$$

The $z_v$ cancels: the shift is a constant $-j_x$ in NDC at every depth. The frustum is
sheared, not the camera translated — near and far planes stay put, no parallax appears,
only the sample grid moves.

Both the amount and the sequence are off. NDC spans two units across $W$ pixels, so a
shift of $j_x$ is $j_x W/2$ pixels; the jitter is a Halton offset
$h_x \in [-0.5, 0.5)$ over $W$, landing as $h_x/2$ pixels — half the nominal sub-pixel
step:

{{cite ohao/render/deferred/taa_pass.cpp "return m_jitterSequence[sampleIndex] / glm::vec2(m_width, m_height);"}}

The sequence also never advances: `TAAPass` builds 16 Halton samples indexed
`frameIndex % 16`, but the entry point passes `m_currentFrame`:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->render(cmd, m_currentFrame);"}}

which is the frame-resource ring index, wrapping at `MAX_FRAMES_IN_FLIGHT` = 3:

{{cite ohao/render/frame/frame_resources.hpp "return (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;"}}

Only samples 0, 1 and 2 are ever reached: base-2 offsets
$h_x \in \{0, -\tfrac14, +\tfrac14\}$, which after the halving is three positions
spanning 0.25 px against 0.45 px for the full sixteen (base-3, 0.28 px against
0.43 px).

The same jitter poisons motion vectors: the GBuffer gets `jitteredProj` while
`m_prevViewProj` is stored unjittered at the end of `render()`, and `gbuffer.frag`
writes velocity as half the NDC difference of the two — a screen-constant $-j_x/2$ UV
bias that changes every frame.

## The half of the frame the graph never sees

{{figure deferred-orchestrator-frame-schedule "Frame schedule and HDR dataflow, read from render() — conceptual, not a captured render or a profile. Green ticks mark graph-inserted barriers; sky and particles write the lighting image before SSS blurs it in place."}}

After `m_renderGraph.execute(cmd)` returns, sky, particles, SSS, SSR, post-processing
and gizmos are recorded directly, each rolling its own barriers. `SSSPass` blurs the
skin tiles of the lit image *in place*, and SSR traces that same image at half
resolution, so post-processing always reads the lighting output:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_postProcessing->setHDRInputWithImage(m_lightingPass->getOutputView(), m_lightingPass->getOutputImage());"}}

Sky and particles still come first. Both draw into the lighting image, and SSS's blur
and SSR's reflections should see what they drew: an SSS pass recorded before the sky
would blur skin edges against a black background, and SSR would reflect none. Since
`deferred_lighting.frag` sets `outColor = vec4(0.0)` wherever the GBuffer position
sample is exactly zero, and `sky.frag` fills whatever the GBuffer left at far depth
(it discards below 0.9999), the sky pass is the whole sky, and until it runs the
background those taps and hits would read is black. Both write through render passes as colour
attachments, which no compute read is ordered against, so when either ran a memory
barrier from colour-attachment output to the compute and fragment stages comes
before SSS:

{{cite ohao/render/deferred/deferred_renderer.cpp "// Sky and particles drew into the lit image as a color attachment; SSS (compute), SSR"}}

`onResize` keeps every screen-sized target at the output extent — the GBuffer,
lighting, post, gizmo, sky, SSS, SSR and both RT techniques.

## Ordering constraints that are not obvious

Before compiling the graph, `render()` clears the RT shadow visibility and the GI view
out of the lighting pass. They come back only from inside the RT pass lambdas — the
shadow visibility from `rt.shadows.filter` in realtime quality or from `rt.shadows`
in reference, the GI view likewise from `rt.gi.denoise` in realtime or from `rt.gi` in
reference — and only when that technique's `render()` actually traced:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_lightingPass && m_rtGI->rendered())"}}

The hand-off happens at record time, during `m_renderGraph.execute(cmd)`, and its
consumer is `DeferredLightingPass::execute` later in the same walk, which writes its
slot's descriptor set from those members and re-derives the RT-shadow and SSGI flag
bits from them. So the dependency is pure graph order: the RT passes must be added
before `DeferredLighting`. Since `topologicalSort` keeps insertion order and the
lighting pass declares no read of the RT outputs, nothing in the graph enforces it —
move the RT blocks below lighting and every frame silently falls back to CSM and no
GI.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 32;"}}

Two clocks feed the passes, and neither depends on any pass running. `m_totalTime`
advances by the frame's delta on every frame, outside the particle block; the sky
pass takes it as a star seed, and the particle pass compares it against the end of
the last emit's longest lifetime to decide whether to run at all. The RT passes
are seeded instead by a frame counter, `m_framesSinceReset`, taken once per
`render()` and handed to both RT shadows and RT GI with the render seed:

{{cite ohao/render/deferred/deferred_renderer.cpp "const uint32_t rtFrame = m_framesSinceReset++;"}}

`resetTemporalState` zeroes both clocks, keeps a pending particle lifetime by moving
it onto the restarted clock, and drops RT GI's and TAA's history, so frame *k*
after a reset traces the same rays in every run with the same seed.

{{cite ohao/render/deferred/deferred_renderer.cpp "m_particlesLiveUntil -= m_totalTime;   // the same remaining lifetime on the restarted clock"}}

:::key
`RenderGraph` here is a barrier calculator, not a scheduler and not a resource manager.
Pass order, lifetimes and every barrier outside the eight graph passes are hand-written.
:::

## Contracts

- The layout passed to `declare*Write` must equal the `finalLayout` of that pass's own `VkRenderPass`, and a declaring pass must never also take a graph-owned attachment. Neither is validated.
- A tracked texture read by two graph passes in a row gets a second barrier with `oldLayout = UNDEFINED` and `TOP_OF_PIPE` as its source: `currentLayout` is never refreshed during compilation.
- `setRTShadowMask` and `setSSGITexture` must be called before `DeferredLightingPass::execute` records, which happens only if the RT passes are added to the graph ahead of lighting. A view handed over after lighting recorded is cleared again before the next frame's graph runs.
- `frameIndex` must be the slot whose fence `VulkanRenderer` just waited: it selects the per-slot descriptor set each pass rewrites.
- SSS blurs the lighting HDR image in place and SSR reads it, so anything drawn into it after them reaches the tonemapper but is neither blurred nor reflected. Sky and particles are recorded before both for that reason, followed by the colour-attachment-to-compute barrier; a new pass drawing into the lit image belongs in the same place.
- `importGraphTextures()` must run after any pass reallocates its images, or the graph holds destroyed `VkImage` handles.
- `frameIndex` is a ring index of period 3, not a monotonic counter, so `TAAPass::getJitterOffset` sees three values.
