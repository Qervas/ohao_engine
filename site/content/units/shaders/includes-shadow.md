---
module: shaders
id: includes-shadow
title: Shadow includes
standard: v2
figures: [shaders-includes-shadow-slope-bias-clamp]
---

## Three headers, one live include

`shaders/includes/shadow/` is laid out like a library: a PCF sampler, a cascade
sampler with a Poisson kernel and a PCSS path, and a types header of structs,
defaults and helpers. Exactly one of the three is ever pulled into a stage. Grepping
the include directive across the whole shader tree returns a single hit, in the
forward fragment shader:

{{cite shaders/core/forward.frag "includes/shadow/shadow_pcf.glsl"}}

That hit needs qualifying immediately. `forward.frag` is the fallback stage:
`render()` tries the RT pipelines first, then deferred, and reaches the forward pair
only when neither is live — which no example in `examples/` ever arranges.

{{cite ohao/gpu/vulkan/renderer.cpp "// Forward rendering path"}}

{{cite shaders/core/deferred_lighting.frag "if (viewDepth < cascades.splitDepths[i]) return i;"}}

Its PCF taps accumulate the same way round, too — 1.0 where the fragment is lit:

{{cite shaders/core/deferred_lighting.frag "shadow += (projCoords.z - bias > pcfDepth) ? 0.0 : 1.0;"}}

## The include that has to come last

`shadow_pcf.glsl` takes the shadow map as a parameter but not the lights. It reaches
into a uniform block named `lighting` that the *calling* file must already have
declared, under that exact name, with those exact members:

{{cite shaders/includes/shadow/shadow_pcf.glsl "IMPORTANT: Caller must define LightUBO named 'lighting' before including this file!"}}

GLSL has no forward declaration for an interface block, so this is a purely textual
ordering constraint that the compiler reports only as an undefined identifier.
`forward.frag` declares the block at set 0, binding 1 and pulls the header in
twenty-six lines further down, under a comment explaining why. Hoisting the includes
to the top of the file — the reflex tidy-up — stops it compiling.

:::why
Passing the light data in as a parameter is what the same signature already does for
`shadowMap`, and it was rejected here for a stated reason. The sampler takes an
`int lightIndex`, never a `Light`, because — according to the header's own comment —
copying the 128-byte struct (four `vec4`s plus a `mat4`) out of the UBO comes back
corrupted on some drivers:

{{cite shaders/includes/shadow/shadow_pcf.glsl "Some GPU drivers corrupt large struct copies (the 128-byte Light struct)"}}

That comment is the only evidence in the tree — no driver named, no repro, no test —
so read it as the author's recorded rationale rather than a reproduced finding. The
code follows it regardless: every field is read by indexing the UBO directly, which is
what forces the block to be visible by name, and the fragile include order is the
price paid for it.
:::

The first thing the function does with that index is read `params.z`, the shadow-map
index the host writes, and return zero occlusion for any light whose value is
negative — so the forward shader can call it once per light in the loop and let
non-casters cost a single compare.

{{cite shaders/includes/shadow/shadow_pcf.glsl "float shadowMapIndex = lighting.lights[lightIndex].params.z;"}}

## Bias by the tangent of the incidence angle

A depth map records the occluder distance at texel centres. A receiver shaded
anywhere else inside that texel is compared against a depth belonging to a point up
to half a texel away, and on a surface tilted relative to the light that lateral
offset turns into a depth error proportional to the surface slope. The constant
subtracted before the comparison therefore has to grow with incidence angle:

$$b(\theta) \;=\; \min\!\left(b_0 \tan\theta,\; 0.01\right), \qquad \cos\theta = \max(N\cdot L,\, 0)$$

Here $b_0$ is the host-supplied `shadowBias`, $N$ the shading normal, $L$ the unit
direction from the surface to the light, and $\theta$ the incidence angle. The shader
computes it in exactly that form, through `acos` then `tan`, rather than the
algebraically equal $\sqrt{1-c^2}/c$:

{{cite shaders/includes/shadow/shadow_pcf.glsl "float bias = lighting.shadowBias * tan(acos(cosTheta));"}}

{{figure shaders-includes-shadow-slope-bias-clamp "Conceptual, computed from the shipped expression rather than captured. The two clamps apply the same tangent law with ceilings a factor of five apart, so they agree below 63 degrees and diverge sharply above it."}}

