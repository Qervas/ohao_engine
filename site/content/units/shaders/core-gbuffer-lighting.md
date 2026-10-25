---
module: shaders
id: core-gbuffer-lighting
title: Core GBuffer & lighting shaders
standard: v2
---

## The decision that shapes the rest: position lives in the buffer

`gbuffer.frag` writes four colour attachments plus depth, and the first one
settles the character of the whole pipeline: it stores the interpolated **world
position**, verbatim, with metallic in alpha.

{{cite shaders/core/gbuffer.frag "outGBuffer0 = vec4(fragWorldPos, metallic);"}}

The lighting pass reads it straight back and never touches the depth buffer.

{{cite shaders/core/deferred_lighting.frag "vec3 fragPos = gBuffer0Sample.rgb;"}}

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
`invViewProj * vec4(ndc, depth, 1)`. Both ingredients are present and correct:
the `D32_SFLOAT` depth image goes into descriptor binding 4 every frame, and
`invViewProj` occupies the first 64 bytes of the lighting push-constant block.
Neither is read — the shader declares no sampler at binding 4, and `invViewProj`
appears exactly once in the file, in its own declaration.

{{cite shaders/core/deferred_lighting.frag "    mat4 invViewProj;"}}

Storing position buys simplicity — no matrix, no NDC bookkeeping, no near/far
dependence — for 6 bytes per pixel plus the precision above. Every other reader made
the same call: SSAO, SSR, SSS and both RT techniques read the stored position too,
and `reconstruction.glsl`, the depth-to-position helper, now has no includer at all.
:::

## What the fragment shader packs

Normals are octahedron-encoded to two channels and then remapped into $[0,1]$.

