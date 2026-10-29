---
module: materials
id: ggx
title: GGX implementation
standard: v2
figures: [materials-ggx-ndf-energy]
---

## The α convention every copy shares

The isotropic GGX distribution exists three times in the shaders that run, each behind a
name: `ggxDisoShaded` and `ggxDiso` in `ggx_aniso.glsl`, which only the realtime GLSL
raygen includes, and `ggxD` in `material/openpbr.slang`, the GGX of OpenPBR's base. That
last one is the GGX of everything else: the offline reference path tracer
(`rt/path_tracer.slang`), deferred lighting (`core/deferred_lighting.slang`, through
`includes/lighting/clustered_lights.slang`), the forward transparent pass and RT GI's hits
all import `material.openpbr`.

{{cite shaders/includes/material/ggx_aniso.glsl "Used by the path-tracer raygen for direct-light NEE, env-MIS, and BSDF"}}
{{cite shaders/core/deferred_lighting.slang "import material.openpbr;"}}

The one thing every copy agrees on without qualification is the width parameter. Each
resolves the artist's perceptual roughness to α = roughness² — `ggxDiso` at its call site,
`OpenPBRBase` in its constructor, `ggxDisoShaded` internally — and then squares α again
inside the Trowbridge-Reitz denominator:

$$D(\omega_h)=\frac{\alpha^{2}}{\pi\big[(n\cdot\omega_h)^{2}(\alpha^{2}-1)+1\big]^{2}},\qquad \alpha=\text{roughness}^{2}$$

Here $\omega_h$ is the half-vector between view and light, $n$ the shading
normal, and $\alpha$ the lobe width. $D$ is normalised so that
$\int D(\omega_h)(n\cdot\omega_h)\,d\omega_h = 1$ over the hemisphere — the
microfacets' projected areas must add back up to the macrosurface. That property
is load-bearing, and it is the one the realtime raygen's shaded copy loses:

{{cite shaders/includes/material/ggx_aniso.glsl "return a2 / (3.14159265 * denom * denom + 0.0001);"}}

OpenPBR's copy has no guard. It writes the denominator in the microfacet normal's
components, $m_x^2+m_y^2+m_z^2\alpha^2$, which is the same quantity as
$(n\cdot\omega_h)^2(\alpha^2-1)+1$ for a unit $m$ but never forms $\alpha^2-1$, a value
that rounds to −1 at small α:

{{cite shaders/material/openpbr.slang "const float d = dot(m.xy, m.xy) + m.z * m.z * a2;"}}

{{cite shaders/material/openpbr.slang "alpha = rough * rough;"}}

The double squaring is why a painted roughness of 0.1 is already a near-mirror:
α = 0.01, α² = 10⁻⁴. The realtime stack floors roughness at 0.04 in its closest hit, so
α never falls below 1.6 × 10⁻³ there. OpenPBR's base floors it at 0.01 (α = 10⁻⁴), a
mirror at any resolution, and needs no delta lobe because its D stays finite there; the
G-buffer stores roughness as authored and leaves the floor to the module, as the path
tracer does.

{{cite shaders/rt/pt_closesthit.rchit "roughness = max(roughness, 0.04);"}}

{{cite shaders/material/openpbr.slang "static const float kMinSpecularRoughness = 0.01;"}}

The α agreement is not cosmetic — a sampler whose α drifts from the D it
importance-samples renders a plausible-looking image with a quietly biased
estimator — and `ggx_aniso.glsl` spells the constraint out.

{{cite shaders/includes/material/ggx_aniso.glsl "convention here (alpha = roughness^2) MATCHES"}}

Five lines later the same comment block used to overreach. `ggxDiso` — the D
behind the realtime raygen's VNDF pdf — was introduced as "numerically identical"
to `ggxD_anisoOrIso`'s isotropic branch. The α convention did match; the guard did
not. The branch adds 10⁻⁴ to its denominator, `ggxDiso` added 10⁻⁸, and by the
quadrature below that factor of 10⁴ was the difference between retaining 2.7% and
91% of the distribution's mass at roughness 0.1. Both citations below are pinned
to `9e2d0b4`, the last revision that carried those lines:

{{cite shaders/includes/material/ggx_aniso.glsl@9e2d0b4 "identical to ggxD_anisoOrIso's isotropic branch"}}

{{cite shaders/includes/material/ggx_aniso.glsl@9e2d0b4 "return a2 / (3.14159265 * denom * denom + 1e-8);"}}

`ggxDiso`'s 10⁻⁸ is gone. The guard is now a floor on **α** rather than a term
added to the denominator: `max(α², 10⁻⁸)`, i.e. α ≥ 10⁻⁴, which is the roughness
0.01 floor `pbr_unpack.glsl` already applies. Nothing is added to the denominator
at all, so the function is exact Trowbridge-Reitz at whatever width it was handed
and integrates to 1 at *every* roughness — the flat curve in the figure below.

