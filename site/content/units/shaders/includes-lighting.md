---
module: shaders
id: includes-lighting
title: Lighting includes
standard: v2
figures: [shaders-includes-lighting-falloff]
---

## What the include graph actually says

`shaders/includes/lighting/` holds one header, `light_attenuation.glsl`. Grep the
include directive across `shaders/` and no stage names it; the only `#include` it
takes part in is its own pull of `math.glsl`.

{{cite shaders/includes/lighting/light_attenuation.glsl "includes/common/math.glsl"}}

That would be a boring page if an unused file implied unused math. It does not:
`deferred_lighting.frag`, the shader that actually runs, re-derives the spot-cone
ramp, the light-direction and ambient terms, and the whole split-sum IBL block
inline — the same algebra, none of it included from here. The one thing it does
*not* re-derive is the distance law this header calls its default.

:::key
Treat this directory as a proposal, not an implementation. No shader includes it,
and the CMake source glob compiles only stages, so the header is not even parsed by
a shader build. Everything that ships is in `deferred_lighting.frag`, which copied
some of this math and rejected the rest. The interesting failures are where the
copy and the original disagree.
:::

## The falloff the header calls its default

`attenuationSmooth` is commented in the header as "current OHAO default". It has
no callers, and neither does anything else in the file:

$$a_{\text{hdr}}(d) \;=\; \frac{1}{1 + d^{2}/r^{2}}\;\Big(\mathrm{clamp}\big(1 - \tfrac{d}{r},\,0,\,1\big)\Big)^{2}$$

$d$ is the surface-to-light distance and $r$ is the light's range, which travels
in the `w` channel of the packed direction vector. The first factor is an
inverse-square-*shaped* core that stays finite at $d=0$, where it equals exactly
1. The second is a squared edge ramp, so both the value and its first derivative
vanish at $d = r$: a light leaving its range fades out instead of popping.

{{cite shaders/includes/lighting/light_attenuation.glsl "float attenuation = 1.0 / (1.0 + (distance * distance) / (range * range));"}}

The header pairs it with a plain linear cone ramp between the inner and outer
cosines, `attenuationSpotCone`, and a smoothstepped `attenuationSpotConeSmooth`
variant.

{{cite shaders/includes/lighting/light_attenuation.glsl "float attenuationSpotCone(float cosAngle, float innerCone, float outerCone) {"}}

The price of that bounded shape is that $r$ scales brightness everywhere, not
just near the cutoff. Both factors rise monotonically with $r$ at fixed $d$, so
widening a light's range *brightens* it at every distance: at $d = 5$, taking $r$
from 10 to 20 moves the factor from $0.8 \times 0.25 = 0.200$ to
$0.941 \times 0.5625 = 0.529$. Range is not a cutoff knob here; it is a second
intensity knob.

## The falloff the deferred pipeline evaluates

`deferred_lighting.frag` does not include the header. It declares its own
`calculateAttenuation`, and picks a different law — Karis-style windowed
inverse-square:

$$a_{\text{def}}(d) \;=\; \frac{1}{\max(d^{2},\,0.01)}\;\Big(\mathrm{clamp}\big(1 - (d/r)^{4},\,0,\,1\big)\Big)^{2}$$

Same symbols. This one is correct in the far field and clamps the $1/d^{2}$
singularity at 100 rather than removing it, with a quartic window that reaches
zero at $d = r$ while staying near 1 over most of the light's useful volume.

{{cite shaders/core/deferred_lighting.frag "float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);"}}

That exact zero is load-bearing. Clustered lighting lists, for each froxel, only the
lights whose range sphere reaches it, and the lighting loop skips a light whose
attenuation is exactly 0; both are exact only because past $d = r$ the clamp returns
0, not a small number. The `LightGpu` record states the same contract in its field
documentation:

{{cite shaders/shared/layout.json "xyz direction, w range: point and spot light contributes nothing past it"}}

The header already contains the near-match — `attenuationUE4` — with zero
callers. It also differs from the deferred version in a way that changes units:
it normalises distance by range *before* squaring, so its core is dimensionless
and bounded by 1.

