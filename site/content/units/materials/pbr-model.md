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
reconstruction gets right and the three places it deliberately cheats.

## F0: the 0.04 that is not arbitrary

F0 is the Fresnel reflectance at normal incidence: the fraction of light a flat
surface bounces straight back when you look at it head-on. For a dielectric it is
fixed by the index of refraction n through F0 = ((n - 1)/(n + 1))^2. Common
dielectrics sit near n = 1.5, so:

$$F_0^{\text{dielectric}} = \left(\frac{1.5 - 1}{1.5 + 1}\right)^2 \approx 0.04$$

OHAO hard-codes that constant as the dielectric base reflectivity.

{{cite shaders/includes/material/material_types.glsl "surface.F0 = vec3(0.04); // Dielectric F0"}}

Metals have no meaningful single-scalar IOR in this model and absorb all
transmitted light within nanometers, so they get zero diffuse and reuse base
color as F0. One branch-free `mix` spans the whole range:

{{cite shaders/includes/material/material_types.glsl "return mix(dielectricF0, surface.albedo, surface.metallic);"}}

:::key
The intermediate values of that lerp (metallic = 0.5) are not physical — no real
material is half-conductor. But the slider is continuous and monotonic, which is
all an artist needs, and pure 0/1 endpoints are exactly right.
:::

## Fresnel at grazing angles: the Schlick term

Head-on reflectance is only F0; at grazing angles every surface approaches a
perfect mirror. OHAO uses the roughness-aware Schlick approximation:

$$F(\theta) = F_0 + \big(\max(1-\text{rough},\,F_0) - F_0\big)\,(1 - \cos\theta)^5$$

{{cite shaders/includes/lighting/ibl.glsl "return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);"}}

The textbook Schlick term tops out at 1.0. The `max(1 - roughness, F0)` cap is the
cheat: on a rough surface the grazing highlight should not blow out to a mirror,
so the ceiling is pulled down as roughness rises. This matches the integrated
reflectance a pre-baked BRDF LUT would give, without the texture fetch.

## Past textbook Cook-Torrance

Most engines evaluate specular as single-scatter D·G·F and stop. OHAO adds two
refinements. First, the geometry term is the height-correlated Smith form, which
accounts for masking and shadowing being correlated on the same microsurface
rather than independent:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float geometrySmithCorrelated(float NdotV, float NdotL, float roughness) {"}}

Second — and this is the part a reviewer will notice is missing from most hobby
renderers — single-scatter GGX loses energy at high roughness because it models
exactly one bounce off the microsurface. Rough metal comes out visibly too dark.
OHAO adds the lost energy back with a Kulla-Conty / Turquin compensation term:

$$E_{ms} = (1 - E(\mu_o))\,(1 - E(\mu_i)), \qquad f_{ms} = \frac{F_{avg}\,E_{ms}}{1 - F_{avg}(1 - E(\mu_o))}$$

where E is the directional albedo of single-scatter GGX, evaluated from a
polynomial fit instead of a lookup table.

{{cite shaders/includes/brdf/brdf_ggx.glsl "float Ems = (1.0 - E_o) * (1.0 - E_i);"}}
{{cite shaders/includes/brdf/brdf_ggx.glsl "vec3 Favg = surface.F0 + (1.0 - surface.F0) / 21.0;"}}

{{figure ggx_multiscatter_energy "Single-scatter GGX droops with roughness; the compensation term returns total energy to ~1. Shape from the GGX_E() fit in brdf_ggx.glsl — conceptual, not a captured render."}}

:::why
Energy compensation is not cosmetic. Without it, rough metals read darker than
their albedo implies, so an artist compensates by over-brightening the base
color, and the asset then looks wrong under a different light. Matching Cycles'
multi-scatter GGX keeps look-dev portable between the offline and realtime paths.
:::

## The direct-vs-IBL k remap

The Smith-Schlick geometry approximation needs a roughness remap, and it is a
different constant for punctual lights than for image-based lighting — a subtlety
that silently darkens IBL if you use the wrong one:

{{cite shaders/includes/brdf/brdf_ggx.glsl "float k = (r * r) / 8.0;"}}

This `(rough + 1)^2 / 8` form is the direct-lighting remap (Karis 2013). The IBL
path uses `rough^2 / 2`; they are not interchangeable.

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

- Metals must have zero diffuse and F0 = base color; dielectrics F0 = 0.04. Violating this double-counts or loses energy.
- Roughness is clamped away from 0 (`max(roughness, 0.01)` in the unpack) so the specular lobe never becomes a zero-width delta that fireflies in the path tracer.
- The direct and IBL geometry remaps are not interchangeable; using the IBL k for punctual lights darkens direct specular.
