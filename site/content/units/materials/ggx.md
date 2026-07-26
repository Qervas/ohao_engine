---
module: materials
id: ggx
title: GGX implementation
standard: v2
figures: [materials-ggx-ndf-energy]
---

## The α convention every copy shares

This tree writes the isotropic GGX distribution out twenty-four times. Four of
those copies sit behind a function name: `distributionGGX` in `brdf_ggx.glsl`,
which only `deferred_lighting.frag` and `forward.frag` reach — through
`evaluateBRDF`; `ggxD_anisoOrIso`'s isotropic branch and `ggxDiso` in
`ggx_aniso.glsl`, `#include`d by the three path-tracer raygens and nothing else;
and `D_ClearCoat` in `advanced_brdf.glsl`, a file no shader `#include`s. The other
twenty are inline. Past bounce 0 the raygens stop calling `ggx_aniso.glsl` and
paste the algebra straight into each NEE, env-MIS and pdf branch — six copies in
`pt_raygen.rgen`, six in `pt_raygen_offline.rgen`, eight in
`pt_raygen_realtime.rgen`.

The one thing all twenty-four agree on without qualification is the width
parameter. Every one of them resolves the artist's perceptual roughness to
α = roughness² — `ggxDiso` at its call site, the rest internally — and then squares
α again inside the Trowbridge-Reitz denominator:

$$D(\omega_h)=\frac{\alpha^{2}}{\pi\big[(n\cdot\omega_h)^{2}(\alpha^{2}-1)+1\big]^{2}},\qquad \alpha=\text{roughness}^{2}$$

Here $\omega_h$ is the half-vector between view and light, $n$ the shading
normal, and $\alpha$ the lobe width. $D$ is normalised so that
$\int D(\omega_h)(n\cdot\omega_h)\,d\omega_h = 1$ over the hemisphere — the
microfacets' projected areas must add back up to the macrosurface. That property
is load-bearing, and it is the one this implementation loses. The rasteriser's
copy:

{{cite shaders/includes/brdf/brdf_ggx.glsl "return a2 / max(denom, EPSILON);"}}

and the copy the raygens call at bounce 0 and then re-type by hand at every bounce
after:

{{cite shaders/includes/material/ggx_aniso.glsl "return a2 / (3.14159265 * denom * denom + 0.0001);"}}

The double squaring is why a painted roughness of 0.1 is already a near-mirror:
α = 0.01, α² = 10⁻⁴. Both stacks also floor roughness at 0.04 before shading —
the path tracer in `pt_closesthit.rchit`, the rasteriser in `initBRDFSurface` —
so α never falls below 1.6 × 10⁻³.

{{cite shaders/includes/brdf/brdf_common.glsl "surface.roughness = max(roughness, 0.04);"}}

The α agreement is not cosmetic — a sampler whose α drifts from the D it
importance-samples renders a plausible-looking image with a quietly biased
estimator — and `ggx_aniso.glsl` spells the constraint out.

{{cite shaders/includes/material/ggx_aniso.glsl "convention here (alpha = roughness^2) MATCHES"}}

Five lines later the same comment block overreaches. `ggxDiso` — the D behind the
realtime raygen's VNDF pdf — is introduced as "numerically identical" to
`ggxD_anisoOrIso`'s isotropic branch. The α convention does match; the guard does
not. The branch adds 10⁻⁴ to its denominator and `ggxDiso` adds 10⁻⁸, and by the
quadrature below that factor of 10⁴ is the difference between retaining 2.7% and
91% of the distribution's mass at roughness 0.1.

{{cite shaders/includes/material/ggx_aniso.glsl "identical to ggxD_anisoOrIso's isotropic branch"}}

{{cite shaders/includes/material/ggx_aniso.glsl "return a2 / (3.14159265 * denom * denom + 1e-8);"}}

## The visibility term that is not a geometry term

`geometrySmithCorrelated` is named for a G but returns a V. It is Heitz's
height-correlated Smith term already divided by the Cook-Torrance denominator:

