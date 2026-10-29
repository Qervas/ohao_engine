---
module: hybrid
id: shadow-technique
title: RT shadow technique
standard: v2
---

## The one term the GBuffer cannot hold

A deferred GBuffer stores everything the lighting equation needs at a point —
position, normal, albedo, roughness, metallic — except the one quantity that is
not a property of the point at all. Visibility depends on the whole scene along a
ray. Cascade shadow maps answer it by re-rasterising depth from the light, at a
pass per cascade and the cost of projective aliasing, cascade seams and a bias
nobody gets right. `RTShadowTechnique` asks the TLAS instead: no light-space
projection, no cascade split, no depth-map resolution to alias against. It has a
bias of its own and it cannot see an alpha mask — both below.

The result is a screen-resolution `R32G32_UINT` image holding eight 8-bit
visibility channels, one per shadow light (x: lights 0-3, y: lights 4-7, lowest
byte first), 0 = shadowed, 255 = lit:

{{cite ohao/render/rt/rt_shadow_technique.cpp "constexpr VkFormat kVisFormat = VK_FORMAT_R32G32_UINT;"}}

The deferred lighting fragment shader reads it once, at binding 14, before its
light loop — an integer `Load` at the fragment's own pixel, since the image is the
lighting pass's size — into the `LightVisibility` it hands the shared light loop
(`includes/lighting/clustered_lights.slang`'s `directLight`):

{{cite shaders/core/deferred_lighting.slang "vis.rtWords = vis.rt ? rtShadowVisibility.Load(int3(int2(fragCoord.xy), 0)).xy : uint2(0u);"}}

Inside the loop each light picks its own byte by the slot stored in its
`params.w`, and the BSDF result is multiplied by it over 255.

{{cite shaders/includes/lighting/clustered_lights.slang "shadow = float((word >> (8u * uint(slot & 3))) & 255u) * (1.0 / 255.0);"}}

Binding the view is also what switches the pipeline. `setRTShadowMask` only stores
it; `execute` zeroes the whole flag word and rebuilds it from the bound views every
frame, before pushing it.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags = 0;"}}

In that rebuild the RT bit and the CSM bit are the two arms of a single if/else
keyed on whether an RT visibility view is bound at all, so the two shadow systems
are mutually exclusive by construction rather than by convention.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "else if (m_shadowMapView != VK_NULL_HANDLE)"}}

The shaders repeat the structure. The lighting shader samples the cascades only
when bit 5 is clear and bit 2 is set, and the light loop takes the RT channel when
`vis.rt` is set and reaches the CSM value only through the `else`, for a directional
light.

{{cite shaders/core/deferred_lighting.slang "vis.directional = !vis.rt && (pc.flags & 4u) != 0u ? shadowCSM(P, viewDepth) : 1.0;"}}

{{cite shaders/includes/lighting/clustered_lights.slang "shadow = vis.directional;"}}

