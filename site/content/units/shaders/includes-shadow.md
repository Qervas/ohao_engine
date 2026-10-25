---
module: shaders
id: includes-shadow
title: Shadow sampling in lighting
standard: v2
---

## No shadow header, two shadow sources

There is no shadow include directory. Every shadow lookup the raster frame makes is
written inline in `deferred_lighting.frag`, and it takes one of two forms per frame:
a ray-traced visibility image, or, where the frame cannot trace, a cascaded shadow map
for directional lights. The host picks one when the lighting pass records: the RT bit
and the CSM bit are the two arms of one if/else on whether an RT visibility view is
bound.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 4;   // CSM fallback (bit 2)"}}

The only shadow stage file left is the cascade depth write, `shadow/shadow_csm.vert`
(below). Both consumers live in the lighting loop, after the light's attenuation and
before its BRDF, and both produce *visibility*: 1 lit, 0 occluded, multiplied straight
into that light's contribution.

{{cite shaders/core/deferred_lighting.frag "Lo += evaluateBRDF(surface, L, radiance) * shadow;"}}

## Eight channels, one byte each

The RT image is `R32G32_UINT` at the lighting pass's size, bound at binding 14 as a
`usampler2D`: eight 8-bit channels, x holding lights 0 to 3 and y lights 4 to 7, lowest
byte first. It is fetched once per pixel before the loop, by integer coordinate:

{{cite shaders/core/deferred_lighting.frag "layout(set = 0, binding = 14) uniform usampler2D rtShadowVisibility;"}}

A light finds its channel in `params.w`. The light table hands channels out in light
order to the first eight lights that cast shadows, and writes -1 into every other
light, which is then never occluded:

{{cite shaders/core/deferred_lighting.frag "const uint word = slot < 4 ? rtShadow.x : rtShadow.y;"}}

Eight is the image's width in bytes, not the light table's size: the raster table holds
up to 65,536 lights, so a scene with a ninth caster renders it unshadowed. The light
table counts those lights (`RenderScene::shadowCastersWithoutChannel()`), and test 8
checks the case on Cornell, whose twelve casting lights give eight channels and four
unshadowed lights.

{{cite ohao/render/scene/light_table.cpp "if (m_shadowLights.size() < kMaxShadowLights) {"}}

A channel that reads 0 makes the light's term exactly zero, and the loop skips it
before the BRDF. That skip matters to clustered lighting, which is exact only if every
term it leaves out or skips would have added $+0$:

{{cite shaders/core/deferred_lighting.frag "if (shadow == 0.0) continue;   // fully occluded: adds exactly zero"}}

## The cascade fallback

Without RT shadows, a light with a channel is shadowed by the cascades if it is
directional, and only then: the CSM branch tests `lightType == 0`, so point and spot
lights cast no shadow in a frame that cannot trace.

{{cite shaders/core/deferred_lighting.frag "} else if ((pc.flags & 4u) != 0u && lightType == 0) {"}}

The cascade is chosen by positive view-space depth against the cascade UBO's split
distances:

{{cite shaders/core/deferred_lighting.frag "if (viewDepth < cascades.splitDepths[i]) return i;"}}

and the lookup is a 3×3 PCF whose nine taps are hardware depth comparisons: the map is
bound as a `sampler2DArrayShadow` through the pass's `LESS_OR_EQUAL` comparison sampler,
the fourth coordinate the biased reference depth:

{{cite shaders/core/deferred_lighting.frag "layout(set = 0, binding = 6) uniform sampler2DArrayShadow shadowMap;"}}
{{cite shaders/core/deferred_lighting.frag "float(cascadeIdx), reference));"}}

The bias is the cascade UBO's `shadowBias`, a constant 0.012 subtracted at every
incidence angle. A depth map's error grows with the tangent of the angle between the
surface and the light, so a constant is too much at normal incidence (light leaks at
contact) or too little at grazing angles (acne); the `normalBias` beside it in the same
struct, the usual remedy, is never read by the shader.

{{cite ohao/render/deferred/render_pass_base.hpp "float shadowBias{0.012f};"}}

`calculateShadowCSM` takes the fragment position and view depth and nothing about the
light, so a second directional light recomputes the same nine taps and gets the same
value. The cascades are rendered from a single direction, the deferred renderer's own
sun direction (the sky's, or `setDirectionalLight`'s), whichever directional light the
loop is shading:

{{cite shaders/core/deferred_lighting.frag "shadow = calculateShadowCSM(fragPos, viewDepth);"}}
{{cite ohao/render/deferred/csm_pass.cpp "glm::mat4 lightView = glm::lookAt(center - m_lightDirection * radius, center, up);"}}

## Four cascades, one vertex stage

The depth-write side is a single vertex stage with no fragment shader: depth arrives
straight from the rasteriser into one 2048² layer per cascade. Each cascade is its own
render pass into its own array-layer view, so the vertex shader needs no layer
routing — it takes that cascade's view-projection as a push constant, reads the draw
record at `gl_InstanceIndex` from `RenderScene`'s draw table (the indirect command's
`firstInstance` is the draw's index, written by GPU culling), takes the object's world
matrix from the instance table at the slot the record names, and projects directly:

{{cite shaders/shadow/shadow_csm.vert "gl_Position = pc.viewProj * instances[draws[gl_InstanceIndex].draw.w].world * vec4(inPosition, 1.0);"}}

The push block is a single `mat4`, 64 bytes, half the push-constant size every Vulkan
device guarantees, and the C++ mirror pins that size:

{{cite ohao/render/deferred/csm_pass.hpp "static_assert(sizeof(ShadowPushConstant) == 64);"}}

The renderer does not draw the cascades at all in a frame that traces: the `csm` pass
is added to the graph only when RT shadows do not run.

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_csmPass && !rtShadows) {"}}

## The host's shadow fields no shader reads

The light table still tags the first point or spot light with a shadow-map index in
`params.z` and computes a `lightSpaceMatrix` for it — identity for a point light, which
has no cube-map path. Neither reaches a shader: the GPU record, `LightGpu`, carries
`params.z` but the lighting loop reads only `params.x`, `.y` and `.w`, and the matrix is
dropped when `LightClusters` copies `LightData` into `LightGpu`.

{{cite ohao/render/scene/light_table.cpp "// The legacy rule, kept as is: old LightType 0/2 = Sphere(point)/Spot."}}

## Contracts

- RT shadows (bit 5) and CSM (bit 2) are exclusive per frame, decided by whether an RT visibility view is bound when the lighting pass records. The `csm` pass runs only in frames that do not trace.
- `params.w` and the visibility's channels are one contract with the light table and the RT shadow passes: the table hands out channels 0 to 7 in the order it lists `shadowLights()`, and the passes write light *i* into channel *i*. Casters past the eighth must keep -1.
- A zero visibility must stay an exact zero term: the `shadow == 0.0` skip is part of what makes clustered lighting equal the every-light loop.
- `ShadowPushConstant` and `shadow_csm.vert`'s push block must stay a single `mat4 viewProj` at 64 bytes; the C++ side is statically asserted, the GLSL side is not.
- Cascade shadows apply to directional lights only, with a constant bias; `cascades.normalBias`, `params.z` and the host's `lightSpaceMatrix` are read by no shader.
