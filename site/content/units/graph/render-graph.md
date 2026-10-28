---
module: graph
id: render-graph
title: RenderGraph
standard: v2
figures: [graph-render-graph-layout-timeline]
---

## Four promises, one half-delivery

The class docblock promises a full frame graph: deduce resource lifetimes, alias
transient memory, generate optimal barriers, sort passes into execution order.

{{cite ohao/render/graph/render_graph.hpp " * - Alias transient resource memory"}}

Only the third has any implementation behind it, and the barriers it generates
are neither optimal nor, for a texture read twice in a row, fully correct; both
failures are traced below. The only production consumer is
`DeferredRenderer`, which rebuilds the graph every frame — `reset()`, up to seven pass
registrations of eight possible, `compile()`, `execute()`, all inside one command buffer — to get
one thing: image-layout barriers computed centrally, between passes that know
nothing about each other. Nothing is aliased, nothing is reordered, no memory is
owned. Reading this unit as "a frame graph" will mislead you; reading it as "a
barrier scheduler with a frame-graph-shaped API" will not.

The eight are `csm`, `gbuffer`, `ssao`, `rt.shadows`, `rt.shadows.filter`, `rt.gi`,
`rt.gi.denoise` and `lighting`, in that order — three `addPass`, five `addComputePass` —
named as the pass registry names them, since the graph times each pass under its name.
`gbuffer` is the exception: it is registered self-timed, and the graph neither opens
nor closes a timer around it, because the G-buffer pass times its five parts itself
(`cull.instances`, `gbuffer.early`, `hiz.build`, `cull.late`, `gbuffer.late`) and the
pass timer keeps one query open at a time:

{{cite ohao/render/graph/render_graph.cpp "const bool timed = m_passTimer && !pass.selfTimed;"}}

So the registry and `render.stats` name those five, and no `gbuffer`. Six of the eight
graph passes are conditional. `ssao` is registered only while the effect is on. `rt.shadows` and
`rt.gi` need `RTShadowTechnique` and `RTGITechnique` to have initialised and
`RenderScene` to have a built TLAS (its handle stays null without ray tracing and
before the first build), which on ray-tracing hardware with geometry in the scene is
the ordinary case, not an opt-in; `rt.shadows` also needs a shadow-casting light, and
`rt.shadows.filter` runs only after it in realtime quality, as `rt.gi.denoise` runs
after `rt.gi`. `csm` is the other side of
the same switch: it is registered only in frames where `rt.shadows` is not. The
settings (`render.settings`) turn passes off the same way, by not registering them:
`rtGI.enabled`, `ssr`, `sss` and `rtShadows.enabled` each gate their passes here.

{{cite ohao/render/deferred/deferred_renderer.cpp "const bool giTraced = m_useRTGI && m_rtgiWanted && m_rtGI && sceneTlas != VK_NULL_HANDLE && m_gbufferPass;"}}

## The graph owns nothing it schedules

All five textures the deferred frame tracks — G-buffer normal, albedo and depth,
the CSM shadow array, the lighting output — enter through
`importTexture`, which marks them external with `ownsMemory = false`. Allocation
then skips them, and so does teardown.

{{cite ohao/render/graph/render_graph.cpp "if (!desc.isExternal && m_physicalTextures[i].image == VK_NULL_HANDLE) {"}}

The other half of the file — `allocateTexture`, `findMemoryType`, the
`DEVICE_LOCAL` image and view creation — runs only for a descriptor that is not
external, and the only way to make one is the `PassBuilder::create*Attachment`
family, which nothing in `ohao/`, `examples/` or `tests/` calls. `vkCreateImage`
here has never run in a shipping frame, and `isTransient` — the flag the docblock
hangs aliasing on — sits on every texture and buffer descriptor with no reader
anywhere in the tree.

## Declare-only: how a pass keeps its own VkRenderPass

The hinge of the whole design is a deliberate omission. `declareColorWrite` and
`declareDepthWrite` record a resource access for barrier purposes but do *not*
push the handle into the pass's attachment list.