$$V(\omega_o,\omega_i)=\frac{G_2}{4(n\cdot\omega_o)(n\cdot\omega_i)}=\frac{0.5}{(n\cdot\omega_i)\sqrt{\alpha^{2}+(1-\alpha^{2})(n\cdot\omega_o)^{2}}+(n\cdot\omega_o)\sqrt{\alpha^{2}+(1-\alpha^{2})(n\cdot\omega_i)^{2}}}$$

with $\omega_o$ the view direction, $\omega_i$ the light direction, and $G_2$ the
joint masking-shadowing fraction. The two square roots are the code's `lambdaV`
and `lambdaL`, and the leading 0.5 is where the factor of 4 went:

{{cite shaders/includes/brdf/brdf_ggx.glsl "return 0.5 / max(lambdaV + lambdaL, EPSILON);"}}

Consequently the deferred specular is assembled as a bare triple product with no
denominator at all:

{{cite shaders/includes/brdf/brdf_ggx.glsl "return D * V_term * F;"}}

This is the sharpest edit trap in the file. Anyone who notices the "missing"
Cook-Torrance denominator and restores $1/4(n\cdot\omega_o)(n\cdot\omega_i)$
divides twice: highlights come out four times too dim head-on and wrongly lifted
at grazing angles — a change that reads as a plausible material tweak rather than
a bug. The raygens keep the two apart, building an *uncorrelated* Schlick-k `G`
and dividing by `4 * NdotV * NdotL + 0.001` explicitly in each NEE branch — a
second additive epsilon inside the same expression.

A third arrangement lives in `ggx_aniso.glsl`. `smithG2overG1GGX` is written from
the Smith Λ function rather than the square root above, and substituting
$\Lambda=\tfrac{1}{2}\!\left(-1+\sqrt{1+\alpha^{2}\tan^{2}\theta}\right)$ into
$1/(1+\Lambda_o+\Lambda_i)$ reproduces $2c_oc_i/(c_iS_o+c_oS_i)$ exactly — the same
$G_2$ `geometrySmithCorrelated` computes. That is where the resemblance stops. It
has exactly one call site in the tree, in the realtime raygen, as the VNDF
estimator weight $f\cos/\text{pdf}=F\,G_2/G_1$ with the Fresnel factored out —
not as a shading G.

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

`pt_raygen.rgen` and `pt_raygen_offline.rgen` never mention it. The two pipelines
therefore do not share a masking-shadowing model: the rasteriser shades with
height-correlated Smith, every raygen NEE branch shades with the uncorrelated
Karis product.

## Fresnel: an IBL approximation wired into direct lighting

`brdf_common.glsl` ships two Schlick Fresnels. The textbook one, with F90 fixed
at 1, has no callers anywhere under `shaders/`. The one the deferred BRDF actually
calls replaces F90 with `max(1 - roughness, F0)`:

$$F(\omega_o\cdot\omega_h)=F_0+\big(\max(1-\text{roughness},\,F_0)-F_0\big)\,(1-\omega_o\cdot\omega_h)^{5}$$

{{cite shaders/includes/brdf/brdf_common.glsl "return F0 + (max(vec3(1.0 - roughness), F0) - F0)"}}

At roughness 0.95 a dielectric's F90 collapses from 1.0 to 0.05, i.e. essentially
to $F_0$: no grazing boost at all. This variant is normally an image-based-lighting
device evaluated at $n\cdot\omega_o$, standing in for a pre-integrated
reflectance; OHAO evaluates it per punctual light at $\omega_o\cdot\omega_h$.

{{cite shaders/includes/brdf/brdf_ggx.glsl "F = fresnelSchlickRoughness(HdotV, F0, roughness);"}}

