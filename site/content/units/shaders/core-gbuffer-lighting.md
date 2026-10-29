---
module: shaders
id: core-gbuffer-lighting
title: Core GBuffer & lighting shaders
standard: v2
---

## The decision that shapes the rest: position lives in the buffer

`gbuffer.frag` writes seven colour outputs plus depth, and the first one settles the
character of the whole pipeline: it stores the interpolated **world position**,
verbatim, with metallic in alpha.

{{cite shaders/includes/scene/gbuffer_body.glsl "outGBuffer0 = vec4(fragWorldPos, metallic);"}}

The lighting pass, `core/deferred_lighting.slang`, reads it straight back and never
touches the depth buffer.

{{cite shaders/core/deferred_lighting.slang "const float3 P = g0.rgb;"}}

That target is `R16G16B16A16_SFLOAT` — IEEE binary16, an 11-bit significand. In
the binade $[2^e, 2^{e+1})$ the spacing between representable values is

$$\mathrm{ulp}(x) = 2^{\lfloor \log_2 |x| \rfloor - 10}$$

where $x$ is a world coordinate in metres. At 10 m from the origin that is
$2^{-7} \approx 7.8$ mm; at 100 m, 62.5 mm; at 1 km, half a metre; past 65504 m
it becomes `inf`. That quantised position feeds the view vector, the distance in
the inverse-square falloff, and the shadow-map lookup, so on a large outdoor
scene the error is geometric rather than a banding artefact.

:::why
The alternative is to store nothing and recover position from depth via
`invViewProj * (ndc, depth, 1)`. Both ingredients are present: the `D32_SFLOAT`
depth image is still a combined-image-sampler slot in the pass's layout (binding 4),
and `invViewProj` occupies the first 64 bytes of the lighting push-constant block.
The shader declares nothing at binding 4, and uses `invViewProj` once, for the sky:
a pixel with no geometry builds its view ray from the far plane to look up the
environment.

{{cite shaders/core/deferred_lighting.slang "const float4 far = mulColumns(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));"}}

Storing position buys simplicity — no matrix, no NDC bookkeeping, no near/far
dependence — for 6 bytes per pixel plus the precision above. Every other reader made
the same call: SSAO, SSR, SSS and both RT techniques read the stored position too,
and `reconstruction.glsl`, the depth-to-position helper, has no includer at all.
:::

## What the fragment shader packs

Normals are octahedron-encoded to two channels and then remapped into $[0,1]$.

