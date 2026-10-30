---
module: path-tracer
id: raygen
title: Raygen integrator
standard: v2
---

## One raygen, both profiles

Both ray-tracing profiles bind the same ray-generation program: `PathTracerShaderSet`'s
default, the path tracer in `rt/path_tracer.slang`, whose four stages are entry points
of one Slang file:

{{cite ohao/render/rt/path_tracer.hpp "rt_path_tracer.slang.raygen.spv"}}
{{cite ohao/render/rt/rt_profile_renderer.hpp ": RTProfileRendererBase(kRealtimeRTSettings, PathTracerShaderSet{}) {}   // rt/path_tracer.slang"}}

The base installs the set on the `PathTracer` before the pipeline is built:

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}

The program is a correct estimator of OpenPBR's base, the reference the deferred frame
is judged by. The profiles differ only in settings: the realtime one traces two bounces
a frame for NRD to denoise, the offline one 32 into a running mean (see the profiles
page).

## One path a sample

`path_tracer.slang`'s raygen traces one path per pixel per dispatch through
OpenPBR's base BSDF (`material/openpbr.slang`). At every surface vertex it does
three things: next-event estimation to one light, next-event estimation to the
environment, and one BSDF sample for the next direction.

{{cite shaders/rt/path_tracer.slang "// Next-event estimation: a light, then the environment, each weighed against the BSDF."}}

The camera ray traces with `kRayMaskCamera` (scene, light emitters, first-person
guns); every later ray with the scene and the emitters only:

{{cite shaders/rt/path_tracer.slang "const PathPayload hit = trace(origin, dir, depth == 0u ? kRayMaskCamera : (kRayMaskScene | kTlasMaskEmitter), 1.0e4);"}}

So a bounce ray can meet a light's emitter mesh. That is how every light is found
twice, by light sampling and by BSDF sampling, and why the two strategies are
weighted against each other at any roughness. A ray that misses ends the path with
the environment's radiance; one that meets an emitter ends it with the light's.

## Choosing a light

`sampleLight` picks one light uniformly from the RT light buffer and draws a
direction by the light's shape:

- a **sphere** (point and spot lights) uniformly inside its cone of directions,
  with the solid-angle density $1/(2\pi(1-\cos\theta_{\max}))$ times the choice
  probability $1/N$;
- a **rectangle** (area lights) by area, converted to solid angle by
  $d^2/(\cos\theta_\ell A)$;
- a **sun** (directional) as a delta: its irradiance divided by the choice
  probability, with no density to weigh.

{{cite shaders/rt/path_tracer.slang "s.pdf = choose / (kTwoPi * oneMinusCosMax);"}}
{{cite shaders/rt/path_tracer.slang "s.pdf = choose * d2 / (cosL * l.area);"}}
{{cite shaders/rt/path_tracer.slang "s.radiance = l.radiance / choose;   // a delta: no density to weigh"}}

The radiances are the deferred frame's: a point light of intensity $I$ is a sphere
of radius $r$ (at least 1 cm) and radiance $I/(\pi r^2)$, windowed by its range, a
spot the same times its cone toward the shaded point, a rectangle $I/(4\pi r^2)$.
`1 - cos` is computed as $\sin^2/(1+\sqrt{1-\sin^2})$, which keeps a distant small
sphere from cancelling to zero.

{{cite shaders/rt/path_tracer.slang "const float oneMinusCos = sin2 / (1.0 + sqrt(1.0 - sin2));   // 1 - cos without cancellation"}}

The shadow ray stops at the sphere's near side (or 1 mm short of the rectangle's
point), so a light never occludes its own sample.

## Weighting the strategies

Light sampling and BSDF sampling estimate the same light, so each contribution is
weighted by the power heuristic. For a light sample drawn with density $p_\ell$ in
direction $\omega_i$:

$$C_{\text{NEE}} = T\,\frac{f(\omega_o,\omega_i)\cos\theta_i\,L(\omega_i)}{p_\ell}\,\frac{p_\ell^2}{p_\ell^2 + p_f^2}$$

