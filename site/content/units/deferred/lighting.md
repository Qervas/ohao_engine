---
module: deferred
id: lighting
title: Deferred lighting
standard: v2
figures: [deferred-lighting-bindings]
---

## The position it never reconstructs

Textbook deferred shading stores depth and rebuilds world position per pixel from
an inverse view-projection matrix. OHAO does not. GBuffer0 is an
`R16G16B16A16_SFLOAT` target carrying world position in RGB and metallic in A, and
the lighting shader reads the position straight out of it:

{{cite shaders/core/deferred_lighting.frag "vec3 fragPos = gBuffer0Sample.rgb;"}}

The `invViewProj` matrix is still the first 64 bytes of the push-constant block and
no line of the shader body reads it; the depth attachment is written into descriptor
binding 4 every frame and the fragment shader never declares a sampler for it.

{{cite shaders/core/deferred_lighting.frag "mat4 invViewProj;"}}

:::why
The rejected alternative — depth plus inverse view-projection, which the push block
is still sized for — costs a matrix multiply and a divide per pixel. Keeping
position resident instead costs 48 bits per pixel of extra G-Buffer write and read
bandwidth: the RGB of the RGBA16F target, not all 64, because the alpha channel
carries metallic and has to be stored under either scheme. It also pins world
coordinates to half-float precision: sub-millimetre near the origin, but ~0.5 units
of representable spacing once a fragment is 1000 units out, where lighting quantises
into terraces. For model-sized scenes the trade is free; a kilometre-scale world is
where it breaks first.
:::

## The sky sentinel is a clear colour

Background pixels are detected by an exact float comparison against the value the
G-Buffer pass clears GBuffer0 to — all four channels zero.