{{cite shaders/includes/material/ggx_aniso.glsl "float a2    = max(alpha * alpha, 1e-8);"}}

{{cite shaders/includes/material/ggx_aniso.glsl "return a2 / (3.14159265 * denom * denom);"}}

That widens the gap between the two functions rather than closing it, and the file
says so where a reader will hit it. It matters because the realtime raygen
computes the *same* quantity — the specular BSDF sampler's VNDF pdf — on both
sides of one MIS partition, and both sides must use the same D or the balance
heuristic is evaluated with two different densities:

{{cite shaders/rt/pt_raygen_realtime.rgen "float Dpdf = ggxDiso(NdotH, roughness * roughness);"}}

{{cite shaders/rt/pt_raygen_realtime.rgen "float pdfSpecMIS = smithG1GGX(NdotV, roughness * roughness) * Dpdf"}}

The shaded value `spec` on that same branch still calls `ggxD_anisoOrIso`. Only
the density moved.

## Masking-shadowing: correlated where it matters

OpenPBR's single-scatter lobe is $D\,G_2/4\mu_o$ (times $\cos\omega_i$) with Heitz's
height-correlated $G_2$, written from the Smith Λ function:

$$G_2(\omega_o,\omega_i)=\frac{1}{1+\Lambda(\omega_o)+\Lambda(\omega_i)},\qquad \Lambda(\theta)=\tfrac{1}{2}\!\left(-1+\sqrt{1+\alpha^{2}\tan^{2}\theta}\right)$$

with $\omega_o$ the view direction, $\omega_i$ the light direction and $\mu_o$ the cosine
of the view to the normal. The denominator is explicit, so there is no folded-in
visibility term to divide twice.

{{cite shaders/material/openpbr.slang "const float G2 = 1.0 / (1.0 + ggxLambda(wo.z, a2) + ggxLambda(wi.z, a2));"}}

Because deferred lighting evaluates the same `ggxSingle` through `OpenPBRBase.evalLobes`,
the rasteriser and the reference agree on masking-shadowing exactly, sphere and
rectangle lights included (their lobes are only widened, through `withRoughness`).

The realtime raygen does two other things. Its NEE branches build an *uncorrelated*
Schlick-k `G` and divide by `4 * NdotV * NdotL + 0.001` explicitly, a second additive
epsilon inside the same expression (four copies of that line; the light NEE copy is the
one directly above the near-mirror rule):

{{cite shaders/rt/pt_raygen_realtime.rgen "if (firstHitMirror) spec = vec3(0.0);   // its specular ray reflects the light's mesh"}}

and its first glossy bounce weighs a VNDF sample by $F\,G_2/G_1$ through
`smithG2overG1GGX`, which substituting Λ into $1/(1+\Lambda_o+\Lambda_i)$ makes the same
correlated $G_2$ — used as the estimator weight with the Fresnel factored out, not as a
shading G:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

So the realtime raygen's NEE branches are the one place that shades with the
uncorrelated Karis product.

## Fresnel: exact in two renderers, Schlick in the third

OpenPBR's base gives a dielectric the exact unpolarised Fresnel of its IOR (after
`specular_weight` has scaled its F0) and a metal the F82-tint form: Schlick minus a
$\mu(1-\mu)^6$ term fitted so the reflectance at $\mu = 1/7$ is `specular_color` times
Schlick's:

{{cite shaders/material/openpbr.slang "const float spec = fresnelDielectric(c, eta) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms;"}}

{{cite shaders/material/openpbr.slang "return saturate(F0 + (1.0 - F0) * m5 - b * (c * m5 * m));"}}

Both the path tracer and deferred lighting call it, for every light and, through
`specularAlbedo`, for the environment's split sum. The realtime raygen keeps textbook
Schlick with F90 = 1 at a fixed dielectric F0 of 0.04:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 F0 = mix(vec3(0.04), albedo, metallic);"}}

:::why
A rough dielectric under single-scatter GGX with full Schlick gets a bright rim at
grazing angles that nothing cancels. The deferred frame once hid it by clamping F90 to
`1 − roughness`, a Fresnel that matched no path-traced frame. OpenPBR removes the cause
instead: Kulla-Conty's multiple-scattering lobe returns the energy single-scatter GGX
loses, and the diffuse beneath is scaled by what the specular lets through, so the white
furnace holds with the exact Fresnel and the deferred silhouette matches the reference's.
:::

## The epsilon that eats the highlight

