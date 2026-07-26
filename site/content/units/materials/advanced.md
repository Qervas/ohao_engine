---
module: materials
id: advanced
title: Advanced material includes
standard: v2
---

## Three files the compiler never sees

`shaders/includes/material/` holds four GLSL headers. Three of them —
`advanced_brdf.glsl`, `material_sampling.glsl`, `material_types.glsl` — are
`#include`d by no shader stage in the tree; the only edge in that subgraph is
`material_sampling.glsl` pulling in `material_types.glsl`. The fourth,
`ggx_aniso.glsl`, is included by the three path-tracer raygens — not by
`rt_gi.rgen` or `rt_shadow.rgen`, which are separate live pipelines — and ships
in every path-traced frame.

{{cite shaders/rt/pt_raygen.rgen "includes/material/ggx_aniso.glsl"}}

Nothing in the build notices the difference. `shaders/CMakeLists.txt` invokes
`glslc` once per *entry-point* stage — `.vert`, `.frag`, `.comp`, `.rgen`, and
friends — and never on a `.glsl`.

{{cite shaders/CMakeLists.txt "COMMAND ${GLSLC} --target-env=vulkan1.3"}}

The `includes/` tree is globbed into a separate `SHADER_INCLUDES` list used only
as a `DEPENDS` edge, so touching one of these three files forces every shader in
the engine to recompile while none of the three is ever parsed. They have not
been through a compiler.

Dead code is not the interesting part. Part of this math *does* render — through
a hand-copy that no longer agrees with the original.

## The anisotropy that shipped by copy

`advanced_brdf.glsl` splits one perceptual roughness into two GGX widths using
Burley's aspect ratio, with $\alpha = r^2$ for perceptual roughness $r$ and
$a \in [0,1)$ the anisotropy slider:

$$\alpha_t=\frac{\alpha}{\sqrt{1-0.9\,a}},\qquad \alpha_b=\alpha\sqrt{1-0.9\,a}$$

Here $\alpha_t$ is the lobe width along the tangent and $\alpha_b$ along the
bitangent; at $a=0$ they collapse back to $\alpha$.

{{cite shaders/includes/material/advanced_brdf.glsl "roughnessT = r2 / aspect;"}}

`ggx_aniso.glsl` — the file that actually runs — carries the same two
expressions, 0.9 constant and 0.001 floor included, with the clamp folded into
the assignment instead of following it and the two widths renamed `rT` and `rB`.

{{cite shaders/includes/material/ggx_aniso.glsl "max(r2 / aspect, 0.001)"}}

The distribution term is where the copy diverged. `D_GGX_Aniso` in the dead file
squares both widths before dividing, which is the standard Burley denominator.

{{cite shaders/includes/material/advanced_brdf.glsl "(TdotH * TdotH / a2T)"}}

The shipping copy writes `(TdotH * TdotH / rT)` — unsquared — while keeping the
same `π·rT·rB` normalisation in front, so `rT` has to stand for $\alpha_t^2$ in
the denominator and $\alpha_t$ in the prefactor at once. How large that error is,
and the fact that the same `D` feeds the specular MIS pdf as well as the shading
term, are worked out in the `materials/ggx` unit; what this page adds is where it
came from. The anisotropic branch never fires in a default render — the global
override `RTRenderSettings::anisotropyStrength` starts at zero and only
`env_demo`'s `--aniso=` flag raises it — so the divergence is latent, not visible
in any shipped reference image.

:::why
`ggx_aniso.glsl` copied this math rather than including it because the API in
`advanced_brdf.glsl` is the wrong shape for a path tracer.
`evaluateAnisotropicSpecular` takes `T` and `B` as arguments — it assumes a
caller that already has vertex tangents, which is a rasteriser's world. The
callers of `ggxD_anisoOrIso` are the three path-tracer raygens, and a raygen has
neither: the ray payload it shares with the hit shaders has no tangent field, and
there are no screen-space derivatives to rebuild one from. So `ggx_aniso.glsl`
builds the frame itself by projecting world-up onto the surface, and folds the
aspect split and the rotation into the same call. The rejected alternative —
carry a tangent frame through the
payload — costs bytes on every hit for a term that is off by default. The price
paid for the copy is the divergence above.
:::

## Where the skin model went

`evaluateSkinSSS` wraps $N\cdot L$ into $[0,1]$ and raises it to three
per-channel powers — 1.0, 1.3, 1.8 — so red bleeds furthest past the terminator.

{{cite shaders/includes/material/advanced_brdf.glsl "pow(diffuse, 1.3),"}}

Those powers are conditional on the caller's `curvature` argument: the next
statement is `scatter = mix(vec3(diffuse), scatter, curvature)`, so at
`curvature = 0` the three channels collapse back to the plain wrap and the
per-channel spread has no effect at all.