Same formula, two ceilings, and only the tighter one ships. The header's other
defaults are worth the same scepticism: `DEFAULT_SHADOW_BIAS` does match the 0.005 the
host writes, but `DEFAULT_SHADOW_STRENGTH` is 1.0 where the same function writes
`shadowStrength = 0.7f`.

## Occlusion here, visibility there

The live header filters with a 5×5 grid at one-texel spacing — twenty-five taps, all
weighted equally — and returns *occlusion*, pre-multiplied by the global strength:

{{cite shaders/includes/shadow/shadow_pcf.glsl "return shadow * lighting.shadowStrength;"}}

{{cite shaders/includes/shadow/shadow_pcf.glsl "const int pcfRadius = 2;"}}

`forward.frag` consumes it as `1 - shadow`, so with strength 0.7 a fully occluded
fragment still keeps 30 % of that light's direct contribution — a deliberate lift out
of black, applied uniformly rather than as any ambient or bounce term.

{{cite shaders/core/forward.frag "return contribution * (1.0 - shadow);"}}

A shader that included both headers would compile cleanly and shade one of the two
results inverted. Nothing in the tree currently does, because nothing includes the
second one.

## One map, one light, resolved per vertex

The depth-write side is a matched pair whose fragment shader is empty on purpose —
the pass has no colour attachment, so depth arrives straight from the rasteriser into
one 2048² image sized by a single constant on the renderer:

{{cite ohao/gpu/vulkan/renderer.hpp "constexpr uint32_t SHADOW_MAP_SIZE = 2048;"}}

The vertex shader is where the interesting choice sits. It walks the light UBO looking
for the first entry with a non-negative shadow-map index and adopts that light's
matrix, per vertex:

{{cite shaders/shadow/shadow_depth.vert "if (lightUbo.lights[i].params.z >= 0.0) {"}}

The loop can never find two, because the host hands out at most one non-negative
index. `updateLightBuffer` tags the first light whose `LightType` casts to 0 or 2 and
leaves every other at −1 — and in the engine's enum 0 is not the directional light,
it is `Sphere`, a point light:

{{cite ohao/scene/component/light_component.hpp "Sphere = 0,       // point light with radius (soft shadows)"}}

`Directional` is 1 and `Spot` is 2, so the qualifying set is point-or-spot and a sun
never casts. That inversion is not cosmetic. The type the host packs into
`position.w` is a *different* encoding — directional becomes 0, point becomes 1 — and
`calculateLightSpaceMatrix` switches on that one, falling through to identity for
anything that is neither directional nor spot:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "// Point lights don't use a single matrix (need cube maps)"}}

So whenever the tagged caster is a `Sphere`, it ships `params.z = 0` — a valid
shadow-map index — alongside an identity light-space matrix, which both the depth
pass and `shadow_pcf.glsl` then use as if it were a real projection.

The scan itself is a per-vertex re-derivation of a value the host already knows and
could have pushed, and its failure mode is quiet rather than loud. It is reached from
the other side of the same test: a scene lit only by `Directional` lights leaves every
`params.z` at −1, the loop finds nothing, and the identity fallback survives, so the
pass rasterises raw world coordinates as if they were clip space.

{{cite shaders/shadow/shadow_depth.vert "mat4 lightSpaceMatrix = mat4(1.0);"}}

Nothing skips the pass to spare it that. The guard that would, `m_shadowsEnabled`, is
initialised true and has no assignment anywhere in `ohao/`, `examples/` or `tests/`:

{{cite ohao/gpu/vulkan/renderer.hpp "bool m_shadowsEnabled{true};"}}

## Contracts

- `shadow_pcf.glsl` must be included *after* the caller declares a uniform block named `lighting` containing `lights[]`, `shadowBias` and `shadowStrength`. Reordering the includes is a compile error, and renaming the block is a compile error in a file that does not mention it.
- The function must keep taking a light *index*. The header states that binding a `Light` to a local corrupts on some drivers, and the whole naming coupling above exists to avoid it; the claim is unreproduced in this tree, but the code depends on it.
- The `shadow_depth` pass writes one map for whichever light the host tagged first, and nothing in either shader supports a second shadowed light. A `Sphere` light can be that first tag, and when it is, the matrix it ships with is identity — `calculateLightSpaceMatrix` has no cube-map path.