{{cite shaders/core/gbuffer.frag "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

That remap is a UNORM convention, left from when the target was
`R10G10B10A2_UNORM` and could not hold negatives. The allocator's comment still
says `RGB10A2` and the shared header still calls itself the `A2R10G10B10`
encoder, but the image is half-float now; the remap survives only because
`decodeNormalOctahedron` undoes it symmetrically on the read side. That decoder
lives in `includes/common/encoding.glsl`, and `deferred_lighting.frag` is now its only
consumer; the Slang passes — SSAO, SSR, RT shadows and RT GI — each carry their own
`decodeOctahedron` with the same remap.

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The header also carries an `encodeNormalOctahedron` with the same math as the one
`gbuffer.frag` defines locally, and it has zero callers anywhere in `shaders/`:
the only encode in the tree is the local one. Nothing structural forces that
duplication — `gbuffer.frag` enables `GL_GOOGLE_include_directive` and includes
`scene_data.glsl` and the generated `shared/layout.glsl`, just not `encoding.glsl`.
The pair that must stay in
lockstep is `gbuffer.frag`'s encoder against the header's *decoder*; the header's
encoder is dead weight.

Where the material comes from is part of the same story. The draws are indirect, and a
command's `firstInstance` is the draw's index in `RenderScene`'s draw table, so
`gbuffer.vert` begins by reading the draw record at `gl_InstanceIndex` (set 1, binding
5): its instance slot selects the instance, and its material slot goes to the fragment
shader as a flat varying.

{{cite shaders/core/gbuffer.vert "const uint slot = d.draw.w;"}}

The fragment shader takes that submesh material slot, resolves it through the instance's
material-reference list into a row of `RenderScene`'s material table, and loads three
`vec4`s from it: base colour and albedo texture, roughness, metallic, normal and emissive
textures, and the rough/metal texture with the emissive and subsurface strengths. The same table feeds the
path tracer, so the two renderers cannot disagree about which material a triangle has.
A slot the instance has no reference for falls back to material 0.

Bindless texture indices reach the shader by bit-copy rather than conversion; the table
packer reinterprets each uint32 as a float:

{{cite ohao/render/scene/material_table.cpp "const auto bits = [](uint32_t u) { return std::bit_cast<float>(u); };"}}

so `UINT32_MAX` arrives as a quiet NaN and is recovered with `floatBitsToUint`.
Nothing does arithmetic on the lane, so the pattern survives intact — but the two
"no texture" tests are not the same test. Albedo and normal compare against the
sentinel; rough/metal and emissive compare against the array capacity.

{{cite shaders/core/gbuffer.frag "if (roughMetalTexIdx < 4096u) {"}}

4096 is the descriptor count the renderer asks `BindlessTextureManager` for at
init — the value that becomes the variable-length array's `descriptorCount` — so
the second form is strictly safer: it also rejects a stale but in-range-looking
index that would otherwise read past the end of the array.

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

That sample is then
read as ORM — R occlusion, G roughness, B metallic — though glTF defines only G
and B, so an asset that packs nothing into R darkens by whatever sits there.

## The emissive channel that forgets its colour

There is no emissive attachment. Emission is collapsed to a single Rec. 709
luminance scalar and hidden in the alpha of the normal target:

{{cite shaders/core/gbuffer.frag "emissiveLuminance = dot(emissiveColor, vec3(0.2126, 0.7152, 0.0722)) * row2.y;"}}

It shares that alpha with the subsurface strength, by sign: a non-emissive material
with `subsurface` > 0 stores minus its strength there, for the SSS blur, so the
lighting pass clamps the channel at zero before it reads emission:

{{cite shaders/core/deferred_lighting.frag "float emissiveLuminance = max(gBuffer1Sample.a, 0.0);  // stored by GBuffer pass (negative: subsurface)"}}

and then re-tints that scalar with the surface albedo:

{{cite shaders/core/deferred_lighting.frag "vec3 emissive = albedo * emissiveLuminance;"}}

So an emitter can only glow in its own base colour — a blue LED on a white
housing renders as a white glow of the right brightness. And it cannot glow
*hard*: the strength lane, `row2.y`, is set to exactly `1.0f` for any material that has
an emissive texture and left at 0 otherwise — model upload has no other path that raises
it — and the bindless textures it multiplies are 8-bit
(`R8G8B8A8_SRGB`/`_UNORM`), so the luminance is bounded by 1.0. The half-float
alpha has HDR headroom nothing in this pipeline can fill.

{{cite ohao/render/scene/model_upload.cpp "if (m.emissiveTex != kNoTexture) m.emissiveStrength = 1.0f;"}}

The sharper limitation is upstream of the shader: a material has no emissive colour
factor at all, only a texture index and that strength. The shader gates on the
*index*, so a material without a resolvable emissive map contributes nothing here
regardless of what the source asset's emissive factor said.

## Velocity: right about Vulkan, and about objects

The motion vector is the raw NDC difference, halved, with no vertical flip.

{{cite shaders/core/gbuffer.frag "outVelocity = (currentNDC - prevNDC) * 0.5;"}}

Vulkan's NDC y points down, the same direction as texture v, so
$uv = 0.5\,ndc + 0.5$ in both axes and the halved difference *is* the UV delta
`taa_resolve.frag` subtracts to find history. A GL-derived version needs a `y`
negate here; this one must not.

The "previous" position pairs last frame's view-projection with the instance's
**previous** world matrix, read from the instance table in the vertex shader:

{{cite shaders/core/gbuffer.vert "fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(inPosition, 1.0));"}}

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

Before any BRDF work, `deferred_lighting.frag` rewrites albedo, roughness and
metallic in place through a weather stack — wetness, snow, mud, frost — each
gated on its own push-constant scalar. Three of the four are keyed off `N.y`:
wetness and mud through `clamp(N.y * 2, 0, 1)`, snow through a steeper
`clamp((N.y - 0.3) / 0.7, 0, 1)`, so water pools and snow settles only on
upward-facing surfaces.

{{cite shaders/core/deferred_lighting.frag "roughness  = mix(roughness, max(roughness * 0.15, 0.02), w);"}}

Frost is the exception, and it reads as an oversight rather than a choice: its
block computes no slope factor at all, so a nonzero `frostCover` lays its smooth
pale-blue coating over every surface in the frame with no attenuation whatever —
ceilings, undersides and vertical walls included.

{{cite shaders/core/deferred_lighting.frag "if (pc.frostCover > 0.001) {"}}

This is a real authoring feature of the raster path with no counterpart in the RT
shaders: the path tracer reads its material from the SSBO and never sees wetness
or snow. Only after the stack runs is `initBRDFSurface` called, where the
metallic slider gets its `smoothstep(0.35, 0.65)` sharpening.

## The froxel's lights, and a falloff engineered not to pop

The loop runs over the pixel's froxel's light list, the indices the `lights.cluster`
pass wrote for the froxel the pixel's screen tile and view depth fall in (the
clustered-lighting unit), so a pixel shades the lights that can reach it out of a table
of up to 65,536:

{{cite shaders/core/deferred_lighting.frag "lightCount = clusterData[cluster];"}}

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

{{cite shaders/core/deferred_lighting.frag "return invSq * windowing * windowing;"}}

## What is bound, and what is actually read

Shadowing resolves to one scalar per light iteration, and RT visibility, when
present, wins outright over cascaded shadow maps — the CSM branch is `else if`,
not an extra multiply. The RT visibility is an `R32G32_UINT` image of eight 8-bit
channels, one per shadow-casting light (x: lights 0-3, y: 4-7), bound as a
`usampler2D` and fetched once at `gl_FragCoord` before the loop; each light then
picks its own byte by the slot the light table wrote into its `params.w`:

{{cite shaders/core/deferred_lighting.frag "texelFetch(rtShadowVisibility, ivec2(gl_FragCoord.xy), 0).xy"}}

{{cite shaders/core/deferred_lighting.frag "const uint word = slot < 4 ? rtShadow.x : rtShadow.y;"}}

A slot of -1 (the light casts no shadows, is an emissive proxy, or is a caster past
the first eight) is never occluded; slots 0 to 7 go to the first eight casters in
light order.

CSM picks a cascade by positive view-space depth and takes a 3×3 PCF of hardware
depth comparisons (a `sampler2DArrayShadow`); it neither blends between cascades nor applies a normal-offset bias, though
the cascade UBO carries `cascadeBlendWidth` and `normalBias` for exactly that. It
also runs only for `lightType == 0` with a non-negative slot: point and spot lights
cast no shadow without RT.

Ambient is defensive because every input can legitimately be absent. IBL is gated
not only on its flag but on the env map being larger than 1×1 — how the shader
tells a real environment from the dummy image the descriptor writer substitutes
when none is loaded:

{{cite shaders/core/deferred_lighting.frag "if ((pc.flags & 1u) != 0u && textureSize(envMap, 0).x > 1) {"}}

Inside the branch the defensiveness stops being defensive and becomes the
pipeline. Bindings 7 and 8 (irradiance and prefiltered cubemaps) and binding 9
(the split-sum BRDF LUT) are all fed by a single setter, and that setter has zero
callers repo-wide:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_lightingPass->setIBLTextures(irradiance, prefiltered, brdfLUT, iblSampler);"}}

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfos[8].imageView = m_brdfLUTView != VK_NULL_HANDLE ? m_brdfLUTView : fallbackView;"}}

The shader declares no samplers for 7 and 8 at all, so those two are pure
descriptor-layout ballast. Binding 9 it does declare — which makes the
"unpopulated BRDF LUT" branch not a contingency but the only split-sum the
deferred path ever evaluates:

{{cite shaders/core/deferred_lighting.frag "brdf = vec2(max(1.0 - roughness, 0.04), roughness * 0.25);"}}

Nor is there prefiltering to fall back *from*. The env image the deferred pass
receives is created with a single mip level and viewed with `levelCount = 1`:

{{cite ohao/gpu/vulkan/env_upload.cpp "imgInfo.mipLevels = 1;"}}

and the sampler handed over beside it is a zero-initialised
`VkSamplerCreateInfo` that never assigns `maxLod`, leaving it 0.0:

{{cite ohao/gpu/vulkan/env_upload.cpp "VkSamplerCreateInfo info{};"}}

Both `textureLod` calls therefore clamp to mip 0 — and their third argument is an
absolute LOD, not a bias. The `roughness * 9.0` specular tap and the fixed `9.0`
irradiance tap read the same full-resolution equirect texels: env reflections are
mirror-sharp at every roughness, and the diffuse "irradiance" is one texel of the
raw HDR in the direction of `N`.

{{cite shaders/core/deferred_lighting.frag "vec3 irradiance = textureLod(envMap, nUV, 9.0).rgb;"}}

Which leaves the third fallback — an `F0`-weighted ambient floor for
`metallic > 0.5` — as the thing standing between a dark metal and black when that
single texel comes back dim.

Screen-space AO does reach these terms. When the effect is on, the render graph runs
`executeSSAO`, hands the half-resolution view to the lighting pass, and announces it
via flag bit 1:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_ssaoView != VK_NULL_HANDLE) m_params.flags |= 2;       // SSAO"}}

