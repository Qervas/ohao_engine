---
module: sampling
id: mis
title: MIS heuristics
standard: v2
---

## Two ways to find a light

From a shading point a path tracer can reach a light two ways: sample the light —
a point on it, or a direction from the environment map's luminance CDF — and
shadow-trace toward it, or sample the BSDF, let the bounce ray travel, and take what
it meets. Both are unbiased estimators of the same integral, and each fails where
the other works. Light sampling wastes nearly every sample on a smooth surface,
where almost no light direction has meaningful BSDF response; BSDF sampling wastes
nearly every sample under a small bright source, which almost no BSDF direction
hits. Running both and adding counts the light twice.

Multiple importance sampling is the correction — scale each sample by how likely the
*other* strategy was to have produced that same direction. OHAO's path tracer,
`rt/path_tracer.slang`, which both RT profiles run, weighs every light with an
emitter mesh and the environment against BSDF sampling with the power heuristic.

## The power heuristic

$$w_A(\omega) = \frac{p_A(\omega)^\beta}{p_A(\omega)^\beta + p_B(\omega)^\beta}, \qquad w_A(\omega) + w_B(\omega) = 1$$

Here $p_A$ and $p_B$ are the two strategies' probability densities *evaluated at the
same direction* $\omega$, both in solid-angle measure, and $\beta$ the exponent:
$\beta = 1$ is Veach and Guibas' balance heuristic, $\beta = 2$ their power
heuristic. The guarantee is the second equation: because the weights sum to one at
every direction, the combined estimator stays unbiased no matter how badly the two
densities disagree.

The path tracer writes the power heuristic itself, and returns exactly 0 when both
densities are 0, so a direction neither strategy could produce contributes nothing
rather than a NaN:

{{cite shaders/rt/path_tracer.slang "return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;"}}

:::why
Veach's result is that the balance heuristic's variance exceeds that of any other
weighting by at most a small additive term, and that the power heuristic does better
in the common case where one strategy's density dominates the other's — a sharp
highlight under a large light, a small light over a rough surface. The path tracer
is the image the deferred frame is judged by, and two extra multiplies a weight are
not its cost.
:::

## The partition over every light

At every vertex the path tracer picks one light uniformly, so every light density it
reports carries the choice, $1/n$:

{{cite shaders/rt/path_tracer.slang "const uint i = min(uint(rng.next() * float(n)), n - 1u);"}}

The choice and the light's point are the first three coordinates of one 4-D Sobol
point (the vertex's light group, *Owen-scrambled Sobol*), so they are stratified
together across a pixel's samples.

A sphere (a point or spot light, at least 1 cm) is sampled uniformly over its cone
of directions, a rectangle by its area, a sun along its direction. The bounce rays
that the BSDF draws are traced against the scene *and* the lights' emitter meshes —
the camera ray against everything the camera sees:

{{cite shaders/rt/path_tracer.slang "const PathPayload hit = trace(origin, dir, depth == 0u ? kRayMaskCamera : (kRayMaskScene | kTlasMaskEmitter), 1.0e4);"}}

An emitter mesh carries its light's index in its instance row, and the closest hit
returns it, so a bounce ray that lands on a light knows which light it found:

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}

`emitterRadiance` then gives that light's radiance toward the ray's origin and the
density light sampling had for the same direction, and the bounce's contribution is
weighed against it with the BSDF density the ray was drawn with:

{{cite shaders/rt/path_tracer.slang "add = throughput * le * powerHeuristic(lastPdf, lightPdf);"}}

The light-sampling side mirrors it. A sun is a delta, so no bounce ray can find it
and its sample is taken at full weight; a sphere or rectangle with an emitter mesh is
weighed against the BSDF's density; a light *without* an emitter mesh is found only
by light sampling, and takes full weight too:

{{cite shaders/rt/path_tracer.slang "// A sun is a delta (its radiance already over its choice); a light without an"}}
{{cite shaders/rt/path_tracer.slang "const float3 c = throughput * f * ls.radiance * w;"}}

Which lights have a mesh is the scene's to say. `LightTable::setEmitterLights`
receives the RT indices of the lights RenderScene made an emitter instance for, and
the upload flags them in a spare word of each light — `extra.w` for a rectangle,
`extra2.z` for a sphere:

{{cite ohao/render/scene/light_table.cpp "if (rt[i].positionAndType.w > 2.5f) rt[i].extra.w = 1.0f;   // a rectangle"}}
{{cite shaders/rt/path_tracer.slang "l.emitter = l.type == 3u ? e.w > 0.5 : (l.type != 1u && f.z > 0.5);"}}

The flag is load-bearing in both directions. A light weighed against BSDF sampling
when no bounce ray can meet it loses the share the BSDF side would have carried; a
light taken at full weight while its mesh is also found adds its light twice.

