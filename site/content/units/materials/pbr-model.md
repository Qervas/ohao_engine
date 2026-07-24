---
module: materials
id: pbr-model
title: PBR metallic-roughness
standard: v2
figures: [ggx_multiscatter_energy]
---

## The lie artists can paint

The physically-correct microfacet BRDF wants a spectral, complex index of
refraction at every surface point. No artist authors that. OHAO collapses it to
three scalars an artist can paint into textures — base color, metallic, roughness
— and reconstructs the physics at shade time. This page is about what that
reconstruction gets right, and the one thing about OHAO that surprises people: it
reconstructs the *same three sliders* twice, on two different BRDF stacks that do
not agree.

## F0: the 0.04 that is not arbitrary

F0 is the Fresnel reflectance at normal incidence — the fraction of light a flat
surface bounces straight back when you look at it head-on. For a dielectric it is
fixed by the index of refraction n through F0 = ((n - 1)/(n + 1))^2. Common
dielectrics sit near n = 1.5, so:

$$F_0^{\text{dielectric}} = \left(\frac{1.5 - 1}{1.5 + 1}\right)^2 \approx 0.04$$

Metals have no meaningful single-scalar IOR in this model and absorb all
transmitted light within nanometers, so they get zero diffuse and reuse base
color as F0. Both pipelines collapse the two cases into one branch-free `mix`; in
the path tracer it sits in the raygen shading loop, just after the hit is
unpacked:

{{cite shaders/rt/pt_raygen.rgen "vec3 F0 = mix(vec3(0.04), albedo, metallic);"}}

:::key
The intermediate values of that lerp (metallic = 0.5) are not physical — no real
material is half-conductor. The path tracer keeps the slider continuous anyway,
because a continuous, monotonic control is all an artist needs and the pure 0/1
endpoints are exactly right. The deferred pipeline makes the opposite call — see
below — which is the first place the two stacks diverge.
:::

## One reduction, two BRDFs

Here is the fact that governs everything else on this page. OHAO's path-tracer and
its deferred rasteriser do not share a BRDF file. The path-tracer raygen shaders
include `ggx_aniso.glsl`:

{{cite shaders/includes/material/ggx_aniso.glsl "float ggxD_anisoOrIso(vec3 N, vec3 H, float NdotH, float roughness,"}}

while the deferred and forward fragment shaders call `evaluateBRDF` in
`brdf_ggx.glsl`. They agree on F0 and on being "GGX metallic-roughness," and they
diverge on almost everything expensive: the geometry term, energy compensation,
diffuse model, and even how the metallic slider is interpreted. The rest of this
page walks each stack, then the gap between them.

## The path tracer's specular

The default offline `PathTracer` binds `pt_raygen.rgen` (the shaders behind
`cornell_box`, `model_viewer`, `env_demo`, `turntable`). Its direct lighting is
next-event estimation: for each light sample it evaluates analytic Cook-Torrance
inline — the anisotropy-aware GGX D, a Schlick Fresnel, a Smith-Schlick geometry
term using the `(roughness + 1)^2 / 8` direct-lighting k remap, then a plain
Lambert diffuse (not Burley):

{{cite shaders/rt/pt_raygen.rgen "vec3 diff = kD * albedo / 3.14159;"}}

The D term is anisotropy-aware; at anisotropy = 0 it falls back to the exact
isotropic GGX so older reference renders stay bit-identical. The anisotropy
direction is not read from a tangent attribute — it is reconstructed by projecting
world-up onto the surface:

{{cite shaders/includes/material/ggx_aniso.glsl "t = normalize(ref - n * dot(ref, n));"}}

That detail matters more than it looks: Frisvad's cheaper basis varies
discontinuously with the normal, so on a sphere neighbouring pixels get scrambled
tangents and the anisotropic highlight averages back into something that looks
isotropic. Projecting a fixed world axis gives a smoothly-varying tangent (a
lathe-turned look on spheres), which is the whole point of shipping anisotropy.

Indirect bounces in the offline path are *not* VNDF-importance-sampled — they use
a cosine-weighted hemisphere sample narrowed by roughness, i.e. a roughness-widened
reflection lobe:

{{cite shaders/rt/pt_raygen.rgen "vec3 jitVec = cosineHemisphere(reflected, jitU) * roughness;"}}

The realtime RT profile (`pt_raygen_realtime.rgen`, the `--denoise=nrd`/DLSS path)
is the one that upgrades the indirect glossy bounce to Heitz-2018 sampling of the
*visible* normal distribution:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 Hloc = sampleGGXVNDF(Vloc, alpha, alpha, u);"}}

With a VNDF sample the Monte-Carlo estimator weight `f·cosθ / pdf` collapses to a
single height-correlated Smith ratio once Fresnel is factored out —

$$\frac{f\,\cos\theta_L}{p(\omega_L)} \;=\; F \cdot \frac{G_2(\omega_o,\omega_i)}{G_1(\omega_o)}$$

— so that branch returns exactly `F · G2/G1` instead of forming D and the pdf
separately:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

So VNDF sampling is a realtime-profile feature, not a property of the flagship
offline tracer — a distinction worth keeping straight when reading the raygen
family.

## The deferred pipeline's specular

