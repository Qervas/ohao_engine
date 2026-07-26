---
module: hybrid
id: visibility
title: Visibility helpers
standard: v2
---

## Eight bits between a ray and a triangle

Every TLAS instance in Vulkan carries an 8-bit `mask`, and every `traceRayEXT`
call carries an 8-bit cull mask. When the two share no set bit the traverser
drops the instance before any intersection test — no BLAS descent, no any-hit
invocation, no shader cost. `rt_visibility.hpp` is the twenty-two lines that
name those bits for OHAO, and it opens by stating the rule it is built on:

{{cite ohao/render/rt/rt_visibility.hpp "Vulkan rule: ray intersects instance if (ray_mask & instance_mask) != 0."}}

Writing that as a predicate makes the rest of this page checkable. Let $m_i$ be
the instance mask that ends up in `VkAccelerationStructureInstanceKHR::mask`,
and $m_r$ the cull mask passed as the third argument of `traceRayEXT`:

$$\text{visible}(r,\,i) \iff (m_r \wedge m_i) \neq 0$$

with $\wedge$ the bitwise AND over eight bits. The scheme the header sketches
gives bit 0 the meaning "GI and shadow rays may see this," reserving bits 1–7
for ray classes that do not exist yet (reflection, AO). A ray that wants only
rigid geometry passes $m_r = \texttt{0x01}$; geometry it must not see clears
bit 0.