$T$ is the path throughput, $f\cos\theta_i$ is what `OpenPBRBase::eval` returns, and
$p_f$ is the BSDF's density for the same direction (`OpenPBRBase::pdf`, the mixture
of its GGX and EON lobes). A bounce ray that meets the light's emitter, or escapes to
the environment, takes the mirror weight $p_f^2/(p_f^2 + p_\ell^2)$, with $p_f$ the
density the last bounce was drawn with and $p_\ell$ what light or environment
sampling would have had for that direction.

{{cite shaders/rt/path_tracer.slang "add = throughput * le * powerHeuristic(lastPdf, lightPdf);"}}
{{cite shaders/rt/path_tracer.slang "const float w = depth == 0u ? 1.0 : powerHeuristic(lastPdf, envPdf(dir));"}}

Two cases skip the weight. A sun is a delta, found only by light sampling. A light
without an emitter mesh (`LightTable::setEmitterLights` did not flag it) is also
found only by light sampling, so it is taken at full weight:

{{cite shaders/rt/path_tracer.slang "const float w = ls.pdf > 0.0 ? (ls.mis ? powerHeuristic(ls.pdf, base.pdf(wo, wi, tables)) : 1.0) / ls.pdf : 1.0;"}}

A bounce ray that meets an emitter learns which light it is from the emitter
instance's `emitterLight.w`, which the closest hit returns as `payload.light`;
`emitterRadiance` then recomputes the light's radiance toward the ray's origin and
the light-sampling density it would have had.

:::why
The power heuristic, not the balance heuristic: when one strategy's density is much
larger than the other's (a mirror against a large light, a small light against a
diffuse surface), the power heuristic gives the better one almost all the weight and
leaves less variance from the weaker one. Because the emitter meshes are in the
bounce ray's mask, no roughness threshold is needed to decide which strategy owns a
glossy reflection of a light.
:::

Emissive surfaces are not in the light list (the raster frame's emissive proxies are
left out of the RT buffer); a path finds them by BSDF sampling only, at full weight.

{{cite shaders/rt/path_tracer.slang "add = throughput * hit.emission;   // emissive surfaces: found by BSDF sampling only"}}

## The environment

When the environment has a CDF, each vertex draws one direction from it
(`sampleEnv`: a row by the marginal, a texel by the conditional, a point uniformly in
the texel) and weighs it against the BSDF in the same way. `envPdf` evaluates the
density of any direction from the same two tables, so the miss of a bounce ray is
weighted against exactly the density the sampler used. Its map is row 0 at the
zenith, as every environment lookup in the engine is:

{{cite shaders/rt/path_tracer.slang "const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / kPi);   // row 0 the zenith"}}

## The next direction, and when the path ends

`OpenPBRBase::sample` picks the specular lobe with the probability of its share of
the albedo's luminance, samples GGX's bounded visible normals or EON's diffuse lobe,
and returns $f\cos\theta/p$ with $p$ the mixture density. That weight multiplies the
throughput, and the density is kept for the next vertex's MIS.

{{cite shaders/rt/path_tracer.slang "const BsdfSample s = base.sample(wo, float3(rng.next(), rng.next(), rng.next()), tables);"}}

A sample below the geometric surface ends the path (the base has no transmission).
From the third vertex on, Russian roulette continues with probability
$q = \min(\max(T_r, T_g, T_b), 0.95)$ and divides the throughput by it, which keeps
the estimator unbiased:

{{cite shaders/rt/path_tracer.slang "const float q = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);"}}

The bounce cap is `params.w`'s low 16 bits; the offline profile asks for 32, which
roulette rarely reaches. A cap of 0 still adds the emitter or environment a direct
light sample's companion BSDF ray meets, so 0 is direct light, both strategies
weighted.

{{cite shaders/rt/path_tracer.slang "if (stop || depth > bounces) break;"}}

Random numbers are one PCG stream per pixel and sample, seeded from the pixel and
the sample index.