{{cite ohao/render/graph/render_graph.cpp "// NOT added to colorAttachments"}}

That omission cascades. `createRenderPasses` skips any pass with no attachments,
so `vulkanRenderPass` stays null, so the `vkCmdBeginRenderPass` block in
`execute()` never fires and the framebuffer loop finds nothing to build.

{{cite ohao/render/graph/render_graph.cpp "if (pass.colorAttachments.empty() && !pass.depthAttachment.isValid()) {"}}

For such a pass `execute()` does exactly two things: flush the compiled barrier
list, then invoke the callback — which begins and ends the pass's own
`VkRenderPass` itself.

The price is a contract with no compiler behind it: the `finalLayout` argument
must equal the `finalLayout` of the corresponding attachment in the pass's own
render pass, because that value is what the graph will use as the *source* layout
of the next barrier. The G-buffer pass declares `SHADER_READ_ONLY_OPTIMAL` for
its colour handles and `DEPTH_STENCIL_READ_ONLY_OPTIMAL` for depth —

{{cite ohao/render/deferred/deferred_renderer.cpp "builder.declareColorWrite(m_graphNormalHandle,"}}

— because that is what the attachment descriptions of its late render pass, the
second of its two, say, in a different file that the graph never reads.

{{cite ohao/render/deferred/gbuffer_pass.cpp "a.finalLayout = late ? (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL"}}

:::why
Letting the graph build the render passes was the obvious alternative, and the
code for it is right there in `createRenderPasses`. It was rejected because that
generated pass has one fixed shape: a single subpass, `LOAD_OP_CLEAR` on every
attachment, no subpass dependencies, and hardcoded final layouts —
`COLOR_ATTACHMENT_OPTIMAL` for every colour attachment,
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` for the depth one. The G-buffer needs two
compatible render passes over the same targets, one clearing and one loading, with
compute work between them and four colour targets ending in
`SHADER_READ_ONLY_OPTIMAL`, plus two external subpass dependencies; CSM needs one
framebuffer per cascade layer over a shared render pass. Declare-only keeps the
barrier model and leaves each pass free — at the cost of two descriptions of the
same layout that a human keeps in sync.
:::

## What the barrier compiler actually knows

`computeBarriers` is one linear walk carrying a map from texture index to the
last `ResourceAccess` on it, and it has four branches, not the two the shape of
the problem suggests. A read whose predecessor is a write gets a full barrier —
writer's stage, access and layout on the source side, reader's on the
destination. A read whose predecessor is anything else falls to a second branch
that takes its old layout from the physical resource, and only fires if that
layout differs from the one the reader wants.

{{cite ohao/render/graph/render_graph.cpp "} else if (m_physicalTextures[read.texture.index].currentLayout != read.imageLayout) {"}}

Writes split the same way. A write with a recorded predecessor gets a barrier
only if the two layouts differ. A write with no predecessor — the texture's first
appearance this frame — always gets one, and its source layout is not read from
the physical resource at all but hardcoded to `UNDEFINED`.

{{cite ohao/render/graph/render_graph.cpp "barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

Because a declare-only write records the layout the image will be in *after* its
pass, that first-use barrier emitted *before* the pass moves the image to its
post-pass layout. The inversion is harmless only because every self-managed
pass's first render pass uses `initialLayout = UNDEFINED` and clears (for the
G-buffer, the early one): the pass immediately overwrites what the graph just asked
for.

Three barriers carry the graph's weight in the frame, and two of them are the same
edge. The G-buffer's render passes ship their own external subpass dependencies, and
the outgoing one's destination stages include `COMPUTE_SHADER` as well as
`FRAGMENT_SHADER`: the Hi-Z build samples the early pass's depth between the two
render passes, and the two must carry identical dependencies to stay compatible, so
the union covers every compute reader of the targets too.

{{cite ohao/render/deferred/gbuffer_pass.cpp "dependencies[1].dstStageMask = kReaders | kAttachments;"}}

Three of the graph's passes are compute dispatches that sample G-buffer colour —
`ssao`, `rt.shadows` and `rt.gi` — and two of them own a target's *first* read
after the G-buffer wrote it: the first of them on `gbuffer_normal` (`ssao` when
the effect is on, else `rt.shadows`), `rt.gi` on `gbuffer_albedo`. Only those two
take the read-after-write branch, so `COLOR_ATTACHMENT_OUTPUT → COMPUTE_SHADER` is
emitted exactly twice, each because a setup lambda called `readComputeTexture` and
the graph turned it into a compute-stage read.

{{cite ohao/render/deferred/deferred_renderer.cpp "builder.readComputeTexture(m_graphAlbedoHandle);"}}

Neither moves the image: both layouts are `SHADER_READ_ONLY_OPTIMAL`, the value
`declareColorWrite` recorded. They exist purely for their stage and access scopes,
and they restate the edge the render pass's dependency already makes: the ledger
states it from the declarations, the dependency from a hand-written stage mask, and
either alone would order the reads.

The third, in a frame where CSM runs, is the shadow map, which CSM leaves in
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` and the lighting pass samples. In a frame that
traces RT shadows instead, the lighting pass's read of the shadow map is its first
access and takes the read-from-`UNDEFINED` branch below; the map is bound but not
sampled, since the lighting shader's CSM bit is clear.