The two GLSL D expressions that run guard against a vanishing denominator in two
different ways, and only one of them is a floor. `ggxDisoShaded` — which is both
`ggxD_anisoOrIso`'s isotropic branch and every D the realtime raygen shades with —
*adds* 10⁻⁴. `ggxDiso` adds nothing: it floors α² upstream, which is the one arrangement
that costs no mass. OpenPBR's `ggxD` is the same in kind: the 0.01 roughness floor is
its only guard.

An additive term is not a floor — it never leaves the expression. At the lobe
peak the true denominator is $\pi\,\text{roughness}^{8}$, which at the realtime stack's
0.04 roughness floor is about 2 × 10⁻¹¹: seven orders of magnitude under the added
10⁻⁴, so the guard rather than the GGX algebra sets the height of the highlight. A
floor on α is a different kind of object: it substitutes a slightly wider — but
still correctly normalised — lobe, so it moves *where* the energy sits without
removing any.

Numerical quadrature of the guards shows how far each truncation reaches. `ggxDiso`
integrates to 1.000 at every roughness in the table — it truncates nothing. The +10⁻⁴
expression — the one on every NEE branch of the realtime raygen — does not recover until
roughness ≈ 0.4, and at roughness 0.2 it retains only 37% of the distribution's mass
(2.7% at roughness 0.1, 0.07% at the 0.04 floor). For scale, a 10⁻⁶ denominator floor
recovers by roughness ≈ 0.16 with 0.9% surviving at 0.04, and `ggxDiso` in its
pre-`9e2d0b4` +10⁻⁸ form sat between: 91% at roughness 0.1, within 1% of full by 0.14,
and 7% at the 0.04 floor.

{{figure materials-ggx-ndf-energy "Hemispherical integral of isotropic D guards versus perceptual roughness — measured by numerical quadrature of the GLSL, not a captured render. Two of the curves come from ggx_aniso.glsl, which pairs a 10⁻⁴ additive guard with an α floor that loses nothing; the 10⁻⁶ denominator floor is a comparison curve that no shader in the tree uses; the dashed curve is the 10⁻⁸ additive guard ggxDiso carried until 9e2d0b4. A correct NDF integrates to 1 everywhere; the missing mass is the peak of the specular lobe."}}

Where it bites matters as much as how much. The realtime profile binds the GLSL
raygen:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}

There D enters every light NEE and environment-MIS branch, at bounce 0 and on
both the specular and the diffuse chain, so the truncation dims specular NEE at
every bounce. What it does not touch is indirect specular throughput. The first
glossy bounce draws from the VNDF and weighs by $F\,G_2/G_1$; past it, the chain
reflects a mirror direction jittered by a roughness-scaled cosine lobe and tints
by the metal colour:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput *= mix(vec3(1.0), bAlbedo, bMetallic);"}}

Its MIS pdf is where D re-enters that chain:

{{cite shaders/rt/pt_raygen_realtime.rgen "specLastBsdfPdf = specProb * pdf_spec;"}}

So a chrome sphere still reflects the environment at full strength. What shrinks
is the direct-light highlight on smooth surfaces, and the MIS weight deciding how
much environment radiance the specular ray is allowed to keep. The offline
reference has none of this: its D is exact, its samples come from bounded VNDF
sampling (Eto and Tokuyoshi 2023) with the matching density, and its estimator
weight is the BSDF over that density.

{{cite shaders/material/openpbr.slang "s.pdf = ps * ggxPdfBounded(wo, s.wi, alpha) + (1.0 - ps) * eonPdf(wo, s.wi, diffRough);"}}

{{cite shaders/material/openpbr.slang "if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;"}}

:::key
The `+ 0.0001` in the realtime raygen's D is not a division guard; it is an
undeclared highlight clamp on smooth materials. A roughness-0.2 surface loses
roughly two thirds of its NEE specular before any firefly clamp runs.

{{cite ohao/render/rt/rt_settings.hpp ".enableFireflyClamp = true,"}}

There is a free repair, and `ggxDiso` and OpenPBR's `ggxD` have both taken it:
floor **α** instead of guarding the denominator, and the expression stays exactly
normalised at every roughness. The realtime raygen's shaded D keeps its 10⁻⁴; the
reference the golden corpus renders with, and the deferred frame, are not built on it.
:::

## The anisotropic branch and the normalisation it drops

The anisotropic path of `ggxD_anisoOrIso` builds an elliptical GGX from a
tangent frame projected off world-up. The standard Burley form is

$$D=\frac{1}{\pi\alpha_x\alpha_y\Big[\big(\tfrac{T\cdot\omega_h}{\alpha_x}\big)^{2}+\big(\tfrac{B\cdot\omega_h}{\alpha_y}\big)^{2}+(n\cdot\omega_h)^{2}\Big]^{2}}$$

with $T,B$ the tangent and bitangent and $\alpha_x,\alpha_y$ the two lobe widths.
The shipped code divides $(T\cdot\omega_h)^2$ by `rT` rather than by `rT²`, which
makes `rT` behave as $\alpha_x^{2}$ —