{{cite shaders/includes/lighting/light_attenuation.glsl "float d = distance / range;"}}

{{figure shaders-includes-lighting-falloff "Conceptual, not measured: the three laws evaluated directly from the formulas in light_attenuation.glsl and deferred_lighting.frag, each normalised to 1 at d = 0.05·range so the shapes can be compared on one axis. Log vertical scale; curves are clipped where they fall below 1e-3, though all three reach exactly zero at d = range."}}

:::why
One light record, two incompatible falloff laws. The header's default is
dimensionless and peaks at 1; the deferred law carries 1/length² units and peaks
at 100. They are not related by any constant — the ratio between them varies
with both distance and range — so an intensity tuned against one is simply wrong
under the other. That is the reason not to "tidy" `deferred_lighting.frag` onto
this header: including it is free, but switching the law re-authors every light
intensity in every scene. If a shared include is wanted, the function to share
is the deferred one, and the header's `attenuationUE4` is the closest starting
point. Any replacement must keep a hard zero at the range, or clustered lighting
stops being exact.
:::

## The IBL the deferred pass evaluates instead

The header directory has no image-based lighting at all. What ships is split-sum
IBL written inline in `deferred_lighting.frag`, fed from the 2D equirect
environment map with a mip level as roughness proxy instead of a prefiltered cubemap:

{{cite shaders/core/deferred_lighting.frag "float lod = roughness * 9.0; // assuming 10 mip levels for 1024x512"}}

The BRDF LUT it samples is, in practice, the pass's 1×1 fallback image.
`DeferredLightingPass::setIBLTextures` exists and `DeferredRenderer` forwards to
it, but nothing calls the outer setter, so `m_brdfLUTView` stays null and binding
9 resolves to the dummy view:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfos[8].imageView = m_brdfLUTView != VK_NULL_HANDLE ? m_brdfLUTView : fallbackView;"}}

That dummy is a 1×1 RGBA8 image created with `VK_IMAGE_USAGE_SAMPLED_BIT` and
nothing else — it has no transfer usage, so no upload is even permitted — and it
receives one layout transition and no writes. Its texel is Vulkan-undefined, not
guaranteed zero.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;"}}

So the shader's "no real LUT bound" branch is not defensive paranoia about a
hypothetical: it is the only thing standing between the deferred metals and
whatever that allocation happens to contain. It fires when the sampled pair sums
below `1e-4` and substitutes an analytic `vec2(max(1-roughness, 0.04),
roughness*0.25)`. Whether the engine's environment-specular response today is
that analytic curve or an undefined texel is not something the code decides.

{{cite shaders/core/deferred_lighting.frag "brdf = vec2(max(1.0 - roughness, 0.04), roughness * 0.25);"}}

## Where the light record lives

The light record the lighting pass reads is not in this directory either. It is
`LightGpu` in `shaders/shared/layout.json`, generated into `shared/layout.glsl`,
`layout.slang` and `ohao/gpu/shared_layout.hpp`: type, range, intensity, the cone
cosines, the shadow-map index and the RT shadow channel ride in the `w` and `params`
channels of four `vec4`s, 64 bytes, std430.

{{cite shaders/shared/layout.json "x cos inner cone, y cos outer cone, z shadow-map index (-1 none), w RT shadow channel (-1 never, 0..kShadowMaxLights-1)"}}

The channel is -1 for a light that never casts, else the light's RT visibility
channel, 0 to 7: the first eight shadow-casting lights in light order own one each,
and a caster past them gets -1 and renders unshadowed.

{{cite ohao/render/scene/render_scene_types.hpp "inline constexpr uint32_t kMaxShadowLights = 8;"}}

## Contracts

- `light_attenuation.glsl` is compiled by nothing; editing it changes no image. Its default falloff and `deferred_lighting.frag`'s windowed inverse-square have different units, so intensities tuned against the deferred pass are not portable to anything that adopts the header law.
- Whatever attenuation law the lighting pass uses must be exactly 0 at and past `direction.w`: the froxel lists and the `attenuation == 0.0` skip both depend on it.