Only the ray-mask half is hybrid-specific. Every `traceRayEXT` in
`pt_raygen.rgen`, `pt_raygen_offline.rgen` and `pt_raygen_realtime.rgen` passes
`0xFF`, so the path tracers never cull by mask; `rt_shadow.rgen` and
`rt_gi.rgen` are the only rays in the tree that pass anything else. The
instance-mask half is *shared*: one TLAS builder stamps the mask into every
instance, and the same `RTAccelerationStructure` object is handed to the
deferred renderer and to the path tracer.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->render(cmd, m_rtAccel.get(), ptView, ptProj,"}}

A bad instance mask is therefore not contained to the hybrid passes.

## The asymmetry that never got built

Read the five constants and the scheme collapses. Static geometry is `0xFF`,
animated geometry is `0x00`:

{{cite ohao/render/rt/rt_visibility.hpp "MASK_ANIMATED       = 0x00;  // TEST"}}

`0x00` does not mean "invisible to GI rays." Under the predicate above it fails
against *every* $m_r$, including the `0xFF` of the path tracer's primary trace —
so in the *path-traced* image an instance carrying it is a hole in the frame,
not a missing bounce. The deferred frame survives it, because there primary
visibility is rasterized by the GBuffer pass and the RT techniques are compute
passes layered over that GBuffer; a `0x00` instance would still rasterize and
merely lose its RT shadow and GI contribution. The `TEST:` prefix is the author
flagging it as a diagnostic value. The production value would be something like
`0xFE`: visible to camera rays, culled by anything that passes `0x01`.

None of that bites today, because nothing ever assigns it. Exactly one mask
reaches a TLAS instance, applied uniformly to every actor in the build loop:

{{cite ohao/gpu/vulkan/rt_build.cpp "uint32_t instanceMask = rt::MASK_STATIC_ONLY;"}}

and `MASK_STATIC_ONLY` is itself `0xFF`, bit-identical to `MASK_VISIBLE_ALL`.
The reason sits a few lines above the instance loop: the skinned-geometry path
that motivated the split was removed from the RT builder, so every BLAS in the
tree is rigid.

{{cite ohao/gpu/vulkan/rt_build.cpp "// Skeletal animation removed — all meshes are static."}}

Consequently $\texttt{0x01} \wedge \texttt{0xFF} = \texttt{0x01} \neq 0$ for
every instance in every scene the engine can currently build: the shadow and GI
rays that carefully pass `RAY_MASK_STATIC` cull nothing. The mechanism is wired
end to end and correct; it is idle because the geometry class it excludes no
longer exists.

:::key
`MASK_STATIC_ONLY` and `MASK_VISIBLE_ALL` are the same number. Reintroducing
animated RT geometry means *changing `MASK_STATIC_ONLY`* to a value with the
animated bit cleared — not merely starting to write `MASK_ANIMATED`. Add the new
instance class alone and the cull still passes everything.
:::

## Four copies of one constant

The header is C++; the cull mask is consumed in GLSL. There is a mirror file
that promises to keep the two in step:

{{cite shaders/rt/includes/rt_masks.glsl "// RT visibility masks — must match ohao/render/rt/rt_visibility.hpp"}}

No shader includes it. `rt_gi.rgen` and `rt_shadow.rgen` each pull exactly one
header, `includes/common/encoding.glsl`, and nothing else in `shaders/` names
`rt_masks.glsl` at all. Both consumers instead hard-code the literal and point
at the C++ header in a comment — the GI raygen:

{{cite shaders/rt/rt_gi.rgen "uint rayMask = 0x01; // RT_RAY_MASK_STATIC (see rt_visibility.hpp)"}}

and the shadow raygen, inline in the trace call. So `0x01` is written down in
four places — the C++ `RAY_MASK_STATIC`, the uncompiled `RT_RAY_MASK_STATIC`
macro, and one literal in each raygen — with no compiler or build step linking
them, and the one file whose whole job is to prevent drift is dead. The C++ side
is barely better off:
`render_dispatch.cpp` includes `rt_visibility.hpp` and references no symbol
from it — `rt_build.cpp` is the only real user.

## When the shader stops trusting the hardware

The most interesting line in this unit is a shader that refuses to believe the
cull mask works at all. `rt_gi.rchit` re-implements the visibility test in
software, on the grounds that the hardware ignored it:

{{cite shaders/rt/rt_gi.rchit "// Workaround for RTX 5070 driver bug: traceRayEXT cull mask is ignored."}}

The fallback smuggles the static/animated bit through the alpha channel of the
per-instance material SSBO (set 0, binding 6): the closest-hit shader reads
`materials[gl_InstanceID].a`, and if it is below 0.5 it writes the miss sentinel
`vec4(0,0,0,-1)` and returns, so the raygen's `giPayload.a >= 0.0` test treats
the hit as empty space. This is a per-hit shader cost paid to emulate a
zero-cost traversal reject — the trade you take when the free version is
untrustworthy.

It is also inert. `RTGITechnique::setMaterialAlbedos` takes the flag vector as a
defaulted parameter:

{{cite ohao/render/rt/rt_gi_technique.hpp "const std::vector<float>& flags = {});"}}

and the single call site omits it:

{{cite ohao/gpu/vulkan/rt_build.cpp "if (gi) gi->setMaterialAlbedos(materialAlbedos);"}}

so every alpha is written as 1.0, and slots beyond the instance count keep the
buffer's `vec4(0.8, 0.8, 0.8, 1.0)` initialiser. `isStatic < 0.5` is never true.
Two independent culling mechanisms, both correctly built, both currently
no-ops — because the thing they cull was deleted upstream. Neither is dead code
in the "delete it" sense; both are the load-bearing half of a feature waiting for
its other half.

## Four interfaces, two implementations

`render_technique.hpp` is the other half of this unit: a pluggable-technique
contract for the hybrid passes. Each feature gets an `Input` struct of GBuffer
views, camera matrices and a nullable `RTAccelerationStructure*`, an `Output`
struct of one image plus its view, and a uniform lifecycle —
`init / resize / render / getOutput / destroy`, plus `getName()` and `needsRT()`
for introspection. Four interfaces are declared: shadows, GI, reflections, AO.

Two have implementations. `RTShadowTechnique` and `RTGITechnique` are the only
classes in the tree deriving from any of them; `IReflectionTechnique` and
`IAOTechnique` have zero implementers. The screen-space features that would fit
those two slots do exist, but on a different base — `SSRPass` and `SSAOPass`
both derive from `RenderPassBase`, the base the rest of the deferred pipeline
uses, and from none of the four interfaces:

{{cite ohao/render/deferred/ssao_pass.hpp "class SSAOPass : public RenderPassBase"}}

The `Output` doc comments have drifted too — `ShadowOutput::shadowMask` is
annotated "R8 or R16F" —

{{cite ohao/render/rt/render_technique.hpp "VkImage shadowMask;            // R8 or R16F — 0=shadowed, 1=lit"}}

— while the implementation commits to a single format, `VK_FORMAT_R8_UNORM`, and
`GIOutput::indirectLight` is documented RGB16F but allocated RGBA16F.

{{cite ohao/render/rt/rt_shadow_technique.cpp "imageInfo.format = VK_FORMAT_R8_UNORM;"}}

## The runtime swap that is not wired

The header's premise is polymorphic substitution, stated in its own example:

{{cite ohao/render/rt/render_technique.hpp "Swap at runtime: renderer.setShadowTechnique"}}

No such setter exists anywhere in the tree. `DeferredRenderer` owns the
techniques by *concrete* type, not by interface pointer:

{{cite ohao/render/deferred/deferred_renderer.hpp "std::unique_ptr<RTShadowTechnique> m_rtShadow;"}}

and `getName()` / `needsRT()` are overridden but never called. The virtual
dispatch is therefore paid for and unused: the vtable exists, the indirection
happens, and the pointer is always the same derived type. What actually varies
at runtime is coarser — `init()` returning false makes the renderer drop the
technique and fall back to CSM shadows, a boolean choice, not a technique swap.

:::why
`rt_meta.hpp` hedges this by writing the same surface down a second time, as a
C++20 concept:

{{cite ohao/render/rt/rt_meta.hpp "concept ShadowTechniqueLike = requires(T t, VkCommandBuffer cmd, const ShadowInput& in,"}}

The assertion that binds concept to implementation cannot live in `rt_meta.hpp`
— the include edge runs the other way, `rt_shadow_technique.hpp` and
`rt_gi_technique.hpp` both include it — so each derived header asserts against
itself, one line below the class it just closed:

{{cite ohao/render/rt/rt_shadow_technique.hpp "static_assert(ShadowTechniqueLike<RTShadowTechnique>);"}}

So the shape is checked structurally as well as virtually, and that is the
escape hatch: once the virtual bases go, `template<ShadowTechniqueLike T>` keeps
the same guarantee with no vtable. The rejected alternative — deleting the
interfaces outright — throws away the documented shape that both implementations
and the fallback logic already agree on.

The two variable templates the concepts also constrain carry none of that
weight:

{{cite ohao/render/rt/rt_meta.hpp "inline constexpr bool kIsShadowTechnique = true;"}}

A constrained variable template is only checked when instantiated, and neither
`kIsShadowTechnique` nor `kIsGITechnique` is instantiated anywhere in the tree.
The `static_assert`s are the whole of the enforcement.
:::

## The write that must precede the graph

The RT techniques record into the render graph as compute passes, but their
outputs are handed to the deferred lighting pass by CPU-side descriptor writes
that are *not* graph edges. That creates a real ordering constraint, flagged in
the source:

{{cite ohao/render/deferred/deferred_renderer.cpp "// Pre-bind RT shadow mask to lighting pass descriptors (must happen before execute)"}}

`getOutput()` → `setRTShadowMask()` → `updateDescriptorSets()` run
unconditionally, every frame, a few lines above `m_renderGraph.compile()` and
`execute()`. Move the block below `execute()` and the lighting pass samples the
previous frame's view. Being unconditional also makes it self-healing: any image
the technique reallocates is re-bound on the next frame.

The resize path carries the opposite defect, and it is live.
`DeferredRenderer::onResize` forwards the new extent to the GI technique:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_rtGI) m_rtGI->resize(width, height);"}}

