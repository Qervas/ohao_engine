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
a few scalars an artist can paint into textures — base color, metallic, roughness,
and OpenPBR's specular weight, colour and IOR beside them — and reconstructs the
physics at shade time. This page is about what that reconstruction gets right, and
about the one split that remains: the same sliders are reconstructed by two surface
models — OpenPBR's base, which the reference path tracer, the deferred frame, the
forward transparent pass and RT GI's hits all evaluate, and a GLSL metal-roughness
Cook-Torrance in the realtime tracer — and the two do not agree.

## F0: the 0.04 that is not arbitrary

F0 is the Fresnel reflectance at normal incidence — the fraction of light a flat
surface bounces straight back when you look at it head-on. For a dielectric it is
fixed by the index of refraction n through F0 = ((n - 1)/(n + 1))^2. Common
dielectrics sit near n = 1.5, so:

$$F_0^{\text{dielectric}} = \left(\frac{1.5 - 1}{1.5 + 1}\right)^2 \approx 0.04$$

Metals have no meaningful single-scalar IOR in this model and absorb all
transmitted light within nanometers, so they get zero diffuse and reuse base
color as F0. The realtime GLSL tracer collapses the two cases into one branch-free
`mix` in its raygen shading loop, just after the hit is unpacked:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 F0 = mix(vec3(0.04), albedo, metallic);"}}

OpenPBR's base keeps the IOR instead of its F0. A material's `specular_ior`
(default 1.5) gives the dielectric its exact Fresnel at every angle, and
`specular_weight` scales that F0 by returning the IOR that has the scaled value;
a metal's F0 is `base_weight × base_color`:

{{cite shaders/material/openpbr.slang "eta = clamp(modulatedIor(m.specularIor, m.specularWeight), 1.0, kPbrEtaMax);"}}

{{cite shaders/material/openpbr.slang "metalF0 = rho;"}}

:::key
The intermediate values of the metallic slider (metallic = 0.5) are not physical —
no real material is half-conductor. Every renderer keeps the slider continuous
anyway, because a continuous, monotonic control is all an artist needs and the pure
0/1 endpoints are exactly right; OpenPBR mixes its metal and dielectric lobes by it,
and the realtime tracer mixes F0 and the diffuse weight by it.
:::

## One input set, two BRDFs

Here is the fact that governs everything else on this page. The offline reference,
`rt/path_tracer.slang`, and the deferred lighting shader, `core/deferred_lighting.slang`,
both import OpenPBR's base from `material/openpbr.slang`; each resolves its material into
`MaterialInputs`, OpenPBR's names — the path tracer's closest hit from the material rows
and textures, deferred lighting from the G-buffer:

{{cite shaders/material/openpbr.slang "struct MaterialInputs"}}

{{cite shaders/core/deferred_lighting.slang "mi.baseDiffuseRoughness = pbr.r;"}}

The realtime raygen includes `ggx_aniso.glsl` instead:

{{cite shaders/includes/material/ggx_aniso.glsl "float ggxD_anisoOrIso(vec3 N, vec3 H, float NdotH, float roughness,"}}

The two agree on being "GGX metallic-roughness," and diverge on almost everything
expensive: the Fresnel, the geometry term, energy compensation and the diffuse model.
The rest of this page walks each, then the gaps between them.

## The reference's base

`OpenPBRBase` is OpenPBR Surface's base layer: a metal lobe and a dielectric lobe
over a diffuse one, mixed by `base_metalness`. The metal is GGX with the F82-tint
Fresnel; the dielectric is GGX with the exact Fresnel of its IOR, tinted by
`specular_color`, over EON diffuse (Portsmouth, Kutz and Hill 2025), which takes
what the specular does not reflect toward the viewer:

{{cite shaders/material/openpbr.slang "(1.0 - dielectricAlbedo(wo.z, t)) * eonEval(rho, diffRough, wi, wo) * wi.z;"}}

