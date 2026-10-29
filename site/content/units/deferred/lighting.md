---
module: deferred
id: lighting
title: Deferred lighting
standard: v2
figures: [deferred-lighting-bindings]
---

## One surface model, shared with the path tracer

The lighting pass is a Slang fragment shader, `core/deferred_lighting.slang`, compiled to
`core_deferred_lighting.slang.spv`. It shades every G-buffer pixel through OpenPBR's base,
the same `OpenPBRBase` the reference path tracer samples, and takes its light loop from
`includes/lighting/clustered_lights.slang`, which the forward transparent pass shares:

{{cite shaders/core/deferred_lighting.slang "import material.openpbr;"}}
{{cite shaders/core/deferred_lighting.slang "import includes.lighting.clustered_lights;"}}
{{cite ohao/render/deferred/deferred_lighting_pass.cpp "core_deferred_lighting.slang.spv"}}

The pixel's OpenPBR inputs come back from four targets: base colour (already times
`base_weight`), metalness, specular roughness and AO from the first three, `specular_color`
and `specular_weight` from the specular target at binding 18, `base_diffuse_roughness` and
`specular_ior` from the OpenPBR target at 19. The two encodings are undone where they are
read: the weight was stored over 1.5, the IOR as $(\eta - 1)/2.55$.

{{cite shaders/core/deferred_lighting.slang "mi.specularWeight = spec.a * 1.5;"}}
{{cite shaders/core/deferred_lighting.slang "mi.specularIor = 1.0 + pbr.g * 2.55;"}}

`makeSurface` builds the shading frame and the BSDF once per pixel; every light, the
environment and the ambient fill are then weighed by that one `OpenPBRBase`. Its energy
tables (the Kulla-Conty and dielectric albedo tables the path tracer reads at its binding
38) are a storage buffer at binding 20, an `OpenPBRTablesBuffer` the pass creates at init.

