---
module: graph
id: particles
title: Particle system
standard: v2
figures: [graph-particles-dataflow]
---

## One pool, two dispatches, one indirect draw

Sparks and smoke are the one class of geometry the CPU must not touch per-item:
the count changes every frame and they die faster than a readback round-trip.
OHAO answers with a fixed device-local pool — 65,536 records of four `vec4`s,
4 MiB, allocated once and never resized. Position carries current age in `.w`,
velocity carries total lifetime in `.w`, then RGBA colour, then a params vector of
size, rotation, type and an alive flag.

{{cite ohao/render/particles/particle_system.cpp "VkDeviceSize particleSize = sizeof(GPUParticle) * m_maxParticles;"}}

Two four-word buffers ride alongside — a counter block and a
`VkDrawIndirectCommand` — both host-visible and coherent, so the CPU seeds them
without a staging copy. A frame records one `emit` dispatch per queued emitter
(64 threads per group, one per requested particle), a compute→compute barrier,
then one `update` dispatch that sweeps the *entire* pool regardless of how many
particles are alive:

{{cite ohao/render/particles/particle_system.cpp "uint32_t groupCount = (m_maxParticles + 255) / 256;"}}

:::why
The alternative is the textbook alive-list/dead-list pair driven by
`vkCmdDispatchIndirect`, where the update dispatch is sized from a GPU-resident
count. That costs a compaction pass, an extra barrier, and makes the dispatch
depend on device memory. OHAO pays a flat 65,536-lane sweep of mostly-early-out
threads instead. A device-resident count does not disappear — `update` still
builds `instanceCount` atomically and the draw reads it back through a
`DRAW_INDIRECT` barrier — but only the *draw* waits on it, never a dispatch, so
there is no compaction pass. The trade stops being right an order of magnitude
up.
:::

## The allocator that only counts up

Emission allocates by bump. Each thread atomically increments `aliveCount` and
treats the old value as its slot; the shader's comment says "find a dead particle
slot" but there is no free list to search.

{{cite shaders/particles/particle_emit.comp "uint slot = atomicAdd(aliveCount, 1);"}}

An out-of-range thread gives its slot back by adding `-1u` and returns; nothing
else ever lowers the counter. `update` marks expired particles dead and increments
`deadCount`, which no shader and no C++ line reads. The counter block is written
exactly once, at buffer creation — the only per-frame reset is the indirect
command.

`aliveCount` is therefore not a population but a monotone high-water mark of every
particle ever emitted, and dead slots are never reclaimed. After 65,536 cumulative
emissions — 512 explosions at 128 each — the bounds check fails for every emit
thread and the system stops spawning for the life of the process. That is a
data-structure gap, not a tuning one: recycling needs a free list or a compaction
pass, and the three-buffer layout has nowhere to put one.

## Uniform in angle is not uniform in solid angle

Emission direction is a cone sample around the configured axis. The shader draws
uniform variates $\xi_1,\xi_2$, uses the first for an azimuth, and feeds the
second *directly* to the polar angle:

{{cite shaders/particles/particle_emit.comp "float phi = r2 * spread;"}}

then tilts the axis by that angle against an orthonormal in-plane pair:

{{cite shaders/particles/particle_emit.comp "vec3 randomDir = dir * cos(phi)"}}

with $\theta = 2\pi\xi_1$, $\phi = \xi_2\phi_{\max}$, and $\phi_{\max}$ =
`ParticleEmitterConfig::spreadAngle` in radians. The rotation is exact:
$\mathbf{d}\cos\phi + (\mathbf{r}\cos\theta + \mathbf{u}\sin\theta)\sin\phi$ tilts
$\phi$ off the axis $\mathbf{d}$. The density is not. A solid-angle element is
$d\omega = \sin\phi\,d\phi\,d\theta$, so uniform $\phi$ gives a directional
density

$$p(\omega) = \frac{p(\phi)}{2\pi\sin\phi} = \frac{1}{2\pi\,\phi_{\max}\,\sin\phi}$$

which diverges as $\phi \to 0$ and again as $\phi \to \pi$. Uniform coverage of
the cone instead requires inverting the cosine,

$$\cos\phi = 1 - \xi_2\,\bigl(1 - \cos\phi_{\max}\bigr)$$

For a narrow cone the bias is invisible — muzzle flash's $\phi_{\max} = 0.3$ rad
just gets a hot core. The preset that suffers asks for a full sphere:

{{cite ohao/render/particles/particle_system.cpp "config.spreadAngle = 3.14159f;"}}

At $\phi_{\max} = \pi$ the $1/\sin\phi$ weighting piles particles onto both poles
of the $+Y$ emission axis and starves the equator, so a "spherical" burst reads as
a vertical dumbbell. The fix is the one line above.

## What the presets ask for, and what the GPU does