{{cite shaders/includes/material/ggx_aniso.glsl "float d     = (TdotH * TdotH / rT) + (BdotH * BdotH / rB) + NdotH * NdotH;"}}

— while the normalisation factor `π·rT·rB` assumes `rT` is $\alpha_x$:

{{cite shaders/includes/material/ggx_aniso.glsl "return 1.0 / (3.14159265 * rT * rB * d * d + 0.0001);"}}

Both readings cannot hold. Driving anisotropy toward zero and integrating the
shipped expression gives $\int D(\omega_h)(n\cdot\omega_h)\,d\omega_h = 1/\text{roughness}^{2}$
rather than 1 — measured as 4.0 at roughness 0.5 and 25 at roughness 0.2 — and
the lobe carries the width of α = roughness instead of roughness². The branch
therefore does not degenerate into the isotropic branch it sits next to; it steps
to a wider, brighter lobe the moment anisotropy crosses the 0.001 gate. Because
the same D feeds both the NEE specular and the specular MIS pdf, enabling
anisotropy over-brightens the highlight *and* de-normalises the pdf in the same
direction.

This never fires in a default render. The global override defaults to zero and
only `env_demo`'s `--aniso=` flag raises it, and OpenPBR's base, which the
reference and the deferred frame use, has no anisotropy at all.

{{cite ohao/render/rt/rt_settings.hpp "float anisotropyStrength{0.0f};"}}

One reading trap in the same file: its header comment says the tangent frame comes
from Frisvad's basis, while `worldUpTangent` — the function the D term actually
calls — was written specifically to reject Frisvad. Trust the function.

## What is actually reachable

Outside `ggx_aniso.glsl` and `openpbr.slang` there is no named GGX function in the tree.
The realtime raygen still open-codes a textbook model under no name at all: the
`(roughness+1)²/8` Schlick-k G in every NEE branch, textbook Schlick in those same
branches, and the `albedo/π` Lambert sitting a few lines under each Fresnel.

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 diff = kD * albedo / 3.14159;"}}

Prune by grepping the formula, not only the name: a copy written with different
temporaries is invisible to a search for the usual identifiers.

## Contracts

- OpenPBR's `ggxD`, `ggxSingle` and Fresnels are the one BSDF of the path tracer, deferred lighting, the transparent pass and RT GI's hits. A change to `material/openpbr.slang` moves all four, and the energy tables (`tools/material/openpbr_tables`) must be regenerated from the module.
- The realtime tracer floors roughness at 0.04 before shading; OpenPBR's base floors it at 0.01 and the G-buffer stores roughness as authored. Lowering the GLSL floor does not sharpen the realtime highlight — it drives D further into its epsilon guard.
- `ggxDisoShaded`, `ggxDiso` and `smithG1GGX` must all keep α = roughness². Changing one without the others biases the MIS estimator silently.
- `ggxDisoShaded` is the *shaded* D and `ggxDiso` the *density*. The realtime raygen's deeper bounces feed `ggxDisoShaded` into `pdf_spec = D·n·ω_h / 4(ω_o·ω_h)` and on into `specLastBsdfPdf`, which **looks** like the density/shaded confusion this page warns about and is not. `specLastBsdfPdf` is never a divisor — its only use is as an argument to the balance heuristic — and the env-IS side of that same partition computes the identical expression from the identical function. Balance weights sum to one whenever both sides agree, so the combination is unbiased; the wrong density costs variance only. Substituting `ggxDiso` at those sites *alone* would make them disagree with `pdfSpecMIS` and would create the bias rather than remove it.
- Past its first glossy bounce the realtime raygen's specular sampler is not GGX: it is a mirror direction jittered by a cosine lobe scaled by roughness, so neither D is its true pdf. Its first bounce has a real VNDF sampler with matched pdfs on both sides (`OHAO_CRUDE_GLOSSY` restores the jitter there too). The offline reference samples bounded VNDF at every vertex and divides by that density exactly.
- `ggxD_anisoOrIso` and `ggxDiso` are *not* the same D, and have not been since `9e2d0b4`. Anywhere one quantity is computed on both sides of a MIS weight, the same one must be called on both sides — `pt_raygen_realtime.rgen`'s env-IS `pdfSpecMIS` and its VNDF `specLastBsdfPdf` are exactly that pair, and both call `ggxDiso`.
- The anisotropic branch is not energy-matched to its own isotropic branch and is off by default. `--aniso=` renders are not radiometrically comparable to isotropic ones.
- Deferred lighting and the reference share the exact dielectric Fresnel and F82-tint; the realtime raygen uses textbook Schlick at F0 0.04. Rough dielectric silhouettes match between the deferred frame and the reference, not the realtime tracer.