:::why
The rejected alternative is textbook Schlick with F90 = 1 — exactly what all three
raygens use inline. Under a rasteriser that Fresnel gives a rough dielectric a
bright white rim at grazing angles with nothing to cancel it: the inter-reflection
that would swallow it is microfacet-scale transport the deferred pass never traces.
Clamping F90 to 1 − roughness buys EEVEE-like edge behaviour at the price of a
Fresnel that no longer matches the offline render, so a rough dielectric silhouette
is dimmer in the deferred preview than in the path-traced frame.
:::

## The epsilon that eats the highlight

Those twenty-four D expressions guard their denominator in four different ways,
and only one of them is a floor. `distributionGGX` floors the finished denominator,
`max(π·denom², EPSILON)`, with EPSILON defined as 10⁻⁶.

{{cite shaders/includes/common/constants.glsl "#define EPSILON       1e-6"}}

`ggxD_anisoOrIso`'s isotropic branch and all twenty inline raygen copies *add*
10⁻⁴. `ggxDiso` adds 10⁻⁸. `D_ClearCoat` guards nothing at all — harmless only
because nothing includes the file it sits in.

{{cite shaders/includes/material/advanced_brdf.glsl "return a2 / (3.14159265 * denom * denom);"}}

An additive term is not a floor — it never leaves the expression. At the lobe
peak the true denominator is $\pi\,\text{roughness}^{8}$, which at the 0.04
roughness floor is about 2 × 10⁻¹¹: roughly five orders of magnitude under
`distributionGGX`'s floor, seven under the added 10⁻⁴, and not quite three under
`ggxDiso`'s 10⁻⁸. The guard, not the GGX algebra, sets the height of the highlight.

Numerical quadrature of the three shipped guards shows how far each truncation
reaches. `distributionGGX` recovers full normalisation by roughness ≈ 0.16 and
`ggxDiso` is within 1% of it by 0.14, but the +10⁻⁴ expression — the one on every
NEE branch of every raygen — does not until roughness ≈ 0.4, and at roughness 0.2
it retains only 37% of the distribution's mass (2.7% at roughness 0.1, 0.07% at
the 0.04 floor).

{{figure materials-ggx-ndf-energy "Hemispherical integral of the three shipped isotropic D guards versus perceptual roughness — measured by numerical quadrature of the GLSL, not a captured render. Two of the three curves come from ggx_aniso.glsl, which ships both a 10⁻⁴ and a 10⁻⁸ guard. A correct NDF integrates to 1 everywhere; the missing mass is the peak of the specular lobe."}}

Where it bites matters as much as how much. Take the raygen `PathTracer` binds by
default:

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_pt_raygen.rgen.spv"};"}}

D enters that file at nine sites: six BRDF evaluations — light NEE and env-MIS, at
bounce 0 and again on both the specular and the diffuse chain — and three MIS
pdfs. The truncation therefore dims specular NEE at every bounce along both chains.
What it never touches is indirect specular throughput, which is a bare metal tint:

{{cite shaders/rt/pt_raygen.rgen "specThroughput = mix(vec3(1.0), albedo, metallic);"}}

The ray it weights is not a mirror ray, though. The comment calling the dual-ray
design deterministic is about lobe *selection* — bounce 0 traces a specular and a
diffuse child instead of choosing one — not direction. Above roughness 0.01 the
reflection is perturbed by a roughness-scaled cosine-hemisphere jitter, with a
stochastic fallback below the horizon:

{{cite shaders/rt/pt_raygen.rgen "vec3 jitVec = cosineHemisphere(reflected, jitU) * roughness;"}}

{{cite shaders/rt/pt_raygen.rgen "reflected = cosineHemisphere(N, fallbackU);"}}

Its MIS pdf is where D re-enters the chain:

{{cite shaders/rt/pt_raygen.rgen "specLastBsdfPdf = pdf_spec;  // pure GGX PDF (no lobe-mix factor in dual-ray)"}}

So a chrome sphere still reflects the environment at full strength. What shrinks
is the direct-light highlight on smooth surfaces, and the MIS weight deciding how
much environment radiance the specular ray is allowed to keep.