{{cite shaders/core/deferred_lighting.frag "if (gBuffer0Sample.rgb == vec3(0.0) && gBuffer0Sample.a == 0.0) {"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};"}}

Those two lines are one contract split across two files. Change the clear colour to
anything else — a far-plane position, a debug magenta — and every background pixel
falls through into the full lighting loop. The sentinel also has a real, if narrow,
false positive: a non-metallic fragment sitting exactly on the world origin is
classified as sky and written as transparent black.

## Any number of lights, the froxel's few per pixel

The lights are a storage buffer of 64-byte `LightGpu` records at binding 5, with no
size in the declaration, and the froxel lists the `lights.cluster` pass builds sit
beside it at binding 16:

{{cite shaders/core/deferred_lighting.frag "layout(set = 0, binding = 5, std430) readonly buffer Lights { LightGpu lights[]; };"}}

A pixel finds its froxel from its screen tile and the logarithm of its view depth,
and loops over that froxel's list, which holds light indices in ascending order; with
push flag bit 6 it loops over every light in the table instead. The clustered-lighting
unit covers the lists, how they are built and why the two loops give the same image.

{{cite shaders/core/deferred_lighting.frag "const LightGpu light = lights[clustered ? clusterData[listBase + k] : k];"}}

The record is the host's `LightData` without its `lightSpaceMatrix`: `LightClusters`
copies the four `vec4`s across when it writes a frame slot's table, so the matrix the
light table computes for its one legacy shadow-map caster never reaches the GPU.
`params.z`, the shadow-map index, does, and the shader never reads it. It reads `.x`
and `.y`, the two spot-cone cosines, and `.w`, the light's RT shadow slot (below).

{{cite ohao/render/deferred/light_clusters.cpp "std::memcpy(g.params.data(), &l.params, sizeof(g.params));"}}
{{cite shaders/core/deferred_lighting.frag "light.params.x, light.params.y);"}}

The `BRDFSurface` — metallic sharpening, F0 derivation, normalised N and V — is
built once before the loop, so the per-light work is `evaluateBRDF` from
`brdf_ggx.glsl` (the height-correlated Smith stack with Burley diffuse and
Kulla-Conty compensation, a different BRDF from the one the path tracer runs), plus
`calculateAttenuation` for point and spot lights and a cone term for spots. On the
CSM path it is also a whole `calculateShadowCSM` per iteration — a matrix transform
and a nine-tap PCF — whose two arguments do not depend on the light index, so a
second directional light recomputes a scalar identical to the first.

{{cite shaders/core/deferred_lighting.frag "shadow = calculateShadowCSM(fragPos, viewDepth);"}}

## Attenuation: a physical falloff with a hard floor

Point and spot lights use the windowed inverse square that UE4 popularised:

$$\Phi(d) \;=\; \frac{1}{\max(d^2,\,0.01)}\;\Big(\mathrm{clamp}\big(1 - (d/r)^4,\,0,\,1\big)\Big)^{2}$$

where $d$ is the distance from fragment to light and $r$ is the light's range,
packed into `light.direction.w`. The first factor is the physical $1/d^2$; the
second is a window that drives both the value *and* its derivative to zero at
$d = r$ — squaring the window is what makes the derivative vanish, so a light's
influence ends without a visible edge.

{{cite shaders/core/deferred_lighting.frag "return invSq * windowing * windowing;"}}

The window is also what makes a range cull exact. Past $d = r$ the clamp returns
exactly 0, so the term a light adds there is exactly zero, not merely small. The loop
skips a light whose attenuation is exactly 0 before any BRDF work, and the froxel
lists leave out lights whose range sphere cannot reach the froxel: both drop only
terms that would have added $+0$.

{{cite shaders/core/deferred_lighting.frag "if (attenuation == 0.0) continue;   // adds exactly zero: past its range or its cone"}}

Where OHAO departs from UE4 is the denominator. UE4 divides by $d^2 + 1$, softening
the near field everywhere; OHAO keeps the true $1/d^2$ and clamps only the
singularity, at $d = 0.1$ world units. Inside that radius a point light stops
brightening, and that clamp is the only thing between a light placed inside
geometry and an unbounded value in an `R16G16B16A16_SFLOAT` target whose ceiling is
65504.

{{cite shaders/core/deferred_lighting.frag "float invSq = 1.0 / max(d2, 0.01);"}}

## One visibility channel per shadow light

Shadowing is decided per fragment before the BRDF, and the two shadow techniques
are mutually exclusive rather than combined: if an RT visibility view is bound the
C++ side sets bit 5 and never sets the CSM bit.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 32;  // RT shadows (bit 5)"}}

The visibility is a screen-space `R32G32_UINT` image holding eight 8-bit channels,
one per shadow-casting light (x: lights 0-3, y: 4-7, lowest byte first), bound as a
`usampler2D` at binding 14. It is read once before the loop with an integer
`texelFetch` at the fragment's own pixel — no filtering, no UV — because the RT passes
write it at the lighting pass's resolution:

{{cite shaders/core/deferred_lighting.frag "texelFetch(rtShadowVisibility, ivec2(gl_FragCoord.xy), 0).xy"}}

Each light names its channel in `params.w`, which the light table fills: -1 for a
light that never casts (its `castShadows` off, or an emissive proxy), otherwise 0 to
7, the caster's position among the first eight casters in light order. A caster past
the eighth gets -1 too and renders unshadowed; the light table counts those.

{{cite shaders/core/deferred_lighting.frag "int slot = int(round(light.params.w));"}}

A negative slot is never occluded. With RT shadows on, a light's shadow is its byte
of the word divided by 255, and a light whose byte is 0 is skipped, another term
that would have added exactly zero:

{{cite shaders/core/deferred_lighting.frag "shadow = float((word >> (8u * uint(slot & 3))) & 255u) * (1.0 / 255.0);"}}
{{cite shaders/core/deferred_lighting.frag "if (shadow == 0.0) continue;   // fully occluded: adds exactly zero"}}

With them off, the slot only says whether the light casts at all, and the CSM path
decides the rest. That path is gated on `lightType == 0`, so point and spot lights
cast no shadow without RT at all. Cascades themselves are a 3×3 PCF of hardware
depth comparisons on the array slice chosen by view-space depth, and the cascade UBO
carries a `cascadeBlendWidth` the shader never reads:

{{cite shaders/core/deferred_lighting.frag "float cascadeBlendWidth;"}}

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

$$L_{\text{spec}} \;\approx\; L_{\text{pre}}(R,\alpha)\,\big(F_0\,A + B\big)$$

with $L_{\text{pre}}$ read at `roughness * (kEnvPrefilterLevels - 1)` and $(A, B)$ from
Karis's analytic fit to the environment BRDF, `envBRDFApprox`, rather than a lookup
table:

{{cite shaders/core/deferred_lighting.frag "const vec2 ab = envBRDFApprox(NdotV, roughness);"}}

What arrives along the reflection is not the environment alone. RT GI's cosine rays
record the share of them that left the scene (the trace's alpha, accumulated and
filtered with the radiance): the environment is weighted by that visibility, and the
radiance the other rays brought back from the room stands in for the occluded part.
Without RT GI the environment is taken as visible everywhere.

{{cite shaders/core/deferred_lighting.frag "const vec3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, vec3(0.0)) : vec3(0.0));"}}

Occlusion finer than the GI rays resolve comes from the material's `ao` and SSAO: they
darken the diffuse terms as measured and the reflection through Lagarde and de
Rousiers' specular occlusion, which spares a narrow lobe seen head-on:

{{cite shaders/core/deferred_lighting.frag "return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);"}}

`Environment.intensity` scales both images, as it scales the path tracer's misses and
RT GI's. With RT GI the diffuse environment arrives through its escaping rays,
occluded, and the irradiance term is skipped so that light is not counted twice.

The reflection's weight, $W = (F_0 A + B)$ times the specular occlusion, is computed for
every pixel, environment or not, and the pass writes it beside the lit image together
with what the environment and GI contributed through it, $E = W\,L_{\text{in}}$: two more
RGBA16F attachments, for SSR's resolve, which replaces $E$ with its traced reflection
under the same $W$ where it hits (the SSR and SSS unit).

Indirect diffuse light has one source per frame: RT GI's rays when they ran, else the
environment's irradiance, and only with neither a constant ambient (`ambientIntensity`,
0.35 with scene lights), the fallback that keeps a frame without RT or an environment
from going black. The path tracer has no such term, so a frame with RT GI has none
either: walls are lit by direct light and one bounce. All three take the diffuse albedo
only, `albedo * (1 - metallic)`; on a metal, whose albedo is its F0, a diffuse term is a
flat glow the path tracer has no counterpart for.