The offline raygen kept the wrap and replaced the powers. Its `--sss=` path
applies the identical `NdotL * 0.5 + 0.5` remap, then drives the squared distance
from fully lit through three per-channel Gaussians at rates 1.8, 6.0 and 20.0 —
red widest, blue near-Lambertian — with every rate multiplied by
`curvScale = mix(1.0, 0.3, curvature)`, so curved geometry cuts the exponent to
30% of its quoted value and broadens the whole profile. An inline comment says
the linear wrap it replaced could not fake the terminator shoulder.

{{cite shaders/rt/pt_raygen_offline.rgen "float g = exp(-d2 * 6.0 * curvScale);"}}

So here the idea survived and the formula did not — the opposite of the
anisotropy case, where the function has no callers but the formula ships. "No
callers" is not a verdict on either one; only reading both formulas is.

## Two flag namespaces with overlapping bits

`advanced_brdf.glsl` declares thirteen `FEATURE_*` bits starting at bit 0, and
its comment says they must match the C++ `MaterialFeatures` enum, which lives in
`ohao/gpu/vulkan/material_instance.hpp`. They do, bit for bit, from
`DoubleSided` at `1 << 0` through `Sheen` at `1 << 12`.

`material_types.glsl`'s `MaterialData.flags`, however, is meant to be read with
the unrelated `MATERIAL_FLAG_*` set from `includes/common/constants.glsl`, where
bit 0 means "has albedo map".

{{cite shaders/includes/common/constants.glsl "#define MATERIAL_FLAG_HAS_ALBEDO_MAP 0x0001"}}

Handing `MaterialData.flags` to `hasFeature()` — the obvious move if you wire
these two headers together, which sharing a directory implies you should — reads
"has albedo map" as "double sided", and `MATERIAL_FLAG_DOUBLE_SIDED` (0x0040) as
`FEATURE_USE_AO`. Neither compiler nor validator would say anything.

`MaterialData` is also documented against a file that does not exist: there is no
`material_types.hpp` anywhere in the repository.

{{cite shaders/includes/material/material_types.glsl "It matches the C++ MaterialData struct"}}

Nor is there a 96-byte material block on either pipeline for it to match. The
path tracer reads a flat array of three `vec4` per material — the convention the
`materials/pack` unit takes apart — and the raster shaders take base colour,
roughness, metallic and their texture indices out of push constants. The std140
layout documented in this header describes nothing the engine writes.

## Tells that none of this has executed

`V_Sheen` returns `1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV))` with no floor,
while the two other visibility terms in the same file — `V_ClearCoat` and
`V_GGX_Aniso` — both guard their divisor with `max(..., 0.0001)`. On a
silhouette, where both dot products approach zero, `V_Sheen` returns infinity.

`refractRay` swaps $\eta$ correctly for the exit case but never flips `N`, and the
refraction identity it uses is only valid for $\mathbf N\cdot\mathbf I<0$. Feed it
`I = N = (0,0,1)` with `ior = 1.5`: $k=1$, and the expression returns `(0,0,-1)` —
a ray leaving along the normal comes back reversed.

{{cite shaders/includes/material/advanced_brdf.glsl "return eta * I - (eta * NdotI + sqrt(k)) * N;"}}

And the combiner never wires anisotropy in at all. `evaluateAdvancedBRDF` takes
`T` and `B`, tests four of the five advanced features, and has no
`FEATURE_ANISOTROPY` branch — the tangent arguments are unread, and
`evaluateAnisotropicSpecular`, whose only occurrence in the tree is its own
definition, is unreachable from it. The function also returns just the additional
lobes; there is no base diffuse or specular anywhere in the sum, so it is an
increment, not a BRDF.

{{cite shaders/includes/material/advanced_brdf.glsl "vec3 evaluateAdvancedBRDF(AdvancedMaterialParams mat,"}}

## What material_sampling.glsl is, and is not

The name promises an importance sampler; the file contains none. GGX VNDF
sampling lives in `ggx_aniso.glsl`, and `cosineHemisphere` is written out
separately inside each of the three path-tracer raygens. `material_sampling.glsl`
is texture plumbing: UV transform, sRGB decode, AO strength, and two normal-map
paths — one consuming vertex tangents, one rebuilding the frame from screen-space
derivatives.

Its last contradiction is internal to the dead pair. `sampleMetallicRoughness`
reads metallic from blue and roughness from green, per glTF, while `MaterialData`'s
comment on the very texture index it would sample says `R=metallic, G=roughness`.

{{cite shaders/includes/material/material_sampling.glsl "return vec2(mr.b, mr.g); // (metallic, roughness)"}}

:::key
All three uncompiled headers are inert, but not equally so. Two of them are a
reference material pipeline nobody wired up; the third, `advanced_brdf.glsl`,
holds the one piece of math that ships — and it ships as a copy in
`ggx_aniso.glsl` that dropped a square. Zero callers told you nothing here; only
reading both formulas did.
:::