{{cite shaders/rt/path_tracer.slang "rng.state = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z * 0x9E3779B9u + 0x7F4A7C15u)));"}}

## Shadow rays carry no boolean

A visibility ray starts its payload as a hit, skips the closest-hit shader and stops
at the first hit; only the miss shader clears the flag. "The flag is clear" is the
unoccluded test.

{{cite shaders/rt/path_tracer.slang "p.flags = kHit;   // the closest hit is skipped: only the miss shader clears it"}}

Rays leave 1 mm off the surface along the geometric normal on the side the ray came
from, and a light or environment sample on the other side of that normal is
rejected.

## What the raygen writes

Besides radiance, the raygen writes the running mean of every sample since the view
last moved (binding 1, read from the previous frame's image at binding 3), the ACES
image at exposure 0.5 (binding 2), OIDN's albedo and normal, and NRD's and DLSS's
guides. The host says how many frames the mean already holds in `control.y`; 0 means
this frame's sample alone, which is what a temporal denoiser takes.

{{cite shaders/rt/path_tracer.slang "acc = (prev.rgb * prev.w + radiance) / count;"}}

Motion vectors are written whenever last frame's view-projection belongs to a rendered
frame (`control.z`), whether or not the trace accumulates, so a temporal denoiser can
reproject across a moved view:

{{cite shaders/rt/path_tracer.slang "if (pc.control.z != 0u)"}}

The NRD split is by the lobe the first vertex sampled: everything after a diffuse
first bounce goes to the diffuse image, after a specular one to the specular image.
Direct light at the first vertex goes to the diffuse image whole. The diffuse
radiance is demodulated by the first hit's diffuse albedo (floored at 0.03) and the
specular is not:

{{cite shaders/rt/path_tracer.slang "const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));"}}
{{cite shaders/rt/nrd_compose.comp "vec3 composed = diffRad * diffAlbedo + specRad * specColor;"}}

Read from the code: a metal's diffuse albedo is 0, so its direct specular, which sits
in the diffuse image, is multiplied by 0 when the compositor remodulates. The OIDN
path and the plain accumulation are not affected.

## 256 bytes of push constants

The push-constant block is three `mat4`s, two `uvec4`s and two `vec4`s, exactly 256
bytes, and a static assert holds it there. That 256 is a device limit, not a spec
floor: core Vulkan only guarantees 128 bytes.

{{cite ohao/render/rt/path_tracer.hpp "OHAO_ASSERT_GPU_LAYOUT(PTPushConstants, layout::kPTPushConstantsBytes);"}}
{{cite ohao/render/rt/path_tracer_render.cpp "pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces);"}}

:::key
Every path-traced pixel comes from `path_tracer.slang`: one path a sample, next-event
estimation to one light and to the environment, BSDF sampling of OpenPBR's base, every
strategy weighted by the power heuristic, Russian roulette from the third vertex. The
realtime profile is the same estimator at two bounces, its frames denoised instead of
averaged.
:::

## Contracts

- Both profiles run `PathTracerShaderSet`'s default. A set's entry names must match its SPIR-V (`raygen`, `closestHit`, `anyHit`, `miss`).
- A light found by both strategies must be weighted on both sides. A bounce ray meets an emitter only when its mask includes `kTlasMaskEmitter`; a light with no emitter mesh must not be flagged by `LightTable::setEmitterLights`, or its light sampling is halved for a hit that never comes.
- An emitter instance's `emitterLight.w` is its light's index in the RT light buffer. If the two orders differ, a bounce ray weighs one light's emission against another's density.
- Emissive surfaces reach the path tracer by BSDF sampling only. Putting proxy lights for them back into the RT buffer counts their light twice.
- A shadow ray's payload must start with `kHit` set; with the closest hit skipped, nothing else sets it on a hit.
- The bounce cap lives in `params.w`'s low 16 bits.
- `control.y` is the number of frames already in the running mean; the raygen reads the history image only when it is above zero.