GGX is single scatter plus Kulla-Conty's multiple-scattering lobe, so a white metal
and a white dielectric over a white base reflect what they receive. The lobe's
directional albedos come from tables generated from the module itself
(`tools/material/openpbr_tables`), compiled into `openpbr_tables.hpp` and uploaded
once to binding 38:

{{cite shaders/material/openpbr.slang "return (1.0 - t.E1(wo.z, rough)) * (1.0 - t.E1(wi.z, rough)) / (kPbrPi * max(1.0 - e1Avg, 1e-6)) * wi.z;"}}

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(38, BUFFER, RGEN);  // OpenPBR's energy tables (rt/path_tracer.slang)"}}

The base samples as well as evaluates: the specular lobe by bounded VNDF sampling,
the diffuse by EON's own sampler, chosen by each lobe's share of the albedo's
luminance, and the path tracer weighs light samples against that density. The
GGX unit has the details.

## The realtime tracer's specular

The realtime profile binds `pt_raygen_realtime.rgen` (the `--denoise=nrd`/DLSS
path). Its direct lighting is next-event estimation: for each light sample it
evaluates analytic Cook-Torrance inline — the anisotropy-aware GGX D, a Schlick
Fresnel, a Smith-Schlick geometry term using the `(roughness + 1)^2 / 8`
direct-lighting k remap, then a plain Lambert diffuse (not EON):

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 diff = kD * albedo / 3.14159;"}}

The D term is anisotropy-aware; at anisotropy = 0 it falls back to the exact
isotropic GGX. The anisotropy direction is not read from a tangent attribute — it
is reconstructed by projecting world-up onto the surface:

{{cite shaders/includes/material/ggx_aniso.glsl "t = normalize(ref - n * dot(ref, n));"}}

That detail matters more than it looks: Frisvad's cheaper basis varies
discontinuously with the normal, so on a sphere neighbouring pixels get scrambled
tangents and the anisotropic highlight averages back into something that looks
isotropic. Projecting a fixed world axis gives a smoothly-varying tangent (a
lathe-turned look on spheres).

Its first glossy bounce is Heitz-2018 sampling of the *visible* normal
distribution:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 Hloc = sampleGGXVNDF(Vloc, alpha, alpha, u);"}}

With a VNDF sample the Monte-Carlo estimator weight `f·cosθ / pdf` collapses to a
single height-correlated Smith ratio once Fresnel is factored out —

$$\frac{f\,\cos\theta_L}{p(\omega_L)} \;=\; F \cdot \frac{G_2(\omega_o,\omega_i)}{G_1(\omega_o)}$$

— so that branch returns exactly `F · G2/G1` instead of forming D and the pdf
separately:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

Deeper bounces in that raygen are *not* VNDF-sampled — they use a cosine-weighted
hemisphere sample narrowed by roughness, i.e. a roughness-widened reflection lobe,
tinted by the metal colour:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput *= mix(vec3(1.0), bAlbedo, bMetallic);"}}

## The deferred frame's evaluation

The rasteriser cannot importance-sample per pixel, so it evaluates OpenPBR's base
analytically, light by light, through `includes/lighting/clustered_lights.slang`. The
BSDF is the reference's; what differs is how a light is integrated. A sphere light's
specular is taken at the point of the sphere nearest the reflection with the lobe
widened by its angular size, its diffuse toward its centre; a rectangle's diffuse by its
exact irradiance and its specular at the representative point. `evalLobes` returns the
two lobes separately for that purpose:

{{cite shaders/material/openpbr.slang "void evalLobes(float3 wo, float3 wi, OpenPBRTables t, out float3 specular, out float3 diffuse)"}}

Single scatter models exactly one bounce off the microsurface, so rough metal comes out
visibly too dark; Kulla-Conty's lobe (2017) returns the lost energy:

$$E_{ms} = (1 - E(\mu_o))\,(1 - E(\mu_i)), \qquad f_{ms} = \frac{E_{ms}}{\pi\,(1 - E_{avg})}\cdot\frac{F_{avg}^{2}\,E_{avg}}{1 - F_{avg}(1 - E_{avg})}$$