The rasteriser cannot importance-sample per pixel, so `brdf_ggx.glsl` evaluates the
full analytic Cook-Torrance and spends the saved rays on quality. Its geometry term
is the height-correlated Smith form (Heitz 2014) — which, unlike the path tracer's
NEE term, needs no k remap at all:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float geometrySmithCorrelated(float NdotV, float NdotL, float roughness) {"}}

Then — the part most hobby renderers omit — it adds back the energy single-scatter
GGX loses at high roughness (Kulla-Conty 2017 / Turquin 2019). Single-scatter
models exactly one bounce off the microsurface, so rough metal comes out visibly
too dark; the compensation term redistributes the lost energy:

$$E_{ms} = (1 - E(\mu_o))\,(1 - E(\mu_i)), \qquad f_{ms} = \frac{F_{avg}\,E_{ms}}{1 - F_{avg}\,(1 - E(\mu_o))}$$

where E is the directional albedo of single-scatter GGX, taken from a polynomial
fit rather than a lookup table:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float Ems = (1.0 - E_o) * (1.0 - E_i);"}}
{{cite shaders/includes/brdf/brdf_ggx.glsl "vec3 Favg = surface.F0 + (1.0 - surface.F0) / 21.0;"}}

{{figure ggx_multiscatter_energy "Single-scatter GGX droops with roughness; the deferred pipeline's compensation term returns total energy to ~1. Shape from the GGX_E() fit in brdf_ggx.glsl — conceptual, not a captured render. The path tracer (ggx_aniso.glsl) has no equivalent term."}}

The deferred stack also reinterprets the metallic slider itself. Where the path
tracer keeps it continuous, the deferred surface setup sharpens it toward the pure
endpoints before computing F0:

{{cite shaders/includes/brdf/brdf_common.glsl "float sharpenedMetallic = smoothstep(0.35, 0.65, metallic);"}}

so a painted metallic of 0.5 reads as a cleaner dielectric-or-metal decision under
raster lighting — cheaper to make look right, at the cost of the smooth half-metal
ramp the path tracer preserves.

## Where the two paths disagree

:::why
Because multi-scatter compensation lives only in `brdf_ggx.glsl`, and the offline
path tracer runs `ggx_aniso.glsl` with no such term, a rough metal is
energy-compensated in the deferred viewport but single-scatter-dark in the final
path-traced frame. Look-dev done against the fast deferred preview will therefore
*under*-brighten rough metals for the offline render. This is a real portability
gap, not a matched pair — closing it means porting the Kulla-Conty term into the
raygen, not pretending it is already there.
:::

## The k-remaps, and where each one lives

A Smith-Schlick geometry approximation needs a roughness remap, and engine docs
love to get this wrong, so here is the honest map for OHAO. There are three
distinct geometry treatments in the tree:

- **Path-tracer NEE direct light** uses the classic `(roughness + 1)^2 / 8`
  direct-lighting remap, inlined in the analytic Cook-Torrance block cited above
  (`pt_raygen.rgen`, repeated once per NEE branch across all three raygen
  variants). This is the shipping direct-light geometry term — very much alive.
- **Path-tracer realtime VNDF indirect** uses height-correlated Smith
  (`smithG2overG1GGX`), which needs no remap.
- **Deferred image-based lighting** precomputes the split-sum BRDF integral with a
  *different* remap, `(a*a)/2`, into a LUT that the deferred lighting pass samples:

{{cite shaders/compute/brdf_lut.comp "float k = (a * a) / 2.0;"}}

The only thing that is genuinely dead here is the *named* `geometrySchlickGGX`
function in `brdf_ggx.glsl` (no callers); the identical `(r+1)^2/8` math it
contains still ships, inlined, as the path tracer's NEE geometry term. "Function
unused" and "formula unused" are not the same claim, and only the first is true.

## Where the packed roughness comes from

At the closest-hit shader the material arrives packed into a payload vector, and
the unpack carries a backward-compatibility story worth knowing before you touch
it. The current encoding is continuous roughness in `.x`, metallic in `.y`:

{{cite shaders/includes/pbr_unpack.glsl "if (att.x < 0.0 && att.y < 1e-4) {"}}

A legacy signed encoding (negative `.x` meant "binary metal") is still accepted
when `.y` is ~0, so old shader-binding-table payloads authored mid-session do not
silently break. Removing that branch is safe only once every producer writes the
new form.

## Contracts

- Both stacks derive F0 = mix(0.04, albedo, metallic) — but the deferred path first sharpens metallic via `smoothstep(0.35, 0.65)`, so the F0 they feed differs at mid-slider values. Metals get zero diffuse; dielectrics F0 = 0.04.
- Roughness is clamped away from 0 (`max(roughness, 0.01)` in the unpack) so the specular lobe never becomes a zero-width delta that fireflies in the path tracer.
- The offline path tracer (`ggx_aniso.glsl`) and deferred pipeline (`brdf_ggx.glsl`) are NOT energy-matched at high roughness: multi-scatter compensation is deferred-only. Treat the deferred preview as an approximation of the offline result for rough metals, not ground truth.
- VNDF specular importance sampling is specific to the realtime RT profile (`pt_raygen_realtime.rgen`); the default offline tracer uses analytic NEE plus cosine-jittered indirect bounces.
