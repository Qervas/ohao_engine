---
module: hybrid
id: visibility
title: Visibility helpers
standard: v2
---

## Eight bits between a ray and a triangle

Every TLAS instance in Vulkan carries an 8-bit `mask`, and every `traceRayEXT`
call (or inline ray query) carries an 8-bit cull mask. When the two share no set
bit the traverser drops the instance before any intersection test: no BLAS
descent, no any-hit invocation, no shader cost. Let $m_i$ be the instance mask
that ends up in `VkAccelerationStructureInstanceKHR::mask` and $m_r$ the ray's:

$$\text{visible}(r,\,i) \iff (m_r \wedge m_i) \neq 0$$

with $\wedge$ the bitwise AND over eight bits. OHAO names the bits once, in
`shaders/shared/layout.json`, which `tools/gen_layout.py` turns into the C++
header, the GLSL include and the Slang module, so the TLAS staging and every
trace read the same numbers. Bit 0 is scene geometry, bit 1 a light's emitter
mesh:

{{cite shaders/shared/layout.json "TLAS instance mask of a light's emitter mesh: only camera rays and a near-mirror's first specular ray see it"}}

The instance-mask half is shared: the `RenderScene` stamps the mask into every
instance of its one TLAS, and that same TLAS is what the deferred renderer's RT
passes and the path tracer both trace.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->setSceneBuffers(*m_renderScene);"}}

## Three instance masks, two ray masks

Exactly three values reach a TLAS instance, chosen from its flags:

{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceEmitter) != 0 ? gpu::shared::kTlasMaskEmitter : gpu::shared::kTlasMaskScene;"}}

A hidden instance, or a released slot that keeps its place in the table, carries
zero, which no ray mask intersects: it is a hole in the path-traced image and
casts no RT shadow and gives no GI bounce, as the rasteriser, which draws only
visible instances, agrees. A visible instance carries `kTlasMaskScene` (`0x01`),
a light's emitter mesh `kTlasMaskEmitter` (`0x02`).

Two values reach a trace. The path tracer's camera ray passes `kRayMaskCamera`
(`0x03`) and sees both; every other ray, the path tracer's light samples,
bounces and specular chains and the deferred frame's RT shadow and GI queries,
passes `kRayMaskScene`:

{{cite shaders/rt/shadow_trace.slang "q.TraceRayInline(scene, 0, kRayMaskScene, ray);"}}

That split is what makes an emitter mesh safe. The light it shows is the
analytic light the shaders sample; a shadow ray towards that light would
otherwise hit the mesh around it and the light would shadow itself, and a
bounce that reached the mesh would add the light's radiance a second time on
top of its samples. With the mask, the mesh is only ever what the camera sees.

One more ray passes `kRayMaskCamera`: the first specular ray of a near-mirror first hit
(roughness below `ptMirrorRoughness`, 0.05). Sampling a light's surface all but never
lands in a lobe that narrow, so that hit takes its specular light from the ray, which
returns the mesh's radiance, and its light samples drop their GGX term: the light's
reflection is counted once, and it is the mesh.

{{cite shaders/rt/pt_raygen_offline.rgen "bounce == 1u && firstHitMirror ? kRayMaskCamera : kRayMaskScene, 0, 0, 0,"}}

:::key
A new class of geometry that some rays must not see is a new bit in
`layout.json`, set by the `RenderScene`'s instance stage, and cleared from the
ray mask of the traces that must miss it. A constant nobody stamps into an
instance selects nothing.
:::

## A trace that trusts the traversal

The GI trace does not second-guess the cull mask. Its query is forced opaque, so
no candidate is ever handed back to the shader to accept or reject; once traversal
ends, it reads the committed instance's custom index (the instance slot) and loads
that instance's bounce albedo from the `RenderScene` instance table at binding 4:

{{cite shaders/rt/gi_trace.slang "const uint slot = q.CommittedInstanceID();"}}

There is no software re-implementation of the visibility test in it. Whether a
ray may reach an instance is decided entirely in traversal — an `AND` of the
trace's `kRayMaskScene` against the instance's mask — at no per-hit shader
cost, and a hit the traversal commits is always counted.

The consequence is that the mask is the only switch. A hidden instance is removed
by its zero mask alone; there is no second, per-instance flag in the albedo data
that could contradict it.

## Four interfaces, two implementations

`render_technique.hpp` is the other half of this unit: a pluggable-technique
contract for the hybrid passes. Each feature gets an `Input` struct of GBuffer
views, what that feature needs besides (camera matrices for reflections and AO;
for shadows the scene's shadow lights, the sample seeds, the velocity view the filter
reprojects with, the camera position, the `RenderQuality` and the per-pixel light
cap; for GI the same seeds, velocity view, camera position and quality, plus the
scene's lights, the instance table the bounce albedo is read from and the
environment map) and a nullable `VkAccelerationStructureKHR`, an `Output` struct — one view for
shadows, an image plus its view for the other three — and a uniform lifecycle —
`init / resize / render / getOutput / destroy`, plus `getName()` and `needsRT()`
for introspection. Four interfaces are declared: shadows, GI, reflections, AO.

Two have implementations. `RTShadowTechnique` and `RTGITechnique` are the only
classes in the tree deriving from any of them; `IReflectionTechnique` and
`IAOTechnique` have zero implementers. The screen-space features that would fit
those two slots do exist, but on a different base — `SSRPass` and `SSAOPass`
both derive from `RenderPassBase`, the base the rest of the deferred pipeline
uses, and from none of the four interfaces:

{{cite ohao/render/deferred/ssao_pass.hpp "class SSAOPass : public RenderPassBase"}}