where E is the directional albedo of single-scatter GGX and $E_{avg}$ its cosine-weighted
average, both read from the same tables the path tracer uses, and $F_{avg}$ the lobe's Fresnel average:

{{cite shaders/material/openpbr.slang "return favg * favg * e1Avg / (1.0 - favg * (1.0 - e1Avg));"}}

Deferred lighting binds the tables at binding 20, the path tracer at 38.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "// 20: OpenPBR's energy tables (SSBO)"}}

{{figure ggx_multiscatter_energy "Single-scatter GGX droops with roughness; a multiple-scattering compensation term returns total energy to ~1. Conceptual, not a captured render. OpenPBR's base (the reference, the deferred frame, the transparent pass) does it from tabulated albedos; the realtime tracer (ggx_aniso.glsl) has no equivalent term."}}

The environment's specular is the split sum, the prefiltered environment times the
specular lobes' own directional albedo (`specularAlbedo`, the same tables), and every
indirect diffuse term is weighed by the diffuse lobe's albedo beneath the specular
(`diffuseAlbedo`), so a metal takes no diffuse fill at all.

## Where the paths disagree

:::why
The deferred frame and the reference evaluate one model, so a material's look no longer
depends on which of them renders it; what still separates them is transport (one GI
bounce, the split-sum environment, representative points for area and sphere specular).
The realtime tracer keeps its own GLSL model: Schlick at F0 0.04, uncorrelated Smith in
its NEE, Lambert diffuse and no multiple-scattering term, so its rough metals are darker
than either and its rough dielectric silhouettes brighter.
:::

## The k-remaps, and where each one lives

A Smith-Schlick geometry approximation needs a roughness remap, and engine docs
love to get this wrong, so here is the honest map for OHAO. There are three
distinct geometry treatments in the shaders that run:

- **Realtime NEE direct light** uses the classic `(roughness + 1)^2 / 8`
  direct-lighting remap, inlined in the analytic Cook-Torrance block cited above,
  repeated once per NEE branch of `pt_raygen_realtime.rgen`.
- **Realtime VNDF first bounce** uses height-correlated Smith
  (`smithG2overG1GGX`), which needs no remap.
- **OpenPBR's base** uses height-correlated Smith from the Λ function in
  `ggxSingle`: the reference's light samples and bounces, the deferred frame's lights,
  and (through the tables generated from it) the split sum's weight; no remap.

The `(r+1)^2/8` Schlick-k remap has no named function in the tree; it ships inlined as
the realtime tracer's NEE geometry term, so a search for a function name finds nothing
while the formula still runs.

## Where the packed roughness comes from

At the realtime tracer's closest-hit shader the material arrives packed into a
payload vector. The encoding is continuous roughness in `.x`, metallic in `.y`, and the
unpack still accepts an older signed form:

{{cite shaders/includes/pbr_unpack.glsl "if (att.x < 0.0 && att.y < 1e-4) {"}}

A negative `.x` with `.y` near 0 is read as "binary metal". No producer in the tree
writes that form — the closest hit writes roughness clamped to at least 0.04 — so the
branch is unreachable (the shaders/rt-includes unit has the detail). The reference has no
packing step: its payload carries each OpenPBR input as its own field.

## Contracts

- The realtime tracer derives F0 = mix(0.04, albedo, metallic). OpenPBR's base — the reference and the deferred frame — derives a dielectric's reflectance from `specular_ior` and `specular_weight`, a metal's from base colour, and never forms the 0.04; the G-buffer carries those inputs (`kSpecular`, `kOpenPBR`) for it.
- Roughness is clamped away from 0 (`max(roughness, 0.01)` in the realtime unpack, `kMinSpecularRoughness` 0.01 in OpenPBR's base; the G-buffer stores it as authored) so the specular lobe never becomes a zero-width delta.
- The reference and the deferred frame are energy-compensated from the same tables; the realtime tracer is not. Its rough metals are darker than either.
- VNDF specular sampling is in the realtime raygen's first glossy bounce and in every vertex of the reference (bounded VNDF); the realtime raygen's deeper bounces use cosine-jittered mirror directions.
