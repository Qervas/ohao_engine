---
module: shaders
id: includes-shadow
title: Shadow sampling in lighting
standard: v2
---

## No shadow header, two shadow sources

There is no shadow include directory. The raster frame's shadow lookups are written in
`deferred_lighting.slang`, and they take one of two forms per frame: a ray-traced
visibility image, or, where the frame cannot trace, a cascaded shadow map for
directional lights. The host picks one when the lighting pass records: the RT bit and
the CSM bit are the two arms of one if/else on whether an RT visibility view is bound.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "m_params.flags |= 4;   // CSM fallback (bit 2)"}}

The only shadow stage file left is the cascade depth write, `shadow/shadow_csm.vert`
(below). The lighting pass gathers what it has into one `LightVisibility` (the RT words,
or the directional light's CSM term) and hands it to `directLight` in the shared light
loop, `includes/lighting/clustered_lights.slang`, which applies it per light after the
attenuation and before the BSDF. Either way it is *visibility*: 1 lit, 0 occluded,
multiplied straight into that light's contribution.

{{cite shaders/includes/lighting/clustered_lights.slang "Lo += evaluateLight(s, L, radiance, t) * shadow;"}}

The forward transparent pass uses the same loop with a single directional visibility from
its own sun ray (`directionalVisibility`), so it never reads the RT image.

## Eight channels, one byte each

The RT image is `R32G32_UINT` at the lighting pass's size, bound at binding 14 as an
integer sampler: eight 8-bit channels, x holding lights 0 to 3 and y lights 4 to 7, lowest
byte first. It is fetched once per pixel before the loop, by integer coordinate:

{{cite shaders/core/deferred_lighting.slang "[[vk::binding(14, 0)]] Sampler2D<uint2> rtShadowVisibility;   // RG32UI: 8 bits a shadow channel"}}

A light finds its channel in `params.w`. The light table hands channels out in light
order to the first eight lights that cast shadows, and writes -1 into every other
light, which is then never occluded:

{{cite shaders/includes/lighting/clustered_lights.slang "const uint word = slot < 4 ? vis.rtWords.x : vis.rtWords.y;"}}

Eight is the image's width in bytes, not the light table's size: the raster table holds
up to 65,536 lights, so a scene with a ninth caster renders it unshadowed. The light
table counts those lights (`RenderScene::shadowCastersWithoutChannel()`), and test 8
checks the case on Cornell, whose twelve casting lights give eight channels and four
unshadowed lights.

{{cite ohao/render/scene/light_table.cpp "if (m_shadowLights.size() < kMaxShadowLights) {"}}

A channel that reads 0 makes the light's term exactly zero, and the loop skips it
before the BSDF. That skip matters to clustered lighting, which is exact only if every
term it leaves out or skips would have added $+0$:

{{cite shaders/includes/lighting/clustered_lights.slang "if (shadow == 0.0) continue;   // fully occluded: adds exactly zero"}}

## The cascade fallback

Without RT shadows, a light with a channel is shadowed by the cascades if it is
directional, and only then: the lighting pass computes one CSM term when the CSM bit is
set, and the loop applies it only to `type == 0`, so point and spot lights cast no
shadow in a frame that cannot trace.

{{cite shaders/core/deferred_lighting.slang "vis.directional = !vis.rt && (pc.flags & 4u) != 0u ? shadowCSM(P, viewDepth) : 1.0;"}}

{{cite shaders/includes/lighting/clustered_lights.slang "shadow = vis.directional;"}}

The cascade is chosen by positive view-space depth against the cascade UBO's split
distances:

{{cite shaders/core/deferred_lighting.slang "if (viewDepth < cascades.splitDepths[i]) return i;"}}

and the lookup is a 3×3 PCF whose nine taps are hardware depth comparisons: the map is
bound as a `Sampler2DArrayShadow` through the pass's `LESS_OR_EQUAL` comparison sampler,
each tap a `SampleCmpLevelZero` against the biased reference depth:

{{cite shaders/core/deferred_lighting.slang "[[vk::binding(6, 0)]] Sampler2DArrayShadow shadowMap;   // CSM, comparison LESS_OR_EQUAL"}}
{{cite shaders/core/deferred_lighting.slang "shadow += shadowMap.SampleCmpLevelZero(float3(proj.xy + float2(x, y) * texel, float(c)), reference);"}}

The bias is the cascade UBO's `shadowBias`, a constant 0.012 subtracted at every
incidence angle. A depth map's error grows with the tangent of the angle between the
surface and the light, so a constant is too much at normal incidence (light leaks at
contact) or too little at grazing angles (acne); the `normalBias` beside it in the same
struct, the usual remedy, is never read by the shader.

{{cite ohao/render/deferred/render_pass_base.hpp "float shadowBias{0.012f};"}}

`shadowCSM` takes the fragment position and view depth and nothing about the light, and
runs once per pixel: every shadow-casting directional light gets the same value. The
cascades are rendered from a single direction, the deferred renderer's own sun direction
(the sky's, or `setDirectionalLight`'s), whichever directional light the loop is shading:

{{cite shaders/core/deferred_lighting.slang "float shadowCSM(float3 p, float viewDepth)"}}
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

`LightData.lightSpaceMatrix` is identity for every scene light and is dropped when
`LightClusters` copies `LightData` into `LightGpu`: no shadow map is rendered for a point or
spot light. `params.z` is not a shadow field: it is the light's sphere radius, for its
specular.

{{cite ohao/render/scene/light_table.cpp "// z: the sphere's radius for a point or spot light with an emitter mesh"}}

## Contracts

- RT shadows (bit 5) and CSM (bit 2) are exclusive per frame, decided by whether an RT visibility view is bound when the lighting pass records. The `csm` pass runs only in frames that do not trace.
- `params.w` and the visibility's channels are one contract with the light table and the RT shadow passes: the table hands out channels 0 to 7 in the order it lists `shadowLights()`, and the passes write light *i* into channel *i*. Casters past the eighth must keep -1.
- A zero visibility must stay an exact zero term: the `shadow == 0.0` skip is part of what makes clustered lighting equal the every-light loop.
- `ShadowPushConstant` and `shadow_csm.vert`'s push block must stay a single `mat4 viewProj` at 64 bytes; the C++ side is statically asserted, the GLSL side is not.
- Cascade shadows apply to directional lights only, with a constant bias; `cascades.normalBias` and the host's `lightSpaceMatrix` are read by no shader.