The renderer goes one step further and does not draw the cascades at all in a
frame that traces: whether RT shadows run is decided before the graph is built, and
the `csm` pass is added only when they do not.

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_csmPass && !rtShadows) {"}}

## Which lights get a channel

The light table decides. Walking the raster lights in light order, it gives each
light whose `castShadows` is set the next channel and records it in `params.w`:

{{cite ohao/render/scene/light_table.cpp "shadowSlot = static_cast<float>(m_shadowLights.size());"}}

There are eight channels and the raster light table holds up to 65,536 lights, so
only the first eight casters get one:

{{cite ohao/render/scene/render_scene_types.hpp "inline constexpr uint32_t kMaxShadowLights = 8;"}}

A caster past the eighth keeps `kShadowNever` and renders unshadowed. The table counts
those, and `RenderScene::shadowCastersWithoutChannel()` reports the count; test 8
checks that Cornell's twelve casting lights give eight channels and four unshadowed
lights.

{{cite ohao/render/scene/light_table.cpp "++m_shadowCastersWithoutChannel;"}}

A light with `castShadows` off gets `kShadowNever` (-1), and so does every emissive
proxy, which sits inside its own mesh.

{{cite ohao/render/scene/light_table.cpp "light.params = glm::vec4(0.0f, 0.0f, 0.0f, kShadowNever);   // inside its own mesh: never shadowed; a point"}}

The shared light loop reads the two cases directly. A negative slot is never
occluded; any other slot takes its channel when the RT bit is set, and without it
a directional light gets CSM.

{{cite shaders/includes/lighting/clustered_lights.slang "const int slot = int(round(light.params.w));"}}

The lights with a channel, in channel order, are `RenderScene::shadowLights()`,
which the dispatch hands to the deferred renderer each frame (none for a scene
with no lights). RT shadows run only when
that list is non-empty, the device has ray queries and the scene has a TLAS: a
frame in which no light casts shadows traces nothing.

{{cite ohao/render/deferred/deferred_renderer.cpp "const bool rtShadows = m_useRTShadows && m_rtShadow && sceneTlas != VK_NULL_HANDLE && m_gbufferPass &&"}}

## Two compute passes, no ray tracing pipeline

The technique is two compute passes, timed under their registry names: the trace,
`rt.shadows`, and in realtime quality the filter, `rt.shadows.filter`. Neither is a
ray tracing pipeline. The trace uses inline ray queries — no shader binding table,
no raygen, miss or hit shaders — because a shadow ray needs no hit shader: it asks
one yes/no question. Without `VK_KHR_ray_query` `init()` fails and CSM shadows the
frame.

{{cite ohao/render/rt/rt_shadow_technique.hpp "Both passes are compute: the trace uses inline ray queries (VK_KHR_ray_query; without it"}}

Both are Slang, one entry point each (`trace` in `rt/shadow_trace.slang`, `filter` in
`rt/shadow_filter.slang`), loaded by name into plain compute pipelines through the
helper RT shadows and RT GI share in `rt_compute.cpp`:

{{cite ohao/render/rt/rt_shadow_technique.cpp "rtc::createComputePipeline(m_device, "rt_shadow_trace.slang.spv", "trace""}}

{{cite ohao/render/rt/rt_compute.cpp "ci.stage.pName = entry;"}}

The query stops at the first hit it accepts; the answer is whether anything was
committed. A candidate from an alpha-tested or blended instance is committed only where
`rt/alpha_test.slang` says it is hit; every other triangle is opaque.

{{cite shaders/rt/shadow_trace.slang "RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;"}}

{{cite shaders/rt/shadow_trace.slang "return rand01(pixelKey, l, s, 2u) < T ? 1.0 : 0.0;"}}

:::why
The rejected alternative is a closest-hit query reporting hit distance — the
channel an NRD-style denoiser wants for sizing its spatial filter. Accepting the
first hit gives it up: traversal stops at any intersection rather than proving the
nearest one. The filter makes up for it with guides it does have, the pixel's
distance from the camera and its normal; what it cannot do is widen the kernel
where the occluder is far, so penumbra width comes from the light's sampled area,
not from the filter.
:::

The lights reach the trace in a storage buffer, not push constants: each frame slot
owns a host-visible buffer holding up to eight `ShadowLight`s (the struct comes
from the shared layout) and, behind them, a counter. It is an `rtc::HostBuffer`,
device-local memory when the device has a host-visible kind of it (resizable BAR),
because every pixel reads every light; RT GI's light buffers are made the same way.

{{cite ohao/render/rt/rt_compute.cpp "ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);"}}

## Four lights a pixel, by importance

In realtime quality a pixel does not trace every light. It ranks the frame's
shadow-casting lights by what each is worth to it — intensity over squared
distance, a directional light its plain intensity:

{{cite shaders/rt/shadow_trace.slang "return intensity / max(d2, 1e-4);"}}

A light is worth 0 where it cannot contribute: a point or spot light beyond its
range plus its radius, or one whose whole sphere lies behind the surface. A
rectangle is pushed with a range of 0, which the test reads as unlimited. The
importance has no cone term, so a spot light competes for a pixel outside its cone
as if it were a point light.

{{cite shaders/rt/shadow_trace.slang "if (type == 1u && range > radius && d2 > range * range) return 0.0;"}}

The ranking is a sort key: the importance's float bits with the light index in the
low bits, so ties go to the lower index, and bit 3 is set so a key is never 0 for a
light worth anything.

{{cite shaders/rt/shadow_trace.slang "key[l] = imp > 0.0 ? (asuint(imp) & ~15u) | 8u | (7u - l) : 0u;"}}

The pixel finds the budget-th largest key and traces one ray to a sampled point on
each light at or above it. The budget is `ShadowInput::maxLightsPerPixel`, clamped
to `kShadowLightsPerPixel`, four:

{{cite ohao/render/rt/rt_shadow_technique.cpp "std::min(input.maxLightsPerPixel, shared::kShadowLightsPerPixel)"}}

A pixel with more lights worth tracing than it may trace adds one to the counter:

{{cite shaders/rt/shadow_trace.slang "if (wanted > budget) InterlockedAdd(dropped[0], 1u);"}}

The counter is read on the CPU only after that slot's fence has signalled, never
by waiting, and the newest finished frame's count is what `render.stats` reports as
`shadowLightsDropped`:

{{cite ohao/render/rt/rt_shadow_technique.cpp "if (!m_latestDropped || s.frame >= m_latestDropped->frame)"}}

{{cite ohao/runtime/frame_capture.cpp "st->shadowLightsDropped = dropped->pixels;"}}

The trace writes no visibility in realtime. It writes a packed `meta` texel per
pixel: in x, the filter's guide — the distance from the camera as a bfloat16 (a
float's top 16 bits) and the octahedral normal at 8 + 8 bits; in y, a traced bit
and a visible bit per light and a surface bit.

{{cite shaders/rt/shadow_trace.slang "meta[pixel] = uint2(dist | (oct.x << 16u) | (oct.y << 24u), traced | (visible << 8u) | (1u << 16u));"}}

## What the estimator actually computes

Direct lighting from an area source is

$$L_o(x,\omega_o) = \int_{\Omega_\ell} f_r(x,\omega_o,\omega)\, L_i(\omega)\, V(x,\omega)\, (\mathbf{n}\!\cdot\!\omega)\, d\omega$$

where $\Omega_\ell$ is the solid angle the light subtends at the shading point
$x$, $f_r$ is the BRDF, $L_i$ the incident radiance, $\mathbf{n}$ the shading
normal, and $V \in \{0,1\}$ the binary visibility along $\omega$. One channel per
light cannot represent that integral. What the technique evaluates is the
separable approximation

$$L_o \approx \bar V(x)\; f_r(x,\omega_o,\bar\omega)\, L_i(\bar\omega)\,(\mathbf{n}\!\cdot\!\bar\omega),
\qquad \bar V(x)=\frac{1}{N}\sum_{s=1}^{N} V(x,\omega_s)$$

where $\bar\omega$ is the direction the lighting pass uses for the light. In
reference quality $\bar V$ is exactly that mean: eight rays a light a frame,
averaged, then folded into a running mean over the reference frames since the last
temporal reset, so $N$ grows by eight every frame. $k$ is the technique's count of
the frames already in the mean (`history.x`), which a reset, new images or any
realtime frame zero, so the mean never blends into a stale or undefined image.

{{cite shaders/rt/shadow_trace.slang "const float4 meanLo = prevLo + (lo - prevLo) / (k + 1.0);"}}

In realtime each pixel contributes one binary sample a light a frame, and $\bar V$
is a weighted mean over neighbouring pixels and past frames instead (the filter,
below). Either way the lighting pass multiplies that single number by a BRDF
evaluated only at $\bar\omega$. Factoring $V$ out is exact wherever it is constant
across the light — everywhere but the penumbra; inside one, the error is the
covariance between visibility and the cosine-weighted BRDF over the source, the
term a path tracer keeps and this one discards.

## The filter

The filter turns one ray a light a pixel into the eight channels lighting reads.
It runs in 16×16 workgroups, and each group first decodes a 20×20 tile of `meta` —
its pixels plus a two-pixel apron — into shared memory, so every 5×5 neighbourhood
is read from there:

{{cite shaders/rt/shadow_filter.slang "groupshared uint gBits[kSide * kSide];"}}

Most pixels never run the weighted kernel. Over the 5×5, a light every
neighbour that traced it saw lit is lit, and one every such neighbour saw occluded
is occluded, whatever the weights; only a light seen both ways — a penumbra, or
noise — is "mixed":

{{cite shaders/rt/shadow_filter.slang "const uint mixed = anyLit & anyHidden;"}}

A pixel traces at most four lights, so at most four are mixed, and one `float4`
accumulates them. A neighbour's weight falls with its ring (1, 0.6, 0.3), with its
depth difference relative to 5 % of the centre's distance, and with the fourth power
of the normals' cosine; neighbours that did not trace the light, and sky, do not
count.

{{cite shaders/rt/shadow_filter.slang "float w = ring == 0 ? 1.0 : ring == 1 ? 0.6 : 0.3;"}}

{{cite shaders/rt/shadow_filter.slang "const float invTol = 1.0 / (0.05 * gc.w + 1e-3);"}}

Then the temporal step. The pixel is reprojected by the G-buffer's velocity to the
nearest texel of last frame, and last frame's output is blended in only where that
texel is a surface, within 10 % of this pixel's distance, with normals within
$\cos^{-1} 0.9$, and only for lights both pixels traced:

{{cite shaders/rt/shadow_filter.slang "if (isSurface(pm) && abs(gp.w - gc.w) < 0.1 * gc.w && dot(gp.xyz, gc.xyz) > 0.9)"}}

The blend weight is $1/(n+1)$ for $n$ frames of history, floored at
$1/16$ (`kShadowHistoryFrames`): a still camera after a reset averages its frames
evenly, then settles into a moving average.

{{cite shaders/rt/shadow_filter.slang "const float alpha = 1.0 / float(min(pc.extent.w + 1u, kShadowHistoryFrames));"}}

{{cite ohao/render/rt/rt_shadow_technique.cpp "m_historyFrames = m_historyValid ? m_historyFrames + 1 : 1;"}}

The output is quantised to 8 bits with a hash dither in place of rounding, so the
quantisation error averages out over frames instead of biasing the history; 0 and 1
stay exact.

{{cite shaders/rt/shadow_filter.slang "const uint4 q = uint4(min(saturate(v) * 255.0 + dither, 255.0));"}}

The output is also next frame's history, so the meta and visibility images are
ping-pong pairs, this frame writing one half while reading the other. History is
dropped on the first frame after a temporal reset, on a resize, and by any frame
rendered in reference quality.

{{cite ohao/render/rt/rt_shadow_technique.cpp "if (m_frameIndex == 0) m_historyValid = false;"}}

## Reference quality

`RenderQuality::Reference` is the converged answer the realtime frame
approximates. The same trace pass runs with no per-pixel cap and no filter: every
light, `kShadowReferenceRays` (eight) rays each, every frame.

{{cite ohao/render/rt/rt_shadow_technique.cpp "reference ? shared::kShadowReferenceRays : 1u"}}

{{cite shaders/rt/shadow_trace.slang "v /= float(kShadowReferenceRays);"}}

The running mean lives in two `RGBA32F` images (lights 0-3 and 4-7), created on
first use; in realtime a 1×1 stand-in is bound in their place. The trace writes the
mean, packed to 8 bits a light with round-to-nearest, straight to the visibility
image, and the renderer adds no filter pass:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_renderGraph.addComputePass("rt.shadows.filter","}}

## Three penumbra samplers

Softness comes from jittering the sample target, and the trace has a geometry per
light type. A directional light (type 0) gets a tangent basis around the light
direction and an offset inside a disk of the light's angular radius, with
$\sqrt{r_1}$ making samples uniform in disk area — a small-angle construction,
since the offset is the angle rather than its tangent:

{{cite shaders/rt/shadow_trace.slang "const float angle = light.edge1AndRadius.w * sqrt(r1);"}}

A point or spot light (type 1) instead targets a uniformly distributed point on a
sphere of the light's radius:

{{cite shaders/rt/shadow_trace.slang "const float z = 1.0 - 2.0 * r2;"}}

A rectangle (type 2) is sampled uniformly over its area, from the corner its
position names along its two edges — the convention the path tracer samples it
by:

{{cite shaders/rt/shadow_trace.slang "target = light.positionAndType.xyz + light.edge1AndRadius.xyz * r1 + light.edge2AndRange.xyz * r2;"}}

Because $z = 1 - 2r_2$ is uniform on $[-1,1]$, the sphere branch is a correct
uniform-area sampler of the sphere *surface* — with a consequence worth naming. By
Archimedes' hat-box theorem a uniform distribution on a sphere projects to a
uniform distribution on any single axis, so an occluder edge sweeping across the
source yields a **linear** visibility ramp, not the circular-segment S-curve a
genuinely disk-shaped source produces. The difference shows at the two ends of the
penumbra, where the true profile approaches full light and full shadow tangentially
and the linear one arrives with a crease. Reference quality converges to that ramp;
realtime approaches it only as far as its spatial and temporal means reach.

## Rays and seeds

Realtime cost scales with the lights a pixel traces: one ray each, at most four a
pixel, whatever the light count. Reference costs eight rays per light per pixel per
frame. The push constants carry the extent, the light count and the budget, then
the seeds, the quality and the rays per light:

{{cite ohao/render/rt/rt_shadow_technique.cpp "pc.extent = u4(input.width, input.height, m_lightCount,"}}

Measured on `cornell_720p` (1280×720, RTX 5070 Laptop, 2026-10-24), with eight
shadowed lights and four traced a pixel: `rt.shadows` 0.252 ms and
`rt.shadows.filter` 0.061 ms, within budgets of 0.30 and 0.08 — moved there from
SSR, the G-buffer and sky + particles by Frank's ruling of 2026-10-25, the frame's
1.50 ms unchanged, after the original 0.20 and 0.05 proved out of reach with four rays
a pixel. On `helmet_game_720p`, four lights: 0.092 ms and 0.045 ms.

Every random number is an integer PCG hash of the render seed, the frame index
since the last temporal reset and the pixel, hashed once per pixel, then of the
light, the ray and the dimension:

{{cite shaders/rt/shadow_trace.slang "const uint h = pcg(pixelKey ^ pcg((light << 24u) ^ (s << 4u) ^ dim));"}}

The deferred renderer supplies both seeds, the one `setRenderSeed` stored and the
counter `resetTemporalState` zeroes, so the noise decorrelates from frame to frame —
which is what lets the filter's temporal mean converge — while two renders of the
same world with the same seed trace the same rays. The filter's dither hashes the
same seeds.

## Sharp edges

The ray origin is pushed a flat 0.05 world units along the shading normal, with
`TMin` = 0.001 behind that:

{{cite shaders/rt/shadow_trace.slang "const float3 origin = p + n * 0.05;"}}

The offset is absolute, not scaled by depth or texel footprint: too large for
centimetre-scale detail, where contact shadows detach into the ray-traced form of
peter-panning, and too small for a kilometre-scale scene.

Sky pixels are detected by testing all four channels of the GBuffer position for
exact zero:

{{cite shaders/rt/shadow_trace.slang "if (all(position == float4(0.0)) || position.w < 0.0)"}}

The fourth is not a coverage bit — GBuffer0 packs metallic there:

{{cite shaders/includes/scene/gbuffer_body.glsl "outGBuffer0 = vec4(fragWorldPos, metallic);"}}

The test works only because that attachment clears to (0,0,0,0); a non-metallic
surface passing exactly through the world origin is classified as sky and comes out
fully lit in every channel. A negative metallic is a light's emitter mesh (the G-buffer
writes -1 there): lit by nothing, it is skipped like sky, and its pixels no longer count
toward `shadowLightsDropped`.

A light a pixel did not trace — worth nothing there, or past the per-pixel cap —
is lit in that pixel's output. For the first that is free: the light contributes
nothing, so there is nothing to occlude. For the second it is the cost the cap
buys, and the dropped count is how big it is.

Samples whose jittered direction falls below the horizon count as zero
visibility:

{{cite shaders/rt/shadow_trace.slang "if (dot(n, dir) <= 0.0) return 0.0;"}}

Inside the integral that would cost nothing — those directions carry zero cosine
weight anyway. But a skipped sample still counts as a sample, so what reaches the
channel is a binary horizon *indicator* averaged over the jitter, and the lighting
pass multiplies it into OpenPBR's BSDF, whose `eval` returns the BSDF times exactly one
$\mathbf{n}\cdot\bar\omega$, at the unjittered direction:

{{cite shaders/includes/lighting/clustered_lights.slang "return s.bsdf.eval(s.wo, s.toLocal(L), t) * radiance;"}}

{{cite shaders/material/openpbr.slang "    // BSDF x cos(wi)."}}

The product is indicator × cosine: a step-shaped second falloff riding on the
cosine, darkening a band near the terminator by an amount no BSDF term accounts
for.

Finally the ray mask. The trace queries with `kRayMaskScene`; the `RenderScene`
builds every visible scene instance with `kTlasMaskScene`, a light's emitter mesh
with `kTlasMaskEmitter`, a first-person gun's mesh with `kTlasMaskViewModel`, and a
hidden or released slot with zero:

{{cite shaders/rt/shadow_trace.slang "q.TraceRayInline(scene, 0, kRayMaskScene, ray);"}}

{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceEmitter) != 0   ? gpu::shared::kTlasMaskEmitter"}}
{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceViewModel) != 0 ? gpu::shared::kTlasMaskViewModel"}}

so the query sees every visible scene instance and neither hidden ones, the
light meshes nor the gun. The light meshes matter: a light's emitter mesh surrounds
the light's own position, and a shadow ray aimed at the light would hit it and
shadow every pixel the light reaches (hybrid/visibility). The gun is drawn shrunk
toward the eye, away from where it would stand in the world, so a shadow it cast
would fall in the wrong place; it casts none.

:::key
Each channel is a boolean visibility average, not a shadow with radiance in it.
Everything the technique gets right — hardness, contact, penumbra width — is
decided by where the sample targets are placed; everything it gets wrong, at the
terminator and inside the penumbra, follows from that scalar being pulled out of an
integral it does not commute with. In realtime a fifth light that matters to a
pixel is not dimmed but dropped: lit, and counted.
:::

## Contracts

- `DeferredRenderer::onResize` forwards the new extent to `m_rtShadow` as well as `m_rtGI`. `resize` recreates both ping-pong pairs at the new extent and drops the history; the reference means come back on first use. The trace dispatches at `ShadowInput`'s extent, the filter at the technique's own, so the two must agree.
- Each pipeline's descriptor set is written at record time into the set for the caller's `frameSlot`, one of `MAX_FRAMES_IN_FLIGHT`, and the slot's light buffer and counter are rewritten in `render()`. Both hold only while the caller passes the frame-ring slot whose fence it has just waited on; the counter is read only for slots whose fence the caller reports signalled.
- The barrier before the trace discards this frame's meta and visibility halves (`oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`) — every pixel is written each dispatch — and waits on fragment and compute reads of them from an earlier frame.
- Light *i* of `ShadowInput::lights` is traced into channel *i*, and the lighting shader reads channel `params.w`. The two agree only because both come from one walk of the light table: `shadowLights()` lists the lights in the order their slots were handed out. A caller that builds its own list must keep that order; lights past `kShadowMaxLights` are ignored.
- Realtime needs the G-buffer velocity view: `render()` returns without tracing when it is null, and then nothing is handed to lighting.
- `setRTShadowMask` must run after the per-frame reset and before the lighting pass records, and only when `rendered()` is true — from the filter pass's callback in realtime, from the trace pass's in reference, both of which the graph records before `lighting`. The setter only stores the view; the flag bit the fragment shader branches on and the descriptor at binding 14 are both derived from it when the lighting pass records. Hand it over later and that frame records with bit 5 clear and the stand-in view bound.