and never to the shadow technique, though `RTShadowTechnique::resize` is
implemented — destroy view, image and memory, reallocate at the new extent:

{{cite ohao/render/rt/rt_shadow_technique.cpp "void RTShadowTechnique::resize(uint32_t width, uint32_t height) {"}}

Nothing calls it. The shadow mask keeps its `init()` extent while the dispatch
that fills it takes its grid from `ShadowInput`, which the renderer sets from
the new `m_width`/`m_height`:

{{cite ohao/render/rt/rt_shadow_technique.cpp "input.width, input.height, 1);"}}

Grow the window and the trace launches more threads than the image has texels;
the out-of-range `imageStore`s are discarded, and the lighting pass samples an
old-extent mask with normalized coordinates, stretched over the whole frame. The
re-bind keeps the handle valid — it cannot fix the resolution.

## Contracts

- Instance mask and ray mask must be non-disjoint or the instance is invisible: `(m_r & m_i) != 0`. `MASK_ANIMATED = 0x00` is disjoint with *everything*, including the `0xFF` the path tracer's primary trace passes, so it removes geometry from the path-traced image outright — a debugging value, not the production animated mask.
- Instance masks are written once, by the shared TLAS builder, into the one `RTAccelerationStructure` both the deferred renderer and the path tracer consume. A mask change is not scoped to the hybrid passes.
- `MASK_STATIC_ONLY` is `0xFF` today, so `RAY_MASK_STATIC` culls nothing. Any future animated-geometry class must clear a bit in the *static* mask, not just define a new one.
- `0x01` exists as the C++ `RAY_MASK_STATIC`, as the uncompiled `RT_RAY_MASK_STATIC` macro in `rt_masks.glsl`, and as a hard-coded literal in each of `rt_gi.rgen` and `rt_shadow.rgen` — four sites. Changing the header does not change the shaders.
- The `rt_gi.rchit` alpha-channel cull only activates if `setMaterialAlbedos` is called *with* the flags vector. The one caller omits it, so the branch is currently unreachable.
- Technique outputs must be re-bound into the lighting pass descriptors before `renderGraph.execute()`; the graph does not track this dependency. The re-bind is unconditional per frame, so it survives reallocation — but only reallocation that actually happens.
- `DeferredRenderer::onResize` must forward the new extent to every RT technique it owns. It forwards to `m_rtGI` and not to `m_rtShadow`, so the shadow mask stays at its `init()` extent while the dispatch grid follows the window.
- `init()` failure is non-fatal by design — the renderer resets the `unique_ptr` and continues on the CSM path, so a null `m_rtShadow` is a valid steady state, not an error.
