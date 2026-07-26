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
dependence — for 6 bytes per pixel plus the precision above. Other passes made
the opposite call: `reconstruction.glsl` exists and `ssao.comp` uses it.
:::

## What the fragment shader packs

Normals are octahedron-encoded to two channels and then remapped into $[0,1]$.

{{cite shaders/core/gbuffer.frag "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

That remap is a UNORM convention, left from when the target was
`R10G10B10A2_UNORM` and could not hold negatives. The allocator's comment still
says `RGB10A2` and the shared header still calls itself the `A2R10G10B10`
encoder, but the image is half-float now; the remap survives only because
`decodeNormalOctahedron` undoes it symmetrically on the read side. That decoder
lives in `includes/common/encoding.glsl` and is the one every consumer runs —
`deferred_lighting.frag`, `ssao.comp`, `ssr.comp`, `rt_gi.rgen`, `rt_shadow.rgen`.

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The header also carries an `encodeNormalOctahedron` with the same math as the one
`gbuffer.frag` defines locally, and it has zero callers anywhere in `shaders/`:
the only encode in the tree is the local one. Nothing structural forces that
duplication — `gbuffer.frag` simply never enables `GL_GOOGLE_include_directive`,
which `deferred_lighting.frag` and `forward.frag` both do one line into the file,
in the same directory, under the same shader build. The pair that must stay in
lockstep is `gbuffer.frag`'s encoder against the header's *decoder*; the header's
encoder is dead weight.

Bindless texture indices reach the shader by bit-copy rather than conversion:

{{cite ohao/render/deferred/gbuffer_pass.cpp "float f; memcpy(&f, &idx, sizeof(float)); return f;"}}

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

{{cite shaders/core/gbuffer.frag "emissiveLuminance = dot(emissiveColor, vec3(0.2126, 0.7152, 0.0722)) * pc.emissiveParams.y;"}}

and the lighting pass re-tints that scalar with the surface albedo:

{{cite shaders/core/deferred_lighting.frag "vec3 emissive = albedo * emissiveLuminance;"}}

So an emitter can only glow in its own base colour — a blue LED on a white
housing renders as a white glow of the right brightness. And it cannot glow
*hard*: `pc.emissiveParams.y` is only ever 0.0 or exactly `1.0f` — no C++ path
raises it — and the bindless textures it multiplies are 8-bit
(`R8G8B8A8_SRGB`/`_UNORM`), so the luminance is bounded by 1.0. The half-float
alpha has HDR headroom nothing in this pipeline can fill.

{{cite ohao/render/deferred/gbuffer_pass.cpp "emissiveStrength = glm::length(mat.emissive) > 0.01f ? 1.0f : 0.0f;"}}

That line carries the sharper upstream limitation, though not by the route it
looks like: it sits inside a branch already guarded on
`mat.useEmissiveTexture && !mat.emissiveTexture.empty()`, and the `findTexture`
call above it can still miss, leaving strength 1.0 with `emissiveTexIdx` at
`UINT32_MAX`. The shader gates on the *index*, not the strength, so a material
with a nonzero emissive factor and no resolvable emissive map contributes nothing
at all here.

## Velocity: right about Vulkan, wrong about objects

The motion vector is the raw NDC difference, halved, with no vertical flip.

{{cite shaders/core/gbuffer.frag "outVelocity = (currentNDC - prevNDC) * 0.5;"}}

Vulkan's NDC y points down, the same direction as texture v, so
$uv = 0.5\,ndc + 0.5$ in both axes and the halved difference *is* the UV delta
`taa_resolve.frag` subtracts to find history. A GL-derived version needs a `y`
negate here; this one must not.

Two things it does not capture. The "previous" matrix pairs last frame's
view-projection with the **current** model matrix:

{{cite ohao/render/deferred/gbuffer_pass.cpp "const glm::mat4 prevMVP = m_prevViewProj * modelMatrix;"}}

so only camera motion is encoded — a moving actor reprojects as though static and
TAA smears it. And the current projection is Halton-jittered for TAA while the
stored previous one is not:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_prevViewProj = m_proj * m_view;"}}

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

## Eight lights, and a falloff engineered not to pop

The loop runs `min(pc.lightCount, MAX_LIGHTS)` times, and `MAX_LIGHTS` is 8, so
the deferred path shades at most eight lights per pixel however many the scene
holds. (The shader's comment sources that constant from `offscreen_renderer.hpp`,
a file that no longer exists; the matching C++ value now lives in
`ohao/gpu/vulkan/renderer.hpp`.) Point and spot lights use a windowed
inverse-square:

$$\mathrm{atten}(d) = \frac{\big[\mathrm{clamp}\!\left(1 - (d/r)^4,\ 0,\ 1\right)\big]^{2}}{\max(d^{2},\ 0.01)}$$

with $d$ the distance from the shaded point to the light and $r$ the light's
range, packed in `direction.w`. The denominator is the physical term, floored so
the gain saturates at $100\times$ rather than diverging as $d \to 0$. The
numerator is the windowing factor, and it is *squared* on purpose: both the
window and its first derivative vanish at $d = r$, making the whole attenuation
$C^1$ there, so a light does not visibly snap off at its range boundary.

{{cite shaders/core/deferred_lighting.frag "return invSq * windowing * windowing;"}}

## What is bound, and what is actually read

Shadowing resolves to one scalar per light iteration, and an RT mask, when
present, wins outright over cascaded shadow maps — the CSM branch is `else if`,
not an extra multiply. "Per light" flatters the RT path, though: the mask is
sampled at `inTexCoord` with no dependence on the loop index, so when flag bit 5
is set every light in the scene is occluded by the same screen-space term.

{{cite shaders/core/deferred_lighting.frag "shadow = texture(rtShadowMask, inTexCoord).r;"}}

CSM picks a cascade by positive view-space depth and takes a 3×3 PCF
tap; it neither blends between cascades nor applies a normal-offset bias, though
the cascade UBO carries `cascadeBlendWidth` and `normalBias` for exactly that,
and `shadowStrength` and the lighting UBO's `shadowBias` are equally inert. It
also runs only for `lightType == 0`: point and spot lights cast no shadow here.

Ambient is defensive because every input can legitimately be absent. IBL is gated
not only on its flag but on the env map being larger than 1×1 — how the shader
tells a real environment from the dummy image the descriptor writer substitutes
when none is loaded:

{{cite shaders/core/deferred_lighting.frag "if ((pc.flags & 1u) != 0u && textureSize(envMap, 0).x > 1) {"}}

That guard has history: the fallback used to be the GBuffer position texture,
sampled as an environment, producing garbage specular everywhere.

Inside the branch the defensiveness stops being defensive and becomes the
pipeline. Bindings 7 and 8 (irradiance and prefiltered cubemaps) and binding 9
(the split-sum BRDF LUT) are all fed by a single setter, and that setter has zero
callers repo-wide:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_lightingPass->setIBLTextures(irradiance, prefiltered, brdfLUT, iblSampler);"}}

`IBLProcessor` — the class that would convolve the irradiance cube, prefilter the
specular chain and integrate the LUT — is never instantiated either: the
identifier appears nowhere outside its own header and source, and the only thing
that pulls the file in at all is an `#include` line in the umbrella header
`render/render_module.hpp`. So all three views stay
`VK_NULL_HANDLE` and all three bindings are written every frame with the same 1×1
dummy image, which is layout-transitioned once and never uploaded to.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfos[8].imageView = m_brdfLUTView != VK_NULL_HANDLE ? m_brdfLUTView : fallbackView;"}}

The shader declares no samplers for 7 and 8 at all, so those two are pure
descriptor-layout ballast. Binding 9 it does declare — which makes the
"unpopulated BRDF LUT" branch not a contingency but the only split-sum the
deferred path ever evaluates:

{{cite shaders/core/deferred_lighting.frag "brdf = vec2(max(1.0 - roughness, 0.04), roughness * 0.25);"}}

Nor is there prefiltering to fall back *from*. The env image the deferred pass
receives is created with a single mip level and viewed with `levelCount = 1`:

{{cite ohao/gpu/vulkan/light_upload.cpp "imgInfo.mipLevels = 1;"}}

and the sampler handed over beside it is a zero-initialised
`VkSamplerCreateInfo` that never assigns `maxLod`, leaving it 0.0:

{{cite ohao/gpu/vulkan/rt_build.cpp "vkCreateSampler(m_device, &samplerInfo, nullptr, &m_rtTextureSampler);"}}

Both `textureLod` calls therefore clamp to mip 0 — and their third argument is an
absolute LOD, not a bias. The `roughness * 9.0` specular tap and the fixed `9.0`
irradiance tap read the same full-resolution equirect texels: env reflections are
mirror-sharp at every roughness, and the diffuse "irradiance" is one texel of the
raw HDR in the direction of `N`.

{{cite shaders/core/deferred_lighting.frag "vec3 irradiance = textureLod(envMap, nUV, 9.0).rgb;"}}

Which leaves the third fallback — an `F0`-weighted ambient floor for
`metallic > 0.5` — as the thing standing between a dark metal and black when that
single texel comes back dim.

Screen-space AO is the mirror image of that gap. Here the C++ side really does
the work — the render graph runs `executeSSAO`, hands the view to the lighting
pass, and announces it via flag bit 1:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_ssaoView != VK_NULL_HANDLE) m_params.flags |= 2;       // SSAO"}}

but the shader declares no binding 10 and never tests `flags & 2u`. The `ao` that
reaches the BRDF comes entirely from the material occlusion channel in GBuffer2's
alpha. SSAO is currently computed and discarded.

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
- `LightingParams` (C++) and the GLSL push-constant block must stay byte-identical at 200 bytes, including the unread `invViewProj` and `screenSize` fields.
- The pipeline rasterises with `VK_CULL_MODE_NONE` and the shader has no `gl_FrontFacing` flip, so back faces store outward-pointing normals and shade dark. `gbuffer.vert` also transforms the tangent by the inverse-transpose normal matrix, correct only under uniform scale.