The preset table is authored with real intent: muzzle flash gets zero gravity and
drag 5.0 so it stops dead in the air, smoke gets gravity −0.5 for buoyancy, water
splash −6.0 under the comment "arcs up then falls". None of it reaches a shader.
`update()` pushes two literals:

{{cite ohao/render/particles/particle_system.cpp "pc.gravity = 9.81f;"}}

One of those presets would also miss its comment even if the value did arrive.
The integrator *subtracts* the pushed gravity from `velocity.y`, so a negative
`gravity` is a constant upward acceleration held for the particle's whole life:

{{cite shaders/particles/particle_update.comp "p.velocity.y -= update.gravity * update.deltaTime;"}}

Smoke's −0.5 reads as the intended buoyancy. Water splash's −6.0 against drag 0.5
accelerates droplets upward for their entire lifetime, toward a positive terminal
velocity — the inverse of the arc its comment describes, not a tuning error.

{{cite ohao/render/particles/particle_system.cpp "config.gravity     = -6.0f;"}}

Every particle therefore falls at 9.81 with drag 0.1 — a structural consequence of
the single global update dispatch, where one push-constant block covers all 65,536
particles and per-emitter forces have nowhere to live. The particle *type* is
stored in `params.z` precisely so the update shader could branch on it, and the
update shader never reads it. `colorEnd` and `endSize` share that fate: packed
into the emit push constants, read by nothing, so the colour ramp is a hardcoded
alpha fade and the size ramp a hardcoded shrink.

{{figure graph-particles-dataflow "Which stage writes and which stage reads each field, read statically out of particle_system.cpp and the four shaders in shaders/particles/ — conceptual, not a captured GPU trace. Amber rows are written every frame and read by nobody."}}

That shrink has a defect of its own. It names its local `startSize` but reads the
*current* size back out of `params.x` and rescales it in place:

{{cite shaders/particles/particle_update.comp "p.params.x = startSize * (1.0 - lifeRatio * 0.5);"}}

so the reduction compounds geometrically across frames instead of interpolating
from the emitted size, and the result is frame-rate dependent — the same particle
simulated at 120 fps ends up dramatically smaller than at 60 fps.

Every emit queued in a frame is also handed the same `m_totalTime`, and the emit
shader seeds its hash from `(time, index)` alone.

{{cite ohao/render/deferred/deferred_renderer.cpp "m_particleSystem->emit(cmd, m_pendingEmits.front(), m_totalTime);"}}

Two effects spawned on one frame with equal particle counts therefore get
identical random directions, speeds and lifetimes.

## One instance a slot, up to the highest live one

`update` finishes by raising the draw command's instance count to one past the
highest slot still alive, one atomic per alive lane:

{{cite shaders/particles/particle_update.comp "atomicMax(instanceCount, idx + 1u);"}}

A maximum, unlike a count, does not depend on the order the lanes run in, so the
same pool gives the same draw on every run. The CPU pre-seeds that same struct
each frame with a fixed six-vertex quad and a zero instance count:

{{cite ohao/render/particles/particle_system.cpp "indirect[0] = 6; // vertexCount"}}

No vertex buffer is bound at all — the corners are a `const vec2[6]` in the vertex
shader, expanded into a world-space quad by the two camera basis vectors pushed as
constants. So the GPU is told: six vertices, N instances, and the vertex shader
takes its particle from the instance index and its corner from the vertex index:

{{cite shaders/particles/particle_render.vert "uint particleIndex = gl_InstanceIndex;"}}

The draw covers every slot below the highest live one, dead or alive. A dead slot
in that range is drawn as a degenerate point behind the near plane and costs six
vertex invocations. Because emission only bumps, the range is in practice every
slot ever emitted that has a live particle above it.

The alive flag has to mean something in slots no emit has touched yet, and
device-local memory starts as whatever was there. The pool is therefore zeroed on
the GPU once, before the first emit or update records:

{{cite ohao/render/particles/particle_system.cpp "vkCmdFillBuffer(cmd, m_particleBuffer, 0, VK_WHOLE_SIZE, 0u);"}}

## Additive, depth-tested, never depth-written

All seven particle types share one blend state: source alpha in, destination
`ONE`.

{{cite ohao/render/particles/particle_system.cpp "blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;"}}

Depth is tested against the GBuffer so particles are occluded by solid geometry,
but never written, so they never occlude each other.

{{cite ohao/render/particles/particle_system.cpp "depthStencil.depthWriteEnable = VK_FALSE;"}}

:::why
Additive compositing is commutative in RGB, so the colour is identical for any
draw order and the engine never sorts billboards. The alpha channel is not:
`srcAlphaBlendFactor` is `ONE` against `dstAlphaBlendFactor` `ZERO`, which
replaces destination alpha with whichever fragment landed last.

{{cite ohao/render/particles/particle_system.cpp "blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;"}}