The environment is the same pair. A direction drawn from the map's CDF is weighed
against the BSDF:

{{cite shaders/rt/path_tracer.slang "const float3 c = throughput * f * envRadiance(L) * powerHeuristic(pdfL, base.pdf(wo, wi, tables)) / pdfL;"}}

and a bounce ray that leaves the scene against the CDF — except the camera ray,
which no light strategy could have produced:

{{cite shaders/rt/path_tracer.slang "const float w = depth == 0u ? 1.0 : powerHeuristic(lastPdf, envPdf(dir));"}}

What is left out is emissive surfaces. They are found by BSDF sampling alone, at
full weight:

{{cite shaders/rt/path_tracer.slang "add = throughput * hit.emission;   // emissive surfaces: found by BSDF sampling only"}}

and the RT light buffer does not hold the sphere proxies the raster frame makes
for them, so nothing else finds them:

{{cite ohao/render/scene/light_table.cpp "// meets the surfaces themselves, and a proxy light beside them would count their light twice."}}

:::key
Every light with a mesh and the environment sit in a two-strategy partition, and
every other source is found by exactly one strategy. A source found
by both strategies without a weight would be counted twice; one found by neither
would be dark.
:::

The BSDF density on the other side of every one of those weights is the mixture the
sampler actually draws from: `OpenPBRBase::sample` picks the specular lobe with
probability `specularShare` and the diffuse lobe otherwise, and `pdf` reports the
same mixture for any direction, so the density a bounce ray was drawn with and the
density a light sample is weighed against are one function.

{{cite shaders/material/openpbr.slang "return ps * ggxPdfBounded(wo, wi, alpha) + (1.0 - ps) * eonPdf(wo, wi, diffRough);"}}

## Both sides must be quoted in solid angle

A ratio of densities means nothing unless both are in the same measure. The env
sampler draws from a 2D CDF built over equirectangular pixels — a density in UV
space — and converts to solid angle by dividing out the Jacobian of the mapping:

$$p_\Omega(\omega) = \frac{p_{uv}(u,v)}{2\pi^2 \sin\theta}$$

$p_{uv}$ is the luminance-proportional density over the normalised $(u,v)$ square,
$\theta$ the polar angle, and $2\pi \cdot \pi \cdot \sin\theta$ the area stretch at
that latitude — which is why the poles, where $\sin\theta \to 0$, need the `1e-4`
floor before dividing:

{{cite shaders/rt/path_tracer.slang "return max(cond * marg * float(W) * float(H) / (kTwoPi * kPi * max(sin(theta), 1e-4)), 0.0);"}}

The light densities are in the same measure. A sphere's cone of
directions has solid angle $2\pi(1-\cos\theta_{max})$, so a uniform direction in it
has density $1/(2\pi(1-\cos\theta_{max}))$, computed as
$\sin^2/(1+\sqrt{1-\sin^2})$ so that a distant small sphere does not cancel to 0:

{{cite shaders/rt/path_tracer.slang "return 1.0 / (kTwoPi * oneMinusCos);"}}

A rectangle is sampled uniformly by area, $1/A$, and turned into solid angle by
$d^2/\cos\theta_L$, the light-side cosine:

{{cite shaders/rt/path_tracer.slang "s.pdf = choose * d2 / (cosL * l.area);"}}

The BSDF side's environment density is evaluated by the raygen itself: the miss
shader only clears the payload's hit flag, and the escaped bounce ray calls
`envPdf(dir)`, the same function the light-sampling side uses, so both halves of
the weight are one density.

## No delta lobe

The power heuristic assumes both strategies have finite densities; a mirror lobe's
is a Dirac delta. The path tracer has no delta lobe: its GGX is never sharper than
roughness 0.01 — alpha $10^{-4}$, a mirror at any renderer's resolution — so every
density it produces is finite and mirrors are weighed like any other surface, with
no threshold at which a light changes strategy.

{{cite shaders/material/openpbr.slang "static const float kMinSpecularRoughness = 0.01;"}}

## Contracts

- Both densities must be in solid-angle measure. A producer returning UV or area density rebalances the weights instead of failing.
- Argument order encodes which strategy is weighted: the first density's strategy gets the weight. Swapping the arguments inverts the partition, and the symptom is a brightness shift, not a crash.
- A light's emitter flag (`LightTable::setEmitterLights`) must match whether a bounce ray can meet its mesh. A flagged light without a mesh loses energy; an unflagged light with one counts twice.
- A bounce ray's density (`lastPdf`) and a light sample's BSDF density must both be `OpenPBRBase::pdf`, the mixture the sampler draws from.
- The environment's two sides must both call `envPdf`; a second environment strategy must report its density through it.