{{cite ohao/render/deferred/deferred_renderer.cpp "builder.readTexture(m_graphShadowHandle,"}}

That transition is also where the emitter gets a detail right that is easy to get
wrong: the aspect mask comes from the *format*, not the layout. A depth image on
its way to `SHADER_READ_ONLY_OPTIMAL` still needs `VK_IMAGE_ASPECT_DEPTH_BIT`;
deriving the aspect from the destination layout would have produced a colour
aspect and a validation error.

{{cite ohao/render/graph/render_graph.cpp "isDepthFormat(tex.format) ? VK_IMAGE_ASPECT_DEPTH_BIT"}}

{{figure graph-render-graph-layout-timeline "Layout and stage track of four imported textures across csm, gbuffer, ssao, rt.shadows, rt.gi and lighting, drawn on one track although a frame registers csm only when rt.shadows does not run (and adds rt.shadows.filter and rt.gi.denoise, which read no tracked texture, in realtime); each texture's transitions at the passes that do run are as drawn. Conceptual: derived by hand-executing computeBarriers against the pass declarations in deferred_renderer.cpp, not captured from a validation-layer trace."}}

## Why every read-after-read starts from UNDEFINED

`reset()` clears every tracked layout to `UNDEFINED` at the top of each frame, on
the reasoning that an `UNDEFINED → target` transition is always legal.

{{cite ohao/render/graph/render_graph.cpp "tex.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

The subtlety is *when* that field is read. `compile()` runs before any barrier
executes, and `currentLayout` only advances inside `execute()`. So at the moment
the compiler consults it, every texture still reads `UNDEFINED`. The
read-with-a-read-predecessor branch is therefore never "transition from wherever
it was"; it is always "transition from `UNDEFINED`" — and Vulkan permits an
implementation to discard the image contents across that transition.

A frame with SSAO on emits four of them. `gbuffer_normal` is read by `ssao`,
`rt.shadows`, `rt.gi` and `lighting` in that order; only the first of those four
finds a write in the map, so reads two, three and four each produce an
`UNDEFINED → SHADER_READ_ONLY_OPTIMAL` transition on the normal target.
`gbuffer_albedo` contributes the fourth: `rt.gi` reads it first and takes the
read-after-write branch, `lighting` reads it second and does not. The default
frame, without SSAO, has one normal read fewer and emits three.

Both sides of all four name the same layout, so nothing needs to move — which is
why the pipeline renders — but the code depends on that rather than stating it.
No one-line guard helps: skipping the barrier when old equals new is already the
branch's entry condition, and `reset()` has just pinned `currentLayout` to
`UNDEFINED`, so the test passes every time. The fix is structural — track layouts
in compiled state that advances with the walk, not in a physical resource whose
layout field the frame reset has blanked.

## One barrier, four cascades

The CSM shadow map is the frame's one multi-layer resource: a `D32_SFLOAT` array with
one layer per cascade, imported through its full-array view and sampled by the
lighting shader through a single `sampler2DArray` binding.

{{cite ohao/render/deferred/csm_pass.hpp "static constexpr uint32_t CASCADE_COUNT = 4;"}}

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "// 6: Shadow map array (sampler2DArray, 4 cascades)"}}