The rejected alternative — a per-frame
depth sort of the pool, or an order-independent transparency scheme — means a GPU
sort or extra targets for a subsystem that mostly draws sparks. The price is that
nothing can darken: the SMOKE preset's grey *adds* grey light to the HDR buffer
rather than occluding what is behind it, so a smoke plume brightens the scene.
Real smoke needs a second pipeline with `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` and a
sort.
:::

## Drawn into the lit image before SSS and SSR read it

Particles are stage 4.6: after lighting and sky, before SSS, SSR and
post-processing. Their framebuffer's colour attachment is the lighting pass's
output view:

{{cite ohao/render/deferred/deferred_renderer.cpp "VkImageView hdrView = m_lightingPass->getOutputView();"}}

That is also the view post-processing samples: SSS blurs its skin tiles in place, so
the one call to `setHDRInputWithImage` passes the lighting output itself:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_postProcessing->setHDRInputWithImage(m_lightingPass->getOutputView(), m_lightingPass->getOutputImage());"}}

The billboards therefore reach the frame whatever runs after them. The order still
matters for the two passes that *read* the lit image: SSS's taps and SSR's hits see
the particles only because they run after the particle pass has drawn. Between the two
sits a memory barrier from colour-attachment writes to compute and fragment reads,
since the particle render pass's own dependencies do not reach the compute
dispatches:

{{cite ohao/render/deferred/deferred_renderer.cpp "toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;"}}

The pass does not run every frame. The renderer records it only while a particle
can be alive — from a queued emit until that emit's time plus its `maxLifetime` —
so a scene that never emits pays for no dispatch and no draw:

{{cite ohao/render/deferred/deferred_renderer.cpp "const bool particlesLive = !m_pendingEmits.empty() || m_totalTime <= m_particlesLiveUntil;"}}

The clock it compares against, `m_totalTime`, advances by the frame's delta on
every frame whether or not the pass runs.

The render pass itself loads both attachments rather than clearing them, and
declares the colour attachment's initial *and* final layouts as
`SHADER_READ_ONLY_OPTIMAL` rather than `COLOR_ATTACHMENT_OPTIMAL`, matching what
the lighting pass left behind.

{{cite ohao/render/deferred/deferred_renderer.cpp "colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;"}}

The depth attachment is the GBuffer's, and it gets the same care. Particles test
against it but never write it, so the render pass keeps it in
`DEPTH_STENCIL_READ_ONLY_OPTIMAL` at both ends and in the subpass reference:

{{cite ohao/render/deferred/deferred_renderer.cpp "depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;"}}

That is the layout the GBuffer pass resolves depth to, the one the render graph
declares for it, and the one TAA samples it in after this pass. Flipping it to an
attachment layout here would leave TAA reading depth in a layout its descriptor
does not declare; read-only depth testing needs no transition at all.

{{cite ohao/render/deferred/gbuffer_pass.cpp "attachments[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;"}}

The framebuffer holds the lighting output view and the GBuffer depth view
directly, so it is rebuilt on every resize. Failure is handled unevenly. Only a
failed `ParticleSystem::initialize` nulls the system; a failed render pass,
framebuffer or render pipeline logs and leaves it alive.

{{cite ohao/render/deferred/deferred_renderer.cpp "Particle render pass/framebuffer failed (non-fatal)"}}

In that state `emit` and `update` keep dispatching on every frame a particle is
live — burning the compute and the monotone `aliveCount` budget — while `render()`
early-outs on the null pipeline. Nothing is ever drawn, and nothing says so after
the one startup line.

:::key
The pool is a bump allocator that never recycles. The render path draws one
instance per slot up to the highest live one and lands in the image SSS copies, so
what is emitted reaches the screen; but until slots are reclaimed the system is a
one-shot 65,536-particle budget for the process.
:::

## Contracts

- `update()` must be recorded after every `emit()` in the same frame: it is the only writer of `instanceCount` and it zeroes the indirect command from the host at record time, so a later emit contributes nothing to that frame's draw.
- The indirect buffer is a single host-visible allocation rewritten through `vkMapMemory` during command recording, not by a GPU transfer — and the deferred path already runs three frames in flight, waiting only on frame *N*−3 before it records. Nothing synchronizes that host write against the two earlier submissions, which may still be reading the same buffer for their `vkCmdDrawIndirect`. The hazard is live today, not one that adding frames-in-flight would introduce; fixing it needs per-frame indirect buffers.
- The framebuffer aliases `DeferredLightingPass::getOutputView()` and `GBufferPass::getDepthView()`; recreating either pass invalidates it, and the resize path must rebuild it or the render pass binds a destroyed view.
- `emit` push constants total 112 bytes and `render` 96 — both inside the 128-byte floor Vulkan guarantees for `maxPushConstantsSize`. The emit block has exactly one `vec4` of headroom on a minimum-spec device; a second crosses it.