{{cite shaders/core/deferred_lighting.slang "const Surface s = makeSurface(P, N, V, mi);"}}
{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (!m_openpbrTables.create(device, physicalDevice)) {"}}

:::why
Before this the deferred frame ran its own BRDF (Burley diffuse, a fitted environment BRDF,
a roughness floor of 0.04) beside the path tracer's, so the two renderers disagreed on the
material before they disagreed on transport. With one module the remaining differences
are the ones the deferred frame chooses: one GI bounce, the split-sum environment, the
representative point for area and sphere specular.
:::

## The position it never reconstructs

Textbook deferred shading stores depth and rebuilds world position per pixel from
an inverse view-projection matrix. OHAO does not. GBuffer0 is an
`R16G16B16A16_SFLOAT` target carrying world position in RGB and metallic in A, and
the lighting shader reads the position straight out of it:

{{cite shaders/core/deferred_lighting.slang "const float3 P = g0.rgb;"}}

The depth attachment is still written into descriptor binding 4 every frame, and the
velocity target into 3; the shader declares neither. `invViewProj`, the first 64 bytes of
the push block, is read for one thing only: the view ray of a sky pixel (below).

:::why
The rejected alternative — depth plus inverse view-projection — costs a matrix multiply
and a divide per pixel. Keeping position resident instead costs 48 bits per pixel of
extra G-Buffer write and read bandwidth: the RGB of the RGBA16F target, not all 64,
because the alpha channel carries metallic and has to be stored under either scheme. It
also pins world coordinates to half-float precision: sub-millimetre near the origin, but
~0.5 units of representable spacing once a fragment is 1000 units out, where lighting
quantises into terraces. For model-sized scenes the trade is free; a kilometre-scale
world is where it breaks first.
:::

## The sky sentinel is a clear colour

Background pixels are detected by an exact comparison against the value the G-Buffer pass
clears GBuffer0 to — all four channels zero.

{{cite shaders/core/deferred_lighting.slang "if (all(g0 == float4(0.0)))"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};"}}

Those two lines are one contract split across two files. Change the clear colour to
anything else and every background pixel falls through into the lighting loop. With an
environment the sky pixel gets the environment along its view ray, prefiltered level 0
(the source), as the path tracer's missing camera rays see it; without one it stays
transparent black for the sky pass. The sentinel has a real, if narrow, false positive: a
non-metallic fragment exactly at the world origin is classified as sky.

{{cite shaders/core/deferred_lighting.slang "o.color = float4(pc.envIntensity * envPrefiltered.SampleLevel(equirectUV(viewDir), 0.0).rgb, 1.0);"}}

A light's emitter mesh (emissive alpha 1) is the other early exit: its radiance alone,
with $E = W = 0$ so SSR leaves it.

{{cite shaders/core/deferred_lighting.slang "if (emitted.a > 0.5)"}}

## Any number of lights, the froxel's few per pixel

The lights are a structured buffer of 64-byte `LightGpu` records at binding 5, and the
froxel lists the `lights.cluster` pass builds sit beside it at binding 16:

{{cite shaders/core/deferred_lighting.slang "[[vk::binding(5, 0)]] StructuredBuffer<LightGpu> lights;"}}

A pixel finds its froxel from its screen tile and the logarithm of its view depth
(`lightList`), and `directLight` loops over that froxel's list, which holds light indices
in ascending order; with push flag bit 6 it loops over every light in the table instead.
The clustered-lighting unit covers the lists, how they are built and why the two loops
give the same image.

{{cite shaders/includes/lighting/clustered_lights.slang "float3 directLight(Surface s, StructuredBuffer<LightGpu> lights"}}

The record is the host's `LightData` without its `lightSpaceMatrix`: `LightClusters`
copies the four `vec4`s across when it writes a frame slot's table. `params.x` and `.y` are
the spot-cone cosines, `.z` the light's sphere radius (0 for a point), `.w` its RT shadow
slot (below).

{{cite ohao/render/deferred/light_clusters.cpp "std::memcpy(g.params.data(), &l.params, sizeof(g.params));"}}
{{cite ohao/render/scene/render_scene_types.hpp "z = sphere radius (0: a point)"}}

Each light becomes a direction and an attenuated radiance (`lightAt`), and then one of
three evaluations of the same BSDF:

- a light with no size (a sun, or a point of radius 0): `OpenPBRBase.eval` toward it;
- a point or spot light with a radius: `evaluateSphere`, the diffuse lobe toward the
  sphere's centre and the specular lobe toward the point of the sphere nearest the
  reflection, with the GGX lobe widened by the sphere's angular size (`withRoughness`),
  so a mirror shows the sphere the camera sees;
- an area light: `evaluateRect`, the diffuse lobe scaled by the rectangle's exact
  irradiance and the specular at the representative point.

{{cite shaders/includes/lighting/clustered_lights.slang "Lo += evaluateSphere(s, light.position.xyz - s.position, light.params.z, radiance, t) * shadow;"}}

`evalLobes` returns the specular and diffuse lobes separately so the sphere and the
rectangle can light each from its own direction. Each lobe is OpenPBR's: F82-tint on the
metal, exact Fresnel at the IOR on the dielectric, Kulla-Conty multiple scattering, EON
beneath — the same expression the path tracer's next-event estimation evaluates.

## Attenuation: a physical falloff with a hard floor

Point and spot lights use the windowed inverse square that UE4 popularised:

$$\Phi(d) \;=\; \frac{1}{\max(d^2,\,0.01)}\;\Big(\mathrm{clamp}\big(1 - (d/r)^4,\,0,\,1\big)\Big)^{2}$$

where $d$ is the distance from fragment to light and $r$ is the light's range,
packed into `light.direction.w`. The first factor is the physical $1/d^2$; the
second is a window that drives both the value *and* its derivative to zero at
$d = r$ — squaring the window is what makes the derivative vanish, so a light's
influence ends without a visible edge.

{{cite shaders/includes/lighting/clustered_lights.slang "return invSq * windowing * windowing;"}}

The window is also what makes a range cull exact. Past $d = r$ the clamp returns
exactly 0, so the term a light adds there is exactly zero, not merely small. The loop
skips a light whose radiance is exactly 0 before any BSDF work (an area light has no
window and is never skipped this way), and the froxel lists leave out lights whose range
sphere cannot reach the froxel: both drop only terms that would have added $+0$.

{{cite shaders/includes/lighting/clustered_lights.slang "if (all(radiance == float3(0.0)) && type != 3) continue;   // adds exactly zero: past its range or cone"}}

Where OHAO departs from UE4 is the denominator. UE4 divides by $d^2 + 1$, softening
the near field everywhere; OHAO keeps the true $1/d^2$ and clamps only the
singularity, at $d = 0.1$ world units. Inside that radius a point light stops
brightening, and that clamp is the only thing between a light placed inside
geometry and an unbounded value in an `R16G16B16A16_SFLOAT` target whose ceiling is
65504.

{{cite shaders/includes/lighting/clustered_lights.slang "const float invSq = 1.0 / max(d2, 0.01);"}}

## One visibility channel per shadow light

Shadowing is decided per light inside the loop, from a `LightVisibility` the shader
fills once per pixel. The two shadow techniques are mutually exclusive rather than
combined: if an RT visibility view is bound the C++ side sets bit 5 and never sets the
CSM bit.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 32;  // RT shadows (bit 5)"}}

The visibility is a screen-space `R32G32_UINT` image holding eight 8-bit channels,
one per shadow-casting light (x: lights 0-3, y: 4-7, lowest byte first), bound as an
integer texture at binding 14. It is read once per pixel with an integer `Load` at the
fragment's own pixel — no filtering, no UV — because the RT passes write it at the
lighting pass's resolution:

{{cite shaders/core/deferred_lighting.slang "vis.rtWords = vis.rt ? rtShadowVisibility.Load(int3(int2(fragCoord.xy), 0)).xy : uint2(0u);"}}

Each light names its channel in `params.w`, which the light table fills: -1 for a
light that never casts (its `castShadows` off, or an emissive proxy), otherwise 0 to
7, the caster's position among the first eight casters in light order. A caster past
the eighth gets -1 too and renders unshadowed; the light table counts those.

{{cite shaders/includes/lighting/clustered_lights.slang "const int slot = int(round(light.params.w));"}}

A negative slot is never occluded. With RT shadows on, a light's shadow is its byte
of the word divided by 255, and a light whose byte is 0 is skipped, another term
that would have added exactly zero:

{{cite shaders/includes/lighting/clustered_lights.slang "shadow = float((word >> (8u * uint(slot & 3))) & 255u) * (1.0 / 255.0);"}}
{{cite shaders/includes/lighting/clustered_lights.slang "if (shadow == 0.0) continue;   // fully occluded: adds exactly zero"}}

With them off, the slot only says whether the light casts at all, and a casting
directional light takes the CSM's visibility, computed once per pixel before the loop and
shared by every directional caster. Point and spot lights cast no shadow without RT.
Cascades are a 3×3 PCF of hardware depth comparisons on the array slice chosen by
view-space depth, and the cascade UBO carries a `cascadeBlendWidth` the shader never
reads:

{{cite shaders/core/deferred_lighting.slang "vis.directional = !vis.rt && (pc.flags & 4u) != 0u ? shadowCSM(P, viewDepth) : 1.0;"}}
{{cite shaders/core/deferred_lighting.slang "float cascadeBlendWidth;"}}

so a cascade transition is a hard switch at the split distance, visible as a
discontinuity in filter width rather than a blend.

## Image-based lighting: prefiltered, split-sum, occluded

The environment reaches lighting through two images `EnvPrefilter` builds once per HDRI
(`shaders/ibl/env_prefilter.slang`, recorded into the environment upload's own
submission): the environment GGX-prefiltered by roughness at binding 15,
`kEnvPrefilterLevels` mip levels with level $i$ at roughness $i/(\text{levels}-1)$, and
its cosine convolution (the irradiance over $\pi$) at binding 7. Both are
equirectangular, mapped as the path tracer's miss maps a direction; the path tracer and
RT GI keep reading the unfiltered source image.

{{cite ohao/render/deferred/deferred_lighting_pass.hpp "void setEnvironment(VkImageView prefiltered, VkImageView irradiance, VkSampler sampler, float intensity);"}}

Each level integrates Hammersley samples of the GGX lobe around the texel's direction
(normal = view = reflection, the split sum's assumption), every sample read from a mip
chain of the environment at the mip its solid angle covers, so a few hundred samples
converge without fireflies from the HDRI's bright spots:

{{cite shaders/ibl/env_prefilter.slang "const float lod = clamp(0.5 * log2(sampleSolidAngle / texel) + 1.0, 0.0, float(pc.size.w - 1u));"}}

Lighting evaluates the split sum

$$L_{\text{spec}} \;\approx\; L_{\text{pre}}(R,\alpha)\;E_{\text{spec}}(\omega_o)$$

with $L_{\text{pre}}$ read at `roughness * (kEnvPrefilterLevels - 1)` and $E_{\text{spec}}$
the specular lobes' own directional albedo toward the viewer, `specularAlbedo`, from
OpenPBR's energy tables rather than a fitted DFG curve. It carries the metal's F82 tint,
the dielectric's IOR and specular colour, and the multiple-scattering energy, so the
reflection's weight and the analytic lights' specular come from one BSDF.

{{cite shaders/core/deferred_lighting.slang "const float3 specularAlbedo = s.bsdf.specularAlbedo(s.wo, tables);"}}

What arrives along the reflection is not the environment alone. RT GI's cosine rays
record the share of them that left the scene (the trace's alpha, accumulated and
filtered with the radiance): the environment is weighted by that visibility, and the
radiance the other rays brought back from the room stands in for the occluded part.
Without RT GI the environment is taken as visible everywhere.

{{cite shaders/core/deferred_lighting.slang "const float3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, float3(0.0)) : float3(0.0));"}}

Occlusion finer than the GI rays resolve comes from the material's `ao` and SSAO: they
darken the diffuse terms as measured and the reflection through Lagarde and de
Rousiers' specular occlusion, which spares a narrow lobe seen head-on:

{{cite shaders/includes/lighting/clustered_lights.slang "return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);"}}

`Environment.intensity` scales both images, as it scales the path tracer's misses and
RT GI's. With RT GI the diffuse environment arrives through its escaping rays,
occluded, and the irradiance term is skipped so that light is not counted twice.

The reflection's weight, $W = E_{\text{spec}}$ times the specular occlusion, is computed
for every pixel, environment or not, and the pass writes it beside the lit image together
with what the environment and GI contributed through it, $E = W\,L_{\text{in}}$: two more
RGBA16F attachments, for SSR's resolve, which replaces $E$ with its traced reflection
under the same $W$ where it hits (the SSR and SSS unit).

{{cite shaders/core/deferred_lighting.slang "const float3 specularWeight = specularAlbedo * specularOcclusion(NdotV, occlusion, roughness);"}}

Indirect diffuse light has one source per frame: RT GI's rays when they ran, else the
environment's irradiance, and only with neither a constant ambient (`ambientIntensity`,
0.35 with scene lights), the fallback that keeps a frame without RT or an environment
from going black. The path tracer has no such term, so a frame with RT GI has none
either: walls are lit by direct light and one bounce. All three are weighed by the
diffuse lobe's directional albedo, `diffuseAlbedo`: EON's albedo times what the specular
layer lets through, zero on a metal.

{{cite shaders/core/deferred_lighting.slang "const float3 diffuseAlbedo = s.bsdf.diffuseAlbedo(s.wo, tables);"}}

## Weather channels with no driver

Four branches in the shader modulate albedo, roughness and metallic for rain
wetness, mud, snow and frost, before the BSDF is built. Three of them are
orientation-aware: wetness and mud scale by `saturate(N.y * 2.0)`, snow by a steeper ramp
that stays at zero until the normal is more than ~17° off horizontal, so water pools and
snow settles on upward-facing surfaces.

{{cite shaders/core/deferred_lighting.slang "const float s = pc.snowCover * saturate((N.y - 0.3) * (1.0 / 0.7));"}}

Frost has no such term: its branch reads the push constant straight into the mix
weight, so undersides, walls and ceilings take the same pale-ice tint and the same
collapse to roughness 0.08 as the ground does.

{{cite shaders/core/deferred_lighting.slang "const float f = pc.frostCover;"}}

All four cost 24 bytes of the push block and are permanently inert: the three
setters that would drive them have no caller.

{{cite ohao/render/deferred/deferred_lighting_pass.hpp "void setWetness(float w)"}}

The physics side declares the same triple — `PhysicsWorld::updateTerrainFriction`
takes wetness, snow and frost — and is equally uncalled, so a weather system was
designed across two subsystems and never given a driver. Cheap to revive; not a
shipped feature.

## Contracts

- GBuffer0's clear colour and the shader's sky test are one invariant in two files. Changing either alone sends background pixels through the lighting loop.
- The pass owns one descriptor set per frame in flight (`maxSets = MAX_FRAMES_IN_FLIGHT`, three today), and `execute(cmd, frameIndex)` rewrites all 18 bindings of `m_descriptorSets[frameIndex]` only, at record time. Its inputs — the slot's light table, the froxel lists, SSAO, RT shadow mask, GI, env map — can change between frames, so the set has to be rewritten; a single shared set would be rewritten while up to two already-submitted frames still read it. The per-slot set is safe because `VulkanRenderer::renderDeferred` waits the fence of `m_currentFrame` and then records the deferred renderer with that same slot, so the set's previous use is retired. The contract is on the caller: `frameIndex` must be the slot whose fence was waited. {{cite ohao/render/deferred/deferred_lighting_pass.hpp "std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};"}} {{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->render(cmd, m_currentFrame);"}}
- `LightingParams` is 184 bytes, all scalar after `screenSize`; `ambientIntensity`, `sliceScale` and `sliceBias` sit after `cameraPos`, `wetness` and `snowCover`, and `envIntensity` after `frostCover` closes the struct, byte-identical to the shader's `LightingPush`, whose comment states the same total. Any field added to one side must be added to the other.
- The G-buffer's encodings and the decode here are one contract: `specular_weight / 1.5` and `(specular_ior - 1) / 2.55` in `gbuffer_body.glsl`, `* 1.5` and `1 + * 2.55` here. The albedo arrives already times `base_weight`, so the shader passes `baseWeight = 1`.
- Every skipped term must be exactly zero: the zero-radiance and `shadow == 0.0` tests, and the froxel lists, are what make clustered lighting equal the every-light loop byte for byte. An attenuation law without a hard zero past `direction.w` breaks both.
- Binding an RT visibility view disables CSM for that frame outright. A light's `params.w` and the visibility's channels are one contract with the light table and the RT shadow passes (`rt/shadow_trace.slang`, `rt/shadow_filter.slang`): the table hands out slots 0 to 7 in the same order it lists `shadowLights()`, and the RT passes write light *i* into channel *i*. The visibility image must stay the lighting pass's size, since it is fetched at the fragment's pixel.
- Declared or carried and never read by this shader: `cascades.normalBias`, `cascades.cascadeBlendWidth`, and descriptor bindings 3 and 4 (written, no resource declared). Bindings 8, 9 and 13 do not exist: the layout goes 0–7, 10–12, 14–20.
