---
module: materials
id: ggx
title: GGX implementation
standard: v2
---

## One GGX for every renderer

The isotropic GGX distribution exists once in the shaders, as `ggxD` in
`material/openpbr.slang`, the specular lobe of OpenPBR's base. Every renderer that
shades a surface imports that module: the path tracer (`rt/path_tracer.slang`, both RT
profiles), deferred lighting (`core/deferred_lighting.slang`, through
`includes/lighting/clustered_lights.slang`), the forward transparent pass and RT GI's
hits. A highlight is the same function of the material in all of them.

{{cite shaders/core/deferred_lighting.slang "import material.openpbr;"}}
{{cite shaders/rt/path_tracer.slang "import material.openpbr;"}}

## The α convention

`OpenPBRBase` resolves the artist's perceptual roughness to α = roughness² in its
constructor, and the Trowbridge-Reitz D squares α again:

$$D(m)=\frac{\alpha^{2}}{\pi\big[(n\cdot m)^{2}(\alpha^{2}-1)+1\big]^{2}},\qquad \alpha=\text{roughness}^{2}$$

Here $m$ is the microfacet normal (the half-vector between view and light), $n$ the
shading normal, and $\alpha$ the lobe width. $D$ is normalised so that
$\int D(m)(n\cdot m)\,dm = 1$ over the hemisphere — the microfacets' projected areas add
back up to the macrosurface.

{{cite shaders/material/openpbr.slang "alpha = rough * rough;"}}

The double squaring is why a painted roughness of 0.1 is already a near-mirror:
α = 0.01, α² = 10⁻⁴. The module floors roughness at 0.01 (α = 10⁻⁴), a mirror at any
renderer's resolution. The G-buffer stores roughness as authored and the path tracer's
closest hit passes it through; the floor is the module's alone.

{{cite shaders/material/openpbr.slang "static const float kMinSpecularRoughness = 0.01;"}}

## A denominator that does not cancel

The textbook denominator forms $\alpha^2 - 1$, which rounds to −1 in single precision
at α = 10⁻⁴ (α² = 10⁻⁸), and the peak becomes 0/0. `ggxD` writes the same quantity in
the microfacet normal's local components, $m_x^2+m_y^2+m_z^2\alpha^2$ — equal to
$(n\cdot m)^2(\alpha^2-1)+1$ for a unit $m$ — and never forms the difference:

{{cite shaders/material/openpbr.slang "const float d = dot(m.xy, m.xy) + m.z * m.z * a2;"}}

So the function has no guard at all: nothing is added to the denominator, and it is
exact Trowbridge-Reitz at whatever width it is handed. Its normalisation holds at every
roughness down to the floor; the floor is the only thing that keeps D finite, and it
does so by substituting a slightly wider — still correctly normalised — lobe rather
than by removing the peak's mass.

## Masking-shadowing: height-correlated

The single-scatter lobe is $D\,G_2/4\mu_o$ (times $\cos\omega_i$) with Heitz's
height-correlated $G_2$, written from the Smith Λ function:

$$G_2(\omega_o,\omega_i)=\frac{1}{1+\Lambda(\omega_o)+\Lambda(\omega_i)},\qquad \Lambda(\theta)=\tfrac{1}{2}\!\left(-1+\sqrt{1+\alpha^{2}\tan^{2}\theta}\right)$$

with $\omega_o$ the view direction, $\omega_i$ the light direction and $\mu_o$ the cosine
of the view to the normal. The denominator is explicit, so there is no folded-in
visibility term to divide twice.

{{cite shaders/material/openpbr.slang "const float G2 = 1.0 / (1.0 + ggxLambda(wo.z, a2) + ggxLambda(wi.z, a2));"}}

Deferred lighting evaluates the same `ggxSingle` through `OpenPBRBase.evalLobes`, so the
rasteriser and the path tracer agree on masking-shadowing exactly; sphere and rectangle
lights only widen the lobe, through `withRoughness`.

## Fresnel: exact for dielectrics, F82-tint for metals

OpenPBR's base gives a dielectric the exact unpolarised Fresnel of its IOR (after
`specular_weight` has scaled its F0) and a metal the F82-tint form: Schlick minus a
$\mu(1-\mu)^6$ term fitted so the reflectance at $\mu = 1/7$ is `specular_color` times
Schlick's:

{{cite shaders/material/openpbr.slang "return saturate(F0 + (1.0 - F0) * m5 - b * (c * m5 * m));"}}

Both the path tracer and deferred lighting call it, for every light and, through
`specularAlbedo`, for the environment's split sum.

:::why
A rough dielectric under single-scatter GGX with full Schlick gets a bright rim at
grazing angles that nothing cancels. OpenPBR removes the cause: Kulla-Conty's
multiple-scattering lobe returns the energy single-scatter GGX loses, and the diffuse
beneath is scaled by what the specular lets through, so the white furnace holds with the
exact Fresnel and the deferred silhouette matches the path tracer's.
:::

{{cite shaders/material/openpbr.slang "return favg * favg * e1Avg / (1.0 - favg * (1.0 - e1Avg));"}}

## Sampling: bounded VNDF with its exact density

The path tracer draws a microfacet normal from the visible normals, with Eto and
Tokuyoshi's 2023 bound: the spherical cap it samples leaves out the normals whose
reflection would fall below the horizon, so no sample is wasted under the surface.
`ggxPdfBounded` is the density of the reflected direction under that sampling, and the
mixture of it with the diffuse lobe's density is what the sampler reports:

{{cite shaders/material/openpbr.slang "s.pdf = ps * ggxPdfBounded(wo, s.wi, alpha) + (1.0 - ps) * eonPdf(wo, s.wi, diffRough);"}}

The estimator weight is the whole BSDF over that density, so the sampler and the
evaluator cannot drift apart:

{{cite shaders/material/openpbr.slang "if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;"}}

:::key
D, its density and the BSDF the weight divides are one module. A change to `ggxD`
moves the deferred frame, the path tracer, the transparent pass and RT GI together,
and moves the energy tables with them: `openpbr_tables --check` fails until they are
regenerated from the module.
:::

## No anisotropy

OpenPBR's base as built has no anisotropic roughness: one α per surface, one tangent-free
lobe. A material's roughness texture scales that α and nothing else.

## Contracts

- `ggxD`, `ggxSingle`, the Fresnels and `ggxPdfBounded` are the one specular lobe of the path tracer, deferred lighting, the transparent pass and RT GI's hits. A change to `material/openpbr.slang` moves all four, and the energy tables (`tools/material/openpbr_tables`) must be regenerated from the module.
- Roughness is floored once, in the module, at 0.01. A renderer that floors it again before handing it over renders a different material from the others.
- α = roughness² everywhere: `OpenPBRBase`'s constructor, `ggxD`, `ggxLambda`, the sampler and its density. Changing the convention in one place biases the estimator silently.
- The sampler's density (`ggxPdfBounded` mixed with `eonPdf`) is also what MIS weighs light samples against; it must stay the density the sampler actually draws from.
