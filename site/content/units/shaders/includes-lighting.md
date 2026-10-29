---
module: shaders
id: includes-lighting
title: Lighting includes
standard: v2
figures: [shaders-includes-lighting-falloff]
---

## What the include graph actually says

`shaders/includes/lighting/` holds two files that have nothing to do with each other.
`light_attenuation.glsl` is a GLSL header no stage includes; the only `#include` it
takes part in is its own pull of `math.glsl`.

{{cite shaders/includes/lighting/light_attenuation.glsl "includes/common/math.glsl"}}

`clustered_lights.slang` is a Slang module, and it is what ships: the froxel lists, each
light's attenuation, and the surface's response through OpenPBR's base. Three passes
import it — deferred lighting, the forward transparent pass and the particle expand —
so a plane shaded forward and deferred runs the same loop.

{{cite shaders/includes/lighting/clustered_lights.slang "// The clustered light loop of the deferred frame and the forward passes (core/deferred_lighting.slang,"}}

:::key
Treat `light_attenuation.glsl` as a proposal, not an implementation. No shader includes
it, and the CMake source glob compiles only stages, so the header is not even parsed by a
shader build. The module beside it re-derives the spot-cone ramp and the light direction
and rejects the header's distance law. The interesting failures are where the two
disagree.
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

`clustered_lights.slang` declares its own `calculateAttenuation`, and picks a different
law — Karis-style windowed inverse-square:

$$a_{\text{def}}(d) \;=\; \frac{1}{\max(d^{2},\,0.01)}\;\Big(\mathrm{clamp}\big(1 - (d/r)^{4},\,0,\,1\big)\Big)^{2}$$

Same symbols. This one is correct in the far field and clamps the $1/d^{2}$
singularity at 100 rather than removing it, with a quartic window that reaches
zero at $d = r$ while staying near 1 over most of the light's useful volume.

{{cite shaders/includes/lighting/clustered_lights.slang "const float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);"}}

That exact zero is load-bearing. Clustered lighting lists, for each froxel, only the
lights whose range sphere reaches it, and `directLight` skips a light whose attenuated
radiance is exactly 0; both are exact only because past $d = r$ the clamp returns 0, not
a small number.

{{cite shaders/includes/lighting/clustered_lights.slang "if (all(radiance == float3(0.0)) && type != 3) continue;   // adds exactly zero: past its range or cone"}}
 The `LightGpu` record states the same contract in its field
documentation:

{{cite shaders/shared/layout.json "w range: point and spot light contributes nothing past it"}}

The header already contains the near-match — `attenuationUE4` — with zero
callers. It also differs from the deferred version in a way that changes units:
it normalises distance by range *before* squaring, so its core is dimensionless
and bounded by 1.

{{cite shaders/includes/lighting/light_attenuation.glsl "float d = distance / range;"}}

{{figure shaders-includes-lighting-falloff "Conceptual, not measured: the three laws evaluated directly from the formulas in light_attenuation.glsl and the deferred light loop (now clustered_lights.slang), each normalised to 1 at d = 0.05·range so the shapes can be compared on one axis. Log vertical scale; curves are clipped where they fall below 1e-3, though all three reach exactly zero at d = range."}}

:::why
One light record, two incompatible falloff laws. The header's default is
dimensionless and peaks at 1; the deferred law carries 1/length² units and peaks
at 100. They are not related by any constant — the ratio between them varies
with both distance and range — so an intensity tuned against one is simply wrong
under the other. That is the reason not to "tidy" the light loop onto
this header: including it is free, but switching the law re-authors every light
intensity in every scene. If a shared include is wanted, the function to share
is the deferred one, and the header's `attenuationUE4` is the closest starting
point. Any replacement must keep a hard zero at the range, or clustered lighting
stops being exact.
:::

## The IBL the deferred pass evaluates instead

The header directory has no image-based lighting of its own. The module keeps two
helpers for it, `specularOcclusion` and `equirectUV` (the environment images' texel
coordinate, row 0 the zenith); the split sum itself is in `deferred_lighting.slang`, fed
by `EnvPrefilter`'s two equirectangular images (the environment GGX-prefiltered into one
mip per roughness step, and its irradiance; the deferred lighting unit, "Image-based
lighting"). Its weight is no analytic fit: it is the specular lobes' own directional
albedo toward the viewer, read from OpenPBR's energy tables.

{{cite shaders/core/deferred_lighting.slang "const float3 specularAlbedo = s.bsdf.specularAlbedo(s.wo, tables);"}}

{{cite shaders/includes/lighting/clustered_lights.slang "float specularOcclusion(float NdotV, float ao, float roughness)"}}

## Where the light record lives

The light record the lighting pass reads is not in this directory either. It is
`LightGpu` in `shaders/shared/layout.json`, generated into `shared/layout.glsl`,
`layout.slang` and `ohao/gpu/shared_layout.hpp`: type, range, intensity, the cone
cosines, the sphere radius and the RT shadow channel ride in the `w` and `params`
channels of four `vec4`s, 64 bytes, std430.

{{cite shaders/shared/layout.json "x cos inner cone, y cos outer cone, z the sphere radius of a point or spot light with an emitter mesh"}}

A non-zero radius turns the light's specular into a sphere light's (`evaluateSphere` in
`clustered_lights.slang`): the representative point on the sphere closest to the reflection
ray, and OpenPBR's specular lobes re-evaluated at the GGX alpha widened by the sphere's angular
size, `a' = a + r / (2d)` (Karis 2013), through `OpenPBRBase.withRoughness`; the diffuse is
evaluated toward the centre. On a mirror that is a disc at the mesh's radiance `I / (pi r^2)`
times the Fresnel, the light's emitter mesh reflected, wherever the light is: on screen or off,
with SSR or without.

{{cite shaders/includes/lighting/clustered_lights.slang "const float aSphere = clamp(s.bsdf.alpha + radius / (2.0 * length(toLight)), 0.0, 1.0);"}}

The channel is -1 for a light that never casts, else the light's RT visibility
channel, 0 to 7: the first eight shadow-casting lights in light order own one each,
and a caster past them gets -1 and renders unshadowed.

{{cite ohao/render/scene/render_scene_types.hpp "inline constexpr uint32_t kMaxShadowLights = 8;"}}

## Contracts

- `light_attenuation.glsl` is compiled by nothing; editing it changes no image. Its default falloff and `clustered_lights.slang`'s windowed inverse-square have different units, so intensities tuned against the deferred pass are not portable to anything that adopts the header law.
- Whatever attenuation law the lighting pass uses must be exactly 0 at and past `direction.w`: the froxel lists and `directLight`'s exact-zero skip both depend on it.
- `clustered_lights.slang` is shared by deferred lighting, the transparent pass and the particle expand; a change to it moves all three, which is what keeps a forward-shaded plane equal to the deferred one.
