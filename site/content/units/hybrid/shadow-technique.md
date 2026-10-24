---
module: hybrid
id: shadow-technique
title: RT shadow technique
standard: v2
figures: [hybrid-shadow-technique-penumbra]
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

The result is a screen-resolution `R8G8B8A8_UNORM` image with one channel per
shadow light, 0 = shadowed, 1 = lit,

{{cite ohao/render/rt/rt_shadow_technique.cpp "imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;"}}

which the deferred lighting fragment shader samples once, at binding 14, before
its light loop:

{{cite shaders/core/deferred_lighting.frag "vec4 rtShadow = rtShadows ? texture(rtShadowMask, inTexCoord) : vec4(1.0);"}}

Inside the loop each light picks its own channel by the slot stored in its
`params.w`, and the BRDF result is multiplied by it.

{{cite shaders/core/deferred_lighting.frag "if (slot < 4) shadow = rtShadow[slot];"}}

Binding the mask is also what switches the pipeline — though not at the line that
looks like it does. `setRTShadowMask` raises flag bit 5 and clears the CSM bit,
but neither write ever reaches the GPU: `execute` zeroes the whole flag word and
rebuilds it from the bound views every frame, before pushing it.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags = 0;"}}

In that rebuild the RT bit and the CSM bit are the two arms of a single if/else
keyed on whether an RT shadow view is bound at all, so the two shadow systems are
mutually exclusive by construction rather than by convention.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "else if (m_shadowMapView != VK_NULL_HANDLE)"}}

The fragment shader repeats the structure, branching on bit 5 first and reaching
CSM only through the `else`.

{{cite shaders/core/deferred_lighting.frag "} else if ((pc.flags & 4u) != 0u && lightType == 0) {"}}

## Which lights get a channel

The light table decides. Walking the raster lights in light order, it gives each
light whose `castShadows` is set the next free channel until there are
`kMaxShadowLights` of them, four, and records it in `params.w`:

{{cite ohao/render/scene/light_table.cpp "shadowSlot = static_cast<float>(m_shadowLights.size());"}}

A light that casts shadows after the four channels are taken gets
`kShadowNoSlot` (4); a light with `castShadows` off gets `kShadowNever` (-1), and
so does every emissive proxy, which sits inside its own mesh.

{{cite ohao/render/scene/render_scene_types.hpp "inline constexpr float kShadowNoSlot = static_cast<float>(kMaxShadowLights);"}}

{{cite ohao/render/scene/light_table.cpp "light.params = glm::vec4(0.0f, 0.0f, -1.0f, kShadowNever);   // inside its own mesh: never shadowed"}}

The lighting shader reads the three cases directly. A negative slot is never
occluded; a slot below four takes its channel when the RT bit is set; slot 4 is
unshadowed under RT shadows and, without them, a directional light still gets
CSM.

{{cite shaders/core/deferred_lighting.frag "int slot = int(round(light.params.w));"}}