and the shader samples binding 10 under that bit, upsampling the (occlusion, distance)
pairs against its own distance from the camera — the four nearest samples weighted
bilinearly and by a 5 % distance match, the closest one where none matches:

{{cite shaders/core/deferred_lighting.frag "float ssaoAt(ivec2 pixel, float dist) {"}}

The result multiplies the constant ambient and the IBL terms, on top of the material
occlusion channel in GBuffer2's alpha; direct light and RT GI never see it. The deferred
SSAO unit covers the pass that produces it.

:::key
This GBuffer is not a compressed encoding of a surface but a lossy
*re-authoring* of one: position quantised to half-float, emission reduced to a
luminance the lighting pass re-tints with albedo, then albedo/roughness/metallic
edited again for weather before the BRDF runs. The deferred image is not showing
the material the path tracer receives.
:::

## Contracts

- Background is detected by exact float equality — `gBuffer0.rgb == vec3(0)` **and** `.a == 0`, matching the clear value. Geometry landing on the world origin with metallic exactly 0 is discarded as sky.
- `gbuffer.frag`'s local `encodeNormalOctahedron` and `encoding.glsl`'s `decodeNormalOctahedron` are the matched pair, as are the `* 0.5 + 0.5` on write and the `* 2.0 - 1.0` on read. Change either side without the other and every normal in the frame rotates. The header's own `encodeNormalOctahedron` is not part of that contract — it has no callers, and editing it changes nothing.
- `LightingParams` (C++) and the GLSL push-constant block must stay byte-identical at 184 bytes, including the unread `invViewProj` and `screenSize` fields.
- Double-sided materials rasterise with `VK_CULL_MODE_NONE` (one-sided ones cull back faces) and the shader has no `gl_FrontFacing` flip, so the visible back faces of a double-sided material store outward-pointing normals and shade dark. `gbuffer.vert` also transforms the tangent by the inverse-transpose normal matrix, correct only under uniform scale.
