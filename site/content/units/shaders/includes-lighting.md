---
module: shaders
id: includes-lighting
title: Lighting includes
standard: v2
figures: [shaders-includes-lighting-falloff]
---

## What the directory holds

`shaders/includes/lighting/` holds one file, `clustered_lights.slang`, a Slang module:
the froxel lists, each light's attenuation, and the surface's response through OpenPBR's
base. Three passes import it — deferred lighting, the forward transparent pass and the
particle expand — so a plane shaded forward and deferred runs the same loop.

{{cite shaders/includes/lighting/clustered_lights.slang "// The clustered light loop of the deferred frame and the forward passes (core/deferred_lighting.slang,"}}

:::key
There is one distance law for raster lights, and it lives here. Any pass that lights a
surface or a particle from the light table imports this module rather than writing its
own falloff; the exact zero at the range is what clustered lighting is built on.
:::

## The falloff the light loop evaluates

`calculateAttenuation` is Karis-style windowed inverse-square:

$$a(d) \;=\; \frac{1}{\max(d^{2},\,0.01)}\;\Big(\mathrm{clamp}\big(1 - (d/r)^{4},\,0,\,1\big)\Big)^{2}$$

$d$ is the surface-to-light distance and $r$ the light's range, which travels in the `w`
channel of the light's direction. The law is correct in the far field, clamps the $1/d^{2}$
singularity at 100 rather than removing it, and its quartic window reaches zero at
$d = r$ while staying near 1 over most of the light's useful volume, so range works as a
cutoff and not as a second intensity knob.

{{cite shaders/includes/lighting/clustered_lights.slang "const float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);"}}

That exact zero is load-bearing. Clustered lighting lists, for each froxel, only the
lights whose range sphere reaches it, and `directLight` skips a light whose attenuated
radiance is exactly 0; both are exact only because past $d = r$ the clamp returns 0, not
a small number.

{{cite shaders/includes/lighting/clustered_lights.slang "if (all(radiance == float3(0.0)) && type != 3) continue;   // adds exactly zero: past its range or cone"}}

The `LightGpu` record states the same contract in its field documentation:

{{cite shaders/shared/layout.json "w range: point and spot light contributes nothing past it"}}

{{figure shaders-includes-lighting-falloff "Conceptual, not measured: the windowed inverse-square law clustered_lights.slang evaluates, beside two other common falloff shapes drawn only for comparison (a bounded 1/(1 + d²/r²) core under a squared linear window, and a range-normalised windowed form), each normalised to 1 at d = 0.05·range so the shapes can be compared on one axis. Log vertical scale; curves are clipped where they fall below 1e-3, though all three reach exactly zero at d = range."}}

:::why
The law is physical: a light of intensity $I$ gives $I/d^{2}$ at distance $d$, which is
also what the path tracer's sphere emitter delivers, so deferred and the reference agree
on direct light. Any bounded, range-normalised alternative would make range scale
brightness at every distance and break that agreement. Any replacement must keep a hard
zero at the range, or clustered lighting stops being exact.
:::

## The IBL the deferred pass evaluates instead

The split-sum image-based lighting is not in this module either. It keeps two
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

- Whatever attenuation law the lighting pass uses must be exactly 0 at and past `direction.w`: the froxel lists and `directLight`'s exact-zero skip both depend on it.
- `clustered_lights.slang` is shared by deferred lighting, the transparent pass and the particle expand; a change to it moves all three, which is what keeps a forward-shaded plane equal to the deferred one.