{{cite shaders/includes/scene/gbuffer_body.glsl "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

That remap is a UNORM convention, left from when the target was
`R10G10B10A2_UNORM` and could not hold negatives. The image is half-float now; the
remap survives only because every reader undoes it symmetrically. Each reader
carries its own decoder: the lighting pass its `decodeNormalOctahedron`, and SSAO,
SSR and GI's denoiser a `decodeOctahedron` with the same remap.

{{cite shaders/core/deferred_lighting.slang "const float2 f = e * 2.0 - 1.0;"}}

`includes/common/encoding.glsl` still holds a GLSL decoder and an
`encodeNormalOctahedron` with the same math as the one `gbuffer_body.glsl` defines
locally, but no shader includes it any more: the lighting pass that was its last
consumer is Slang now.

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The pair that must stay in lockstep is the G-buffer's encoder against each reader's
decoder; the header is dead weight.

Where the material comes from is part of the same story. The draws are indirect, and a
command's `firstInstance` is the draw's index in `RenderScene`'s draw table, so
`gbuffer.vert` begins by reading the draw record at `gl_InstanceIndex` (set 1, binding
5): its instance slot selects the instance, and its material slot goes to the fragment
shader as a flat varying.

{{cite shaders/core/gbuffer.vert "const uint slot = d.draw.w;"}}

The fragment shader takes that submesh material slot, resolves it through the instance's
material-reference list into a row of `RenderScene`'s material table, and loads six of
its seven `vec4` rows (row 3, alpha, only under `OHAO_GBUFFER_MASK`): base colour and
albedo texture; roughness, metallic, normal and emissive textures; the rough/metal
texture, emission luminance, subsurface and occlusion texture; and OpenPBR's specular,
base and emission rows.

{{cite shaders/includes/scene/gbuffer_body.glsl "vec4 row5 = matRows[m * kMaterialRows + 5u];   // specular_ior, base_diffuse_roughness, base_weight, specularTex"}}

The same table feeds the path tracer, so the two renderers cannot disagree about which
material a triangle has. A slot the instance has no reference for falls back to
material 0.

Bindless texture indices reach the shader by bit-copy rather than conversion; the table
packer reinterprets each uint32 as a float:

{{cite ohao/render/scene/material_table.cpp "const auto bits = [](uint32_t u) { return std::bit_cast<float>(u); };"}}

so `UINT32_MAX` arrives as a quiet NaN and is recovered with `floatBitsToUint`.
Nothing does arithmetic on the lane, so the pattern survives intact — but the
"no texture" tests are not all the same test. Albedo and normal compare against the
sentinel; rough/metal, occlusion, emissive and the two specular textures compare
against the array capacity.

{{cite shaders/includes/scene/gbuffer_body.glsl "if (roughMetalTexIdx < 4096u) {"}}

4096 is the descriptor count the renderer asks `BindlessTextureManager` for at
init, so the second form is strictly safer: it also rejects a stale but
in-range-looking index that would otherwise read past the end of the array.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

The rough/metal sample is read for G (roughness) and B (metallic) only, as glTF
defines them; occlusion comes from glTF's own occlusion texture. Roughness is stored as
authored, with no floor: OpenPBR's module holds it at `kMinSpecularRoughness` (0.01)
when it shades, in the lighting pass and the path tracer alike.

{{cite shaders/material/openpbr.slang "rough = clamp(m.specularRoughness, kMinSpecularRoughness, 1.0);"}}

## OpenPBR's inputs: two more targets

OpenPBR's base needs more than albedo, roughness and metallic, so the G-buffer has two
targets for the rest. `kSpecular` (output 5, `RGBA8_UNORM`) holds `specular_color` and
`specular_weight / 1.5`, with the KHR specular textures applied; `kOpenPBR` (output 6,
`RG8_UNORM`) holds `base_diffuse_roughness` and `(specular_ior - 1) / 2.55`. The scales
are chosen so the defaults are exact codes: a weight of 1 is 170, an IOR of 1.5 is 50.

{{cite shaders/includes/scene/gbuffer_body.glsl "outSpecular = vec4(clamp(specularColor, 0.0, 1.0), clamp(specularWeight / 1.5, 0.0, 1.0));"}}

{{cite shaders/includes/scene/gbuffer_body.glsl "outOpenPBR = vec2(clamp(row5.y, 0.0, 1.0), clamp((row5.x - 1.0) / 2.55, 0.0, 1.0));"}}

The albedo target carries `base_weight × base_color`, which is OpenPBR's diffuse albedo
and a metal's F0 at once; the lighting pass therefore passes a base weight of 1. The
lighting pass undoes both encodings and rebuilds `MaterialInputs`:

{{cite shaders/core/deferred_lighting.slang "mi.specularIor = 1.0 + pbr.g * 2.55;"}}

## The emissive attachment

Emission is an `RGBA16F` attachment of its own (`kEmissive`, output 4):
`emission_color × emission_luminance`, times the emissive texture's colour when there is
one (sRGB-decoded by the bindless sampler), the same radiance the path tracer's closest
hit resolves:

{{cite shaders/includes/scene/gbuffer_body.glsl "vec3 emissive = row6.rgb * row2.y;"}}

{{cite shaders/rt/path_tracer.slang "float3 emission = row6.rgb * row2.y;"}}

Lighting adds it unlit, in its own colour. The normal target's alpha keeps only its
luminance, to keep an emissive surface out of the SSS blur. Because the colour is a
material factor (row 6) and the texture only multiplies it, a material with an emission
colour and no emissive map still emits.

## Velocity: right about Vulkan, and about objects

The motion vector is the raw NDC difference, halved, with no vertical flip.

{{cite shaders/includes/scene/gbuffer_body.glsl "outVelocity = (currentNDC - prevNDC) * 0.5;"}}

Vulkan's NDC y points down, the same direction as texture v, so
$uv = 0.5\,ndc + 0.5$ in both axes and the halved difference *is* the UV delta
`taa_resolve.frag` subtracts to find history. A GL-derived version needs a `y`
negate here; this one must not.

The "previous" position pairs last frame's view-projection with the instance's
**previous** world matrix, read from the instance table in the vertex shader:

{{cite shaders/core/gbuffer.vert "fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(prevLocal, 1.0));"}}

(`prevLocal` is the vertex's position, or on a skinned draw last frame's deformed one, which
the skin pass leaves in `boneWeights.xyz`)

so a moving object is reprojected from where it was, and TAA's history follows it
rather than smearing it. What it does not capture is the other half of the pair: the
current projection is Halton-jittered for TAA while the stored previous one is not:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_prevViewProj = m_proj * m_view;   // unjittered"}}

so a static camera over a static scene still writes a non-zero velocity — that
frame's jitter offset, and nothing else. It is smaller than it sounds: the Halton
offsets are bounded by half a pixel, divided by the render resolution, and halved
again on the way from NDC to UV.

{{cite ohao/render/deferred/taa_pass.cpp "return m_jitterSequence[sampleIndex] / glm::vec2(m_width, m_height);"}}

At 1080p that caps the velocity magnitude near $2.7 \times 10^{-4}$; with
`motionScale` at 100 it is a `motionWeight` of at most 0.03, moving the history
blend from 0.9 to about 0.89. Off the floor, and not by much.

{{cite shaders/postprocess/taa_resolve.frag "float motionWeight = clamp(velocityMag * params.motionScale, 0.0, 1.0);"}}

## The lighting pass edits the material it just unpacked

Before it builds the surface, `deferred_lighting.slang` rewrites albedo, roughness and
metallic in place through a weather stack — wetness, snow, mud, frost — each
gated on its own push-constant scalar. Three of the four are keyed off `N.y`:
wetness and mud through `saturate(N.y * 2)`, snow through a steeper
`saturate((N.y - 0.3) / 0.7)`, so water pools and snow settles only on
upward-facing surfaces.

{{cite shaders/core/deferred_lighting.slang "roughness = lerp(roughness, max(roughness * 0.15, 0.02), w);"}}

Frost is the exception, and it reads as an oversight rather than a choice: its
block computes no slope factor at all, so a nonzero `frostCover` lays its smooth
pale-blue coating over every surface in the frame with no attenuation whatever —
ceilings, undersides and vertical walls included.

{{cite shaders/core/deferred_lighting.slang "if (pc.frostCover > 0.001)"}}

This is a real authoring feature of the raster path with no counterpart in the RT
shaders: the path tracer reads its material from the SSBO and never sees wetness
or snow. Only after the stack runs does `makeSurface` build the surface's frame and
its `OpenPBRBase` from the edited inputs.

{{cite shaders/core/deferred_lighting.slang "const Surface s = makeSurface(P, N, V, mi);"}}

## The froxel's lights, and a falloff engineered not to pop

The pixel's light list comes from `lightList` in `clustered_lights.slang`: the indices
the `lights.cluster` pass wrote for the froxel the pixel's screen tile and view depth fall
in (the clustered-lighting unit), so a pixel shades the lights that can reach it out of a
table of up to 65,536. `directLight` then walks the list and shades each light through
OpenPBR's base:

{{cite shaders/core/deferred_lighting.slang "const float3 Lo = directLight(s, lights, clusters, list, clustered, vis, tables);"}}

Point and spot lights use a windowed inverse-square:

$$\mathrm{atten}(d) = \frac{\big[\mathrm{clamp}\!\left(1 - (d/r)^4,\ 0,\ 1\right)\big]^{2}}{\max(d^{2},\ 0.01)}$$

with $d$ the distance from the shaded point to the light and $r$ the light's
range, packed in `direction.w`. The denominator is the physical term, floored so
the gain saturates at $100\times$ rather than diverging as $d \to 0$. The
numerator is the windowing factor, and it is *squared* on purpose: both the
window and its first derivative vanish at $d = r$, making the whole attenuation
$C^1$ there, so a light does not visibly snap off at its range boundary. Past
$d = r$ the clamp makes it exactly 0, and the loop skips such a light; that hard zero
is why the froxel lists, which leave out lights out of range, change no pixel.

{{cite shaders/includes/lighting/clustered_lights.slang "return invSq * windowing * windowing;"}}

## What is bound, and what is actually read

Shadowing resolves to one scalar per light iteration, carried in a `LightVisibility`,
and RT visibility, when present, wins outright over cascaded shadow maps: the pass
computes the CSM term only when RT shadows are off. The RT visibility is an
`R32G32_UINT` image of eight 8-bit channels, one per shadow-casting light (x: lights
0-3, y: 4-7), fetched once at the pixel before the loop; each light then picks its own
byte by the slot the light table wrote into its `params.w`:

{{cite shaders/core/deferred_lighting.slang "vis.rtWords = vis.rt ? rtShadowVisibility.Load(int3(int2(fragCoord.xy), 0)).xy : uint2(0u);"}}

{{cite shaders/includes/lighting/clustered_lights.slang "const uint word = slot < 4 ? vis.rtWords.x : vis.rtWords.y;"}}

A slot of -1 (the light casts no shadows or is a caster past the first eight) is never
occluded; slots 0 to 7 go to the first eight casters in light order.

CSM picks a cascade by positive view-space depth and takes a 3×3 PCF of hardware
depth comparisons (a `Sampler2DArrayShadow`); it neither blends between cascades nor
applies a normal-offset bias, though the cascade UBO carries `cascadeBlendWidth` and
`normalBias` for exactly that. It also shadows only directional lights with a
non-negative slot: point and spot lights cast no shadow without RT.

{{cite shaders/core/deferred_lighting.slang "vis.directional = !vis.rt && (pc.flags & 4u) != 0u ? shadowCSM(P, viewDepth) : 1.0;"}}

Ambient is defensive because every input can legitimately be absent. IBL runs under
flag bit 0, which the pass raises only when both of `EnvPrefilter`'s images are bound:
the environment GGX-prefiltered into `kEnvPrefilterLevels` mips (binding 15) and its
irradiance (binding 7). Without an environment both bindings hold the dummy view and the
branch never runs.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_prefilteredView != VK_NULL_HANDLE && m_irradianceView != VK_NULL_HANDLE)"}}

Inside the branch the split sum reads the prefiltered level for the pixel's roughness
along the reflection, weighted by RT GI's environment visibility with GI's bounce
radiance for the occluded rest, times the reflection weight. That weight is the specular
lobes' own directional albedo toward the viewer, from OpenPBR's energy tables (binding
20) rather than a fitted environment BRDF, times a specular-occlusion term built from the
material's `ao` and SSAO (the deferred lighting unit, "Image-based lighting"). It is
computed before the branch, for every pixel, because SSR's resolve weights its
reflections by it too; the pass writes it and the environment's specular beside the lit
image for that resolve (the SSR and SSS unit):

{{cite shaders/core/deferred_lighting.slang "const float3 specularWeight = specularAlbedo * specularOcclusion(NdotV, occlusion, roughness);"}}

Every indirect diffuse term is weighted by the diffuse's directional albedo beneath the
specular (`diffuseAlbedo`: EON's, scaled by what the specular lets through, zero for a
metal). The constant ambient is the last resort, applied only when the frame has neither
RT GI nor an environment; it has no counterpart in the path tracer.

{{cite shaders/core/deferred_lighting.slang "else if (!ibl) ambient += pc.ambientIntensity * diffuseAlbedo * ao * ssao;"}}

The prefiltered images are the deferred frame's own; the env image the path tracer and
RT GI sample keeps its single mip level and its base-level sampler, so nothing here can
move a path-traced pixel:

{{cite ohao/gpu/vulkan/env_upload.cpp "imgInfo.mipLevels = 1;"}}

Screen-space AO does reach these terms. When the effect is on, the render graph runs
`executeSSAO`, hands the half-resolution view to the lighting pass, and announces it
via flag bit 1:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_ssaoView != VK_NULL_HANDLE) m_params.flags |= 2;       // SSAO"}}