`importTexture` never records an array-layer count — `TextureDesc::arrayLayers`
stays at its default of 1 for every imported resource — and `PhysicalTexture` keeps
one `currentLayout` per texture. The graph cannot name individual layers, so the
only barrier consistent with its own bookkeeping is one that moves all of them:

{{cite ohao/render/graph/render_graph.cpp "imageBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;"}}

The `DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` transition before
lighting therefore reaches all four cascades. `CSMPass` relies on that: it renders
each cascade through its own single-layer framebuffer, leaves every layer in the
attachment layout, and delegates the transition to the graph. Per-layer layouts —
a pass that wrote cascade 2 while lighting sampled cascade 1 — would need layer
ranges in `ResourceAccess`, which the graph does not have. Mips are still pinned to
level 0, which is all any imported texture has today.

## What is declared and not wired

Knowing how much of this class is inert is part of reading it safely.

- `topologicalSort` emits the identity permutation and admits it in a comment; execution order is entirely the order `addPass` was called in. {{cite ohao/render/graph/render_graph.cpp "// Simple linear ordering for now"}}
- `buildDependencyGraph` runs a four-deep loop nest — passes × reads-per-pass × earlier passes × writes-per-pass, O(P²·R·W) — to compute a per-pass `refCount` that nothing anywhere reads. {{cite ohao/render/graph/render_graph.cpp "pass.refCount = 0;"}}
- `getOptimalLayout` has no callers — but the usage-to-layout mapping it encodes is very much alive, hand-inlined into each `PassBuilder` method instead (`readTexture` sets `SHADER_READ_ONLY_OPTIMAL`, `writeStorageTexture` sets `GENERAL`). Unused function, used mapping. {{cite ohao/render/graph/render_graph.cpp "access.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;"}}
- Buffer accesses are recorded by `readBuffer`/`writeBuffer` and then dropped: `computeBarriers` only inspects textures, and `execute` only walks barriers whose texture handle is valid. Buffer hazards are the caller's problem.
- `setOutput` stores a handle nothing reads and `PassCommandBuffer` has no users. The SSAO images are not imported at all: the `ssao` pass transitions them itself, so the graph's ledger knows nothing of them.

:::key
This is a barrier scheduler wearing a frame graph's API. Its two jobs it does: it
derives the G-buffer's compute readers' dependency from the passes' declarations
(the G-buffer's own render passes state the same edge by hand), and it gets CSM's
shadow map to `SHADER_READ_ONLY_OPTIMAL` before lighting, for the whole array in one
barrier. Aliasing, lifetime deduction and pass reordering are API surface
awaiting an implementation.
:::

## Contracts

- The `finalLayout` passed to `declareColorWrite`/`declareDepthWrite` must equal the attachment `finalLayout` in the pass's own `VkRenderPass`. Change either alone and the next barrier's source layout is a lie the validation layer may or may not catch.
- Passes must be added in dependency order; `topologicalSort` will not repair a wrong one.
- A pass added with `selfTimed = true` must time its own parts under names in the pass registry, or its GPU time disappears from `render.stats`; a pass that is not self-timed must not open a timer of its own while the graph's is open.
- `importGraphTextures` must re-run after every resize — it does — because handles are plain indices that only a `shutdown()`/`initialize()` cycle clears.
- Barriers cover mip 0 and every array layer, all moved together. A mipped resource routed through this graph is under-synchronised, and a multi-layer one cannot have its layers in different layouts.