The `Output` doc comments say what the implementations allocate.
`ShadowOutput::visibility` is eight 8-bit channels in an RG32UI image —

{{cite ohao/render/rt/render_technique.hpp "// x holds lights 0-3, y lights 4-7, the lowest byte first."}}

{{cite ohao/render/rt/rt_shadow_technique.cpp "constexpr VkFormat kVisFormat = VK_FORMAT_R32G32_UINT;"}}

— and `GIOutput::indirectLight` is a full-resolution RGBA16F image of the incoming
radiance (the receiver's albedo not applied) with the environment's visibility in alpha:

{{cite ohao/render/rt/render_technique.hpp "VkImage indirectLight;         // RGBA16F, full resolution: rgb incoming radiance (no albedo), a the environment's visibility"}}

{{cite ohao/render/rt/rt_gi_technique.cpp "constexpr VkFormat kRadianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;"}}

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

## The handover the graph does not see

The RT techniques record into the render graph as compute passes, but their
outputs reach the deferred lighting pass through a CPU-side member write, not a
graph edge. Before the graph executes, the renderer clears both inputs, so
lighting falls back to CSM shadows and no indirect light unless something hands
them over:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_lightingPass->setRTShadowMask(VK_NULL_HANDLE);"}}

Each RT pass's record callback then hands its view over — but only if its
`render()` actually dispatched. For shadows the handover is one lambda, called from
the trace pass in reference quality (the trace writes the output itself) and from
`rt.shadows.filter` in realtime, after the filter has written it:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (!m_lightingPass || !m_rtShadow->rendered()) return;"}}

GI follows the same shape: its lambda runs from `rt.gi` in reference quality and
from `rt.gi.denoise` in realtime, after the upsample has written the output.

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_lightingPass && m_rtGI->rendered())"}}

The lighting pass reads those members at its own record time, when it rewrites
this frame slot's descriptor set and picks its flag bits: RT visibility if it
arrived, CSM otherwise. So a frame samples exactly what that frame traced, and
any image a technique reallocates is re-bound on the next frame because the
handover repeats every frame — which the shadow technique relies on, since its
output alternates between the two halves of a ping-pong pair from frame to frame.

What makes it work is ordering, not tracking. The graph records passes in the
order they were added:

{{cite ohao/render/graph/render_graph.cpp "// Simple linear ordering for now (passes are added in correct order)"}}

and `rt.shadows`, `rt.shadows.filter`, `rt.gi` and `rt.gi.denoise` are added before `lighting`.
Register a technique after the lighting pass, or give the graph a scheduler that
reorders passes, and lighting records before the handover and silently takes the
fallback.

The resize path forwards the new extent to both RT techniques, because each owns a
screen-sized target:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_rtGI) m_rtGI->resize(width, height);"}}
{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_rtShadow) m_rtShadow->resize(width, height);"}}

`RTShadowTechnique::resize` destroys its screen-sized images — both halves of the
meta and visibility pairs, and the reference means — and recreates the pairs at the
new extent (the means come back on first use), dropping the temporal history:

{{cite ohao/render/rt/rt_shadow_technique.cpp "void RTShadowTechnique::resize(uint32_t width, uint32_t height) {"}}

That is what keeps the images registered with the trace's dispatch grid, which takes
its extent from `ShadowInput`, set from the renderer's current `m_width`/`m_height`:

{{cite ohao/render/rt/rt_shadow_technique.cpp "vkCmdDispatch(cmd, (input.width + 7) / 8, (input.height + 7) / 8, 1);"}}

A technique left at its `init()` extent would launch more threads than the images
have texels after the window grows; the out-of-range stores are discarded, and the
lighting pass, which fetches the visibility at `gl_FragCoord` rather than sampling
it, reads outside the image for every pixel past the old extent. The per-frame
handover keeps the handle valid — it cannot fix the resolution.

## Contracts

- Instance mask and ray mask must be non-disjoint or the instance is invisible: `(m_r & m_i) != 0`. A hidden or released instance carries 0 and is invisible to every ray.
- Instance masks are written once, by the `RenderScene`, into the one TLAS both the deferred renderer and the path tracer consume. A mask change is not scoped to the hybrid passes.
- Scene instances carry `kTlasMaskScene`, light emitter meshes `kTlasMaskEmitter`. Only the path tracer's camera ray and a near-mirror first hit's specular ray pass `kRayMaskCamera`; every other trace passes `kRayMaskScene`, so no shadow, GI, bounce or other specular ray ever hits an emitter mesh. The mirror's light samples drop their GGX term to match. A new trace that passed the camera mask would shadow lights by their own meshes and count them twice.
- The masks live in `shaders/shared/layout.json` alone; the C++, GLSL and Slang copies are generated from it (`tools/gen_layout.py`).
- `rt/gi_trace.slang` filters nothing: the cull is the ray mask against the instance mask alone, and a hit reads `giAlbedoPacked.rgb` of whatever slot traversal commits.
- Technique outputs reach the lighting pass only through the per-frame handover in each RT pass's callback, and only when `rendered()` is true. The graph does not track this dependency; it holds because the RT passes are added before `lighting` and the graph records in insertion order.
- `DeferredRenderer::onResize` must forward the new extent to every RT technique it owns. It forwards to both `m_rtGI` and `m_rtShadow`; a technique added without a forward keeps its `init()` extent while the dispatch grid follows the window.
- `init()` failure is non-fatal by design — the renderer resets the `unique_ptr` and continues on the CSM path, so a null `m_rtShadow` is a valid steady state, not an error.