and the shader samples binding 10 under that bit, upsampling the (occlusion, distance)
pairs against its own distance from the camera — the four nearest samples weighted
bilinearly and by a 5 % distance match, the closest one where none matches:

{{cite shaders/core/deferred_lighting.slang "float ssaoAt(int2 pixel, float dist)"}}

The result multiplies the constant ambient and the IBL terms, on top of the material
occlusion channel in GBuffer2's alpha; direct light and RT GI never see it. The deferred
SSAO unit covers the pass that produces it.

:::key
This GBuffer is a lossy re-encoding of the material the path tracer receives, not the
material itself: position quantised to half-float, OpenPBR's specular and base inputs
quantised to 8 bits, then albedo/roughness/metallic edited again for weather before the
BSDF runs. Without weather the BSDF is the same `OpenPBRBase` the path tracer samples;
what differs is how light reaches it.
:::

## Contracts

- Background is detected by exact float equality — all four channels of `gBuffer0` zero, matching the clear value. Geometry landing on the world origin with metallic exactly 0 is drawn as sky.
- `gbuffer_body.glsl`'s `encodeNormalOctahedron` and each reader's decoder are the matched pair, as are the `* 0.5 + 0.5` on write and the `* 2.0 - 1.0` on read. Change either side without the other and every normal in the frame rotates. `encoding.glsl` is not part of that contract — nothing includes it.
- The `kSpecular` and `kOpenPBR` encodings (`/ 1.5`, `(ior - 1) / 2.55`) are written in `gbuffer_body.glsl` and undone in `deferred_lighting.slang`; change one without the other and every dielectric's reflectance moves.
- `LightingParams` (C++) and the Slang push-constant block must stay byte-identical at 184 bytes, field for field.
- Double-sided materials rasterise with `VK_CULL_MODE_NONE` (one-sided ones cull back faces) and the shader has no `gl_FrontFacing` flip, so the visible back faces of a double-sided material store outward-pointing normals and shade dark. `gbuffer.vert` also transforms the tangent by the inverse-transpose normal matrix, correct only under uniform scale.