{{cite shaders/core/deferred_lighting.frag "layout(location = 2) out vec4 outSpecularWeight;"}}

:::why
Before this the IBL bindings resolved to an uncleared 1x1 dummy, the shader read the
raw single-mip HDRI at a roughness-proportional LOD that always clamped to mip 0, and
metals got an extra `F0 * env` term on top: every metal was a sharp mirror of the HDRI,
inside a closed room as much as outdoors. DamagedHelmet read as white chrome where the
path tracer shows dark teal metal.
:::

## Weather channels with no driver

Four branches in the shader modulate albedo, roughness and metallic for rain
wetness, mud, snow and frost. Three of them are orientation-aware: wetness and mud
scale by `clamp(N.y * 2.0, 0, 1)`, snow by a steeper ramp that stays at zero until
the normal is more than ~17° off horizontal, so water pools and snow settles on
upward-facing surfaces.

{{cite shaders/core/deferred_lighting.frag "float slopeFactor = clamp((N.y - 0.3) * (1.0 / 0.7), 0.0, 1.0);"}}

Frost has no such term: its branch reads the push constant straight into the mix
weight, so undersides, walls and ceilings take the same pale-ice tint and the same
collapse to roughness 0.08 as the ground does.

{{cite shaders/core/deferred_lighting.frag "float f = pc.frostCover;"}}

All four cost 24 bytes of the push block and are permanently inert: the three
setters that would drive them have no caller.

{{cite ohao/render/deferred/deferred_lighting_pass.hpp "void setWetness(float w)"}}

The physics side declares the same triple — `PhysicsWorld::updateTerrainFriction`
takes wetness, snow and frost — and is equally uncalled, so a weather system was
designed across two subsystems and never given a driver. Cheap to revive; not a
shipped feature.

## Contracts

- GBuffer0's clear colour and the shader's sky test are one invariant in two files. Changing either alone sends background pixels through the lighting loop.
- The pass owns one descriptor set per frame in flight (`maxSets = MAX_FRAMES_IN_FLIGHT`, three today), and `execute(cmd, frameIndex)` rewrites all 16 bindings of `m_descriptorSets[frameIndex]` only, at record time. Its inputs — the slot's light table, the froxel lists, SSAO, RT shadow mask, GI, env map — can change between frames, so the set has to be rewritten; a single shared set would be rewritten while up to two already-submitted frames still read it. The per-slot set is safe because `VulkanRenderer::renderDeferred` waits the fence of `m_currentFrame` and then records the deferred renderer with that same slot, so the set's previous use is retired. The contract is on the caller: `frameIndex` must be the slot whose fence was waited. {{cite ohao/render/deferred/deferred_lighting_pass.hpp "std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};"}} {{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->render(cmd, m_currentFrame);"}}
- `LightingParams` is 184 bytes, all scalar after `screenSize`; `ambientIntensity`, `sliceScale` and `sliceBias` sit after `cameraPos`, `wetness` and `snowCover`, and `envIntensity` after `frostCover` closes the struct, byte-identical to the push block, whose comment states the same total. Any field added to one side must be added to the other.
- Every skipped term must be exactly zero: the `attenuation == 0.0` and `shadow == 0.0` tests, and the froxel lists, are what make clustered lighting equal the every-light loop byte for byte. An attenuation law without a hard zero past `direction.w` breaks both.
- Binding an RT visibility view disables CSM for that frame outright. A light's `params.w` and the visibility's channels are one contract with the light table and the RT shadow passes (`rt/shadow_trace.slang`, `rt/shadow_filter.slang`): the table hands out slots 0 to 7 in the same order it lists `shadowLights()`, and the RT passes write light *i* into channel *i*. The visibility image must stay the lighting pass's size, since it is fetched at `gl_FragCoord`.
- Declared or carried and never read by this shader: `params.z`, `cascades.normalBias`, `cascades.cascadeBlendWidth`, `pc.invViewProj`, and descriptor bindings 3 and 4 (written, no sampler declared) and 7 and 8 (likewise). Binding 13 does not exist: the layout jumps from the cascade UBO at 12 to the RT shadow visibility at 14.