The lights with a channel, in channel order, are `RenderScene::shadowLights()`,
which the dispatch hands to the deferred renderer each frame (or, for a scene
with no lights at all, the fallback sun as channel 0). The renderer adds the
pass only when that list is non-empty: a frame in which no light casts shadows
traces nothing.

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_useRTShadows && m_rtShadow && sceneTlas != VK_NULL_HANDLE && m_gbufferPass && !m_shadowLights.empty()) {"}}

## What the estimator actually computes

Direct lighting from an area source is

$$L_o(x,\omega_o) = \int_{\Omega_\ell} f_r(x,\omega_o,\omega)\, L_i(\omega)\, V(x,\omega)\, (\mathbf{n}\!\cdot\!\omega)\, d\omega$$

where $\Omega_\ell$ is the solid angle the light subtends at the shading point
$x$, $f_r$ is the BRDF, $L_i$ the incident radiance, $\mathbf{n}$ the shading
normal, and $V \in \{0,1\}$ the binary visibility along $\omega$. One mask
channel per light cannot represent that integral. What this pair of passes
evaluates is the separable approximation

$$L_o \approx \bar V(x)\; f_r(x,\omega_o,\bar\omega)\, L_i(\bar\omega)\,(\mathbf{n}\!\cdot\!\bar\omega),
\qquad \bar V(x)=\frac{1}{N}\sum_{s=1}^{N} V(x,\omega_s)$$

where $\bar\omega$ is the direction the lighting pass uses for the light. The
raygen shader produces $\bar V$ by averaging $N$ traced samples into the light's
channel:

{{cite shaders/rt/rt_shadow.rgen "mask[l] = visibility / float(rays);"}}

and the lighting pass multiplies that single number by a BRDF evaluated only at
$\bar\omega$. Factoring $V$ out is exact wherever it is constant across the
light — everywhere but the penumbra; inside one, the error is the covariance
between visibility and the cosine-weighted BRDF over the source, the term a path
tracer keeps and this one discards.

## A ray tracing pipeline whose hit shader never runs

Three shader groups are compiled: raygen, miss, and a triangle hit group whose
only member is an any-hit shader — `closestHitShader` is `VK_SHADER_UNUSED_KHR`
in all three, and max recursion depth is one, since a visibility ray spawns
nothing.

{{cite ohao/render/rt/rt_shadow_technique.cpp "pipelineInfo.maxPipelineRayRecursionDepth = 1;"}}

The interesting part is the flag combination the raygen traces with:
terminate-on-first-hit, force-opaque, and skip-closest-hit together.

{{cite shaders/rt/rt_shadow.rgen "uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT"}}

`gl_RayFlagsOpaqueEXT` forces every intersection to be treated as opaque, and
opaque intersections do not invoke any-hit shaders. The any-hit stage in group 2
therefore never executes — and does not need to. The payload is set to `0.0`
immediately before `traceRayEXT`, the miss shader is the only stage that can
raise it, and a hit leaves the pre-set value alone.

{{cite shaders/rt/rt_shadow.rmiss "shadowPayload = 1.0;"}}

The occlusion answer is delivered by a shader that did not run. `terminateRayEXT`
in the `.rahit` is unreachable under the current flags; it becomes live the moment
someone drops `gl_RayFlagsOpaqueEXT` for alpha-cutout foliage, which is
presumably why it is compiled in at all.

{{cite shaders/rt/rt_shadow.rahit "terminateRayEXT;"}}

:::why
The rejected alternative is a closest-hit shader reporting hit distance — the
channel an NRD-style denoiser wants for sizing its spatial filter. What gives it
up is `gl_RayFlagsTerminateOnFirstHitEXT`: traversal may stop at the first
intersection it finds rather than continue until the nearest one is proven. The
empty `closestHitShader` slots in all three groups follow from that flag — they
are its consequence, not the reason for it. The price: this mask carries no
distance signal, so its softness must come entirely from ray count.
:::

## Three penumbra samplers

The push constants carry up to four lights, three `vec4`s each, followed by the
dispatch size, the light count and the rays per light:

{{cite shaders/rt/rt_shadow.rgen "ShadowLight lights[4];"}}

Softness comes from jittering the sample target, and the shader has a geometry
per light type. A directional light (type 0) gets a tangent basis around the
light direction and an offset inside a disk of the light's angular radius, with
$\sqrt{r_1}$ making samples uniform in disk area — a small-angle construction,
since the offset is the angle rather than its tangent:

{{cite shaders/rt/rt_shadow.rgen "float angle = light.edge1AndRadius.w * sqrt(r1);"}}

A point or spot light (type 1) instead displaces the light *position* to a
uniformly distributed point on a sphere of the light's radius:

{{cite shaders/rt/rt_shadow.rgen "float phi = acos(1.0 - 2.0 * r2);"}}

A rectangle (type 2) is sampled uniformly over its area, from the corner its
position names along its two edges — the convention the path tracer samples it
by:

{{cite shaders/rt/rt_shadow.rgen "target = light.positionAndType.xyz + light.edge1AndRadius.xyz * r1 + light.edge2AndRange.xyz * r2;"}}

Because $\cos\varphi = 1 - 2r_2$ is uniform on $[-1,1]$, the sphere branch is a
correct uniform-area sampler of the sphere *surface* — with a consequence worth
naming. By Archimedes' hat-box theorem a uniform distribution on a sphere
projects to a uniform distribution on any single axis, so an occluder edge
sweeping across the source yields a **linear** visibility ramp, not the
circular-segment S-curve a genuinely disk-shaped source produces. The difference
shows at the two ends of the penumbra, where the true profile approaches full
light and full shadow tangentially and the linear one arrives with a crease.

{{figure hybrid-shadow-technique-penumbra "Penumbra visibility for a straight occluder edge swept across the light. All three curves are analytic: the exact disk-light profile, the ramp implied by the sphere-surface sampler in rt_shadow.rgen, and that ramp quantised at eight rays, the count a single shadow light gets. Conceptual, not a captured render."}}

## Rays and seeds

The technique spends a fixed budget of eight rays a pixel and shares it among
the frame's shadow lights, each getting at least one:

{{cite ohao/render/rt/rt_shadow_technique.hpp "return lights == 0 ? 0u : std::max(1u, raysPerPixel / lights);"}}

So the pass costs about the same with one light as with four, and the quality
does not: one light gets eight rays and nine distinct visibility levels, four
lights get two rays each and three levels apiece. At these counts the
quantisation matters more than the profile.

Every random number is an integer PCG hash of the render seed, the frame index
since the last temporal reset, the pixel, the light, the sample and the
dimension:

{{cite shaders/rt/rt_shadow.rgen "uint h = pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y ^ pcg((light << 24u) ^ (s << 4u) ^ dim)))));"}}

The deferred renderer supplies both seeds, the one `setRenderSeed` stored and the
counter `resetTemporalState` zeroes, so the noise decorrelates from frame to
frame while two renders of the same world with the same seed trace the same
rays. Nothing in the technique accumulates the mask across frames.

{{cite ohao/render/rt/rt_shadow_technique.cpp "pc.seed = glm::uvec4(input.renderSeed, input.frameIndex, 0u, 0u);"}}

## Sharp edges

The ray origin is pushed a flat 0.05 world units along the shading normal, with
`tmin` = 0.001 behind that:

{{cite shaders/rt/rt_shadow.rgen "vec3 origin = worldPos + N * 0.05;"}}

The offset is absolute, not scaled by depth or texel footprint: too large for
centimetre-scale detail, where contact shadows detach into the ray-traced form of
peter-panning, and too small for a kilometre-scale scene.

Sky pixels are detected by testing the GBuffer position for exact zero *and* zero
alpha:

{{cite shaders/rt/rt_shadow.rgen "if (worldPos == vec3(0.0) && posSample.a == 0.0)"}}

The alpha being tested is not a coverage bit — GBuffer0 packs metallic there:

{{cite shaders/core/gbuffer.frag "outGBuffer0 = vec4(fragWorldPos, metallic);"}}

The test works only because that attachment clears to (0,0,0,0); a non-metallic
surface passing exactly through the world origin is classified as sky and written
fully lit in every channel.

A point at more than the light's range plus its radius from a point or spot
light skips that light entirely and keeps its channel at 1: the light
contributes nothing there, so there is nothing to occlude. A rectangle is pushed
with a range of 0, which the test reads as unlimited.

{{cite shaders/rt/rt_shadow.rgen "if (type != 0u && range > 0.0 && distance(worldPos, light.positionAndType.xyz) > range + light.edge1AndRadius.w)"}}

Samples whose jittered direction falls below the horizon are skipped and counted
as zero visibility:

{{cite shaders/rt/rt_shadow.rgen "if (dot(N, L) <= 0.0) continue;"}}

Inside the integral that would cost nothing — those directions carry zero cosine
weight anyway. But a skipped sample still divides by the ray count, so what
reaches the mask is a binary horizon *indicator* averaged over the jitter, and
the lighting pass multiplies it into a BRDF that already applies exactly one
$\mathbf{n}\cdot\bar\omega$, at the unjittered direction.

{{cite shaders/includes/brdf/brdf_ggx.glsl "return (diffuse + specular) * lightColor * NdotL;"}}

The product is indicator × cosine: a step-shaped second falloff riding on the
cosine, darkening a band near the terminator by an amount no BRDF term accounts
for.

Finally the ray mask. The raygen traces with `0x01`, commented "static geometry
only". The `RenderScene` builds every live, visible TLAS instance with mask
`0xFF`, and a hidden or released slot with zero:

{{cite ohao/render/scene/render_scene.cpp "vi.mask = visible ? 0xFFu : 0u;"}}

so the filter passes every visible instance and rejects exactly the ones that
should cast nothing. It is a visibility switch, not a static-versus-dynamic
distinction: the companion `MASK_ANIMATED = 0x00` in `rt_visibility.hpp` has no
users at all, since skeletal geometry left the RT path.

:::key
Each channel is a boolean visibility average, not a shadow with radiance in it.
Everything the technique gets right — hardness, contact, penumbra width — is
decided by where the N sample targets are placed; everything it gets wrong, at
the terminator and inside the penumbra, follows from that scalar being pulled
out of an integral it does not commute with. Past four shadow-casting lights it
gets nothing right: the fifth is lit everywhere.
:::

## Contracts

- `DeferredRenderer::onResize` forwards the new extent to `m_rtShadow` as well as `m_rtGI`; a new RT technique that owns a screen-sized target must be added to that list, or its output keeps its `init()` extent while `vkCmdTraceRaysKHR` dispatches at the new one and the lighting pass samples a mask that no longer registers with the screen.
- The GBuffer sampler is created on the first `render()` and destroyed only in `destroy()`; the descriptor set rewritten during recording is the one for the caller's `frameSlot`, one of `MAX_FRAMES_IN_FLIGHT`. Both hold only while the caller passes the frame-ring slot whose fence it has just waited on.
- The pre-trace transition uses `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` with `srcStageMask = TOP_OF_PIPE`. Discarding the contents is fine — every pixel is written unconditionally each dispatch — but that barrier does not wait on last frame's fragment-shader reads of the same image.
- Light *i* of `ShadowInput::lights` is traced into channel *i*, and the lighting shader reads channel `params.w`. The two agree only because both come from one walk of the light table: `shadowLights()` lists the lights in the order their slots were handed out. A caller that builds its own list must keep that order, and a light beyond the fourth in it is ignored.
- `setRTShadowMask` must run after the per-frame reset and before the lighting pass records, and only when `rendered()` is true — today from the RTShadow pass's own callback, which the graph records before `DeferredLighting`. The setter only stores `m_rtShadowView`; the flag bit the fragment shader branches on and the descriptor at binding 14 are both derived from that member when the lighting pass records. Hand it over later and that frame records with bit 5 clear and the dummy view bound.