:::key
The `+ 0.0001` in the raygens' D is not a division guard; it is an undeclared
highlight clamp on smooth materials. `kOfflineRTSettings` turns firefly clamping
off outright, yet a roughness-0.2 surface loses roughly two thirds of its NEE
specular before any clamp logic runs.

{{cite ohao/render/rt/rt_settings.hpp ".enableFireflyClamp = false,"}}

There is no free repair. Flooring the denominator instead of adding to it — what
`distributionGGX` already does — still loses 99.1% of the mass at the 0.04
roughness floor, and even `ggxDiso`'s 10⁻⁸ leaves 93% missing there. Every option
changes every existing smooth-material reference render, which is the real reason
to touch this carefully rather than not at all.
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
only `env_demo`'s `--aniso=` flag raises it, so every shipped reference image is
on the isotropic branch.

{{cite ohao/render/rt/rt_settings.hpp "float anisotropyStrength{0.0f};"}}

One reading trap in the same file: its header comment says the tangent frame comes
from Frisvad's basis, while `worldUpTangent` — the function the D term actually
calls — was written specifically to reject Frisvad. Trust the function.

## What is actually reachable

`brdf_common.glsl` and `brdf_ggx.glsl` define twenty functions between them; the
deferred and forward fragment shaders reach nine. Four of the eleven unreachable
ones still have their math shipping under no name at all. `geometrySchlickGGX`
and `geometrySmith`, its sole caller, are the `(roughness+1)²/8` remap inlined in
every NEE branch of all three raygens. `fresnelSchlick` — textbook Schlick with
F90 = 1 — is written out by hand in those same branches and in `ssr.comp`, minus
the `saturate`. `lambertianDiffuse` is the `albedo/π` sitting a few lines under
each of those Fresnels.

{{cite shaders/postprocess/ssr.comp "vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);"}}

{{cite shaders/rt/pt_raygen.rgen "vec3 diff = kD * albedo / 3.14159;"}}

Five of the remaining seven are dead in both senses: `f0FromIOR`, `envBRDFApprox`,
and the uncorrelated Smith trio, whose
$(2\,n\cdot\omega)/(n\cdot\omega+\sqrt{\cdot})$ arrangement appears nowhere else
under `shaders/`.

{{cite shaders/includes/brdf/brdf_common.glsl "float smithG(float NdotV, float NdotL, float roughness) {"}}

The last two are one-line compositions the fragment shaders open-code rather than
call: `getRoughnessMipLevel` as a literal `roughness * 9.0`,
`evaluateBRDFWithShadow` as a multiply by `1 - shadow`.

{{cite shaders/core/deferred_lighting.frag "float lod = roughness * 9.0;"}}

Prune by grepping the formula, not only the name — the twenty-four-copy count at
the top of this page is what that grep returns.

## Contracts

- `geometrySmithCorrelated` returns a visibility term with $1/4(n\cdot\omega_o)(n\cdot\omega_i)$ folded in, so `evaluateSpecularBRDF` must return `D * V * F` with no denominator. Adding one divides twice.
- Both stacks floor roughness at 0.04 before shading. Lowering that floor does not sharpen the highlight — it drives D further into its epsilon guard.
- `ggxD_anisoOrIso`, `ggxDiso` and `smithG1GGX` must all keep α = roughness². Changing one without the others biases the MIS estimator silently.
- The three *shipped* isotropic D guards — 10⁻⁶ floored, 10⁻⁴ added, 10⁻⁸ added — normalise at roughness 0.16, 0.4 and 0.14 respectively. They are not interchangeable, and editing one changes which existing renders it matches.
- The anisotropic branch is not energy-matched to its own isotropic branch and is off by default. `--aniso=` renders are not radiometrically comparable to isotropic ones.
- The deferred Fresnel clamps F90 to `1 - roughness`; the raygens do not. Rough dielectric silhouettes will not match between preview and offline render.
