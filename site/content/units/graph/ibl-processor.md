---
module: graph
id: ibl-processor
title: IBL processor
standard: v2
---

## A complete bake with no call site

`IBLProcessor` is a self-contained split-sum preprocessor: an equirectangular HDR
file in, and out come the four resources the Karis/UE4 image-based-lighting
approximation wants — an environment cubemap, a diffuse irradiance cubemap, a
roughness-mipped prefiltered specular cubemap, and a BRDF integration LUT. It is
compiled into `ohao_renderer` by that library's recursive source glob. Nothing in
the tree ever constructs it, and the type is never named outside its own two
files. Its header is included exactly once, by an umbrella header that is itself
included by nothing:

{{cite ohao/render/render_module.hpp "render/ibl/ibl_processor.hpp"}}

`loadEnvironmentMap`, `generateBRDFLUT` and all four view getters have zero
callers in `ohao/`, `examples/` or `tests/`. Before calling that dead, check the
usual trap — is the same math inlined somewhere live? Partly: the split-sum
*evaluation* ships, against different inputs. `DeferredLightingPass` carries
descriptor slots for the irradiance cubemap, the prefiltered cubemap and the LUT,
and the only path that can fill them — `DeferredRenderer::setIBLTextures`
forwarding to the pass's own `setIBLTextures` — has no callers, so all three
resolve to the pass's 1×1 fallback image:

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "imageInfos[8].imageView = m_brdfLUTView != VK_NULL_HANDLE ? m_brdfLUTView : fallbackView;"}}

`deferred_lighting.frag` knows this and shades around it: it approximates the
prefiltered lobe by sampling the raw equirect `sampler2D` at `lod = roughness *
9.0`, asks for irradiance as a single tap at `lod = 9.0`, and when the LUT comes
back all-zero substitutes an analytic scale/bias rather than turning metals black:

{{cite shaders/core/deferred_lighting.frag "brdf = vec2(max(1.0 - roughness, 0.04), roughness * 0.25);"}}

Both LODs are inert. The only image ever handed to that pass is the RT
environment texture, and it is created with a single mip level:

{{cite ohao/gpu/vulkan/light_upload.cpp "imgInfo.mipLevels = 1;"}}
{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setEnvMap(m_envMapImageView, m_rtTextureSampler);"}}

So both `textureLod` calls clamp to level 0. Ambient specular is a
full-resolution mirror tap at every roughness, and the ambient diffuse term is an
unfiltered point sample of the sky in direction **N** — not the 512×-coarser
average `lod = 9.0` asks for. The shader names the assumption it is running on,
and the engine does not meet it:

{{cite shaders/core/deferred_lighting.frag "float lod = roughness * 9.0; // assuming 10 mip levels for 1024x512"}}
{{cite shaders/core/deferred_lighting.frag "vec3 irradiance = textureLod(envMap, nUV, 9.0).rgb;"}}

The matching cubemap helper — `calculateIBL` over `samplerCube irradianceMap` +
`samplerCube prefilteredMap` + the LUT — exists in full, and no shader
`#include`s the file it lives in:

{{cite shaders/includes/lighting/ibl.glsl "vec3 calculateIBL("}}

:::why
The deferred pipeline could have made the cubemap bake a prerequisite for IBL. It
chose the opposite — approximate specular IBL from the equirect 2D map with a
roughness→LOD proxy, and substitute an analytic scale/bias when the LUT is
missing — so a scene lit by `setEnvMap` alone still gets ambient specular instead
of black metal. What it does not get is any filtering at all. With one mip on the
env image the roughness→LOD proxy collapses to a mirror tap and the irradiance
tap collapses to one texel, so the two convolutions the bake exists to buy — a
GGX-weighted lobe and a cosine-weighted hemisphere — are both simply absent, not
approximated.
:::

## Four resources, and one load-bearing detail

All three cubemaps are `VK_FORMAT_R16G16B16A16_SFLOAT` with
`STORAGE_BIT | SAMPLED_BIT`, so the compute passes write them as `imageCube`
instead of round-tripping through a render pass. Every dimension is a
compile-time constant in the header — environment at 512², irradiance at 32²,
prefiltered at 128² over five mips:

{{cite ohao/render/ibl/ibl_processor.hpp "static constexpr uint32_t ENV_CUBEMAP_SIZE = 512;"}}
{{cite ohao/render/ibl/ibl_processor.hpp "static constexpr uint32_t IRRADIANCE_SIZE = 32;"}}
{{cite ohao/render/ibl/ibl_processor.hpp "static constexpr uint32_t PREFILTER_SIZE = 128;"}}
{{cite ohao/render/ibl/ibl_processor.hpp "static constexpr uint32_t PREFILTER_MIP_LEVELS = 5;"}}

The LUT is `VK_FORMAT_R16G16_SFLOAT` — R = scale, G = bias for
`F0 * scale + bias`. The prefiltered cubemap also gets five single-mip
`VIEW_TYPE_CUBE` views, the correct way to bind one mip as a storage image, and
the reason `imageSize()` in the shader returns that mip's true dimensions.

The HDR upload holds one thing that looks like a leftover and is not — the loader
flips the image vertically:

{{cite ohao/render/ibl/ibl_processor.cpp "stbi_set_flip_vertically_on_load(true);"}}

and the cubemap shader maps elevation to V with `theta / PI + 0.5`, so V = 0 means
θ = −π/2, straight down:

{{cite shaders/compute/equirect_to_cubemap.comp "vec2 uv = vec2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);"}}

Row 0 of the buffer becomes V = 0, and an unflipped equirect has its *sky* in row
0. Drop the flip and the world inverts; the two lines are a pair.

## Why roughness = 1 really is the irradiance integral

The irradiance cubemap has no shader of its own. It reuses the specular prefilter
pipeline with roughness pushed to 1.0:

{{cite ohao/render/ibl/ibl_processor.cpp "m_irradiancePipeline = m_prefilterPipeline; // Same pipeline, different params"}}
{{cite ohao/render/ibl/ibl_processor.cpp "} pc = {1.0f, 0, IRRADIANCE_SIZE, 0};"}}

This reads like a shortcut trading correctness for a pipeline object. It is not —
it is exact in the limit. The prefilter is the standard UE4 estimator: sample GGX
half-vectors $\mathbf{h}_k$ about $\mathbf{n}$, reflect the view (which the shader
sets to $\mathbf{n}$) to get $\mathbf{l}_k$, and form a cosine-weighted normalized
average of radiance:

{{cite shaders/compute/prefilter_envmap.comp "prefilteredColor = prefilteredColor / totalWeight;"}}

$$L_{\text{pre}}(\mathbf{n}) = \frac{\sum_k L(\mathbf{l}_k)\,(\mathbf{n}\cdot\mathbf{l}_k)}{\sum_k (\mathbf{n}\cdot\mathbf{l}_k)}$$

Here $L$ is environment radiance and $\mathbf{n}$ is the texel's cubemap
direction. The half-vector draw is
$\cos\theta_h = \sqrt{(1-\xi)/(1+(a^2-1)\xi)}$, where $\xi$ is the second
Hammersley coordinate and $a$ is the shader's own local — which is
`roughness * roughness`, not the roughness, and is what gets squared in that
denominator:

{{cite shaders/compute/prefilter_envmap.comp "float a = roughness * roughness;"}}

At `roughness = 1` that $a$ is 1, so the expression collapses to $\sqrt{1-\xi}$ —
i.e. $\cos^2\theta_h$ uniform, which is cosine-weighted hemisphere sampling of
$\mathbf{h}$. Because $\mathbf{v} = \mathbf{n}$, reflection doubles the polar
angle, $\theta_l = 2\theta_h$. Pushing the density through that map turns a
per-angle density $\propto \cos\theta_h \sin\theta_h$ into one
$\propto \sin\theta_l$ — which is exactly *uniform in solid angle*. With uniform
samples and weights $\mathbf{n}\cdot\mathbf{l}$, the normalized sum converges to

$$\frac{\int_\Omega L(\mathbf{l})(\mathbf{n}\cdot\mathbf{l})\,d\omega}{\int_\Omega (\mathbf{n}\cdot\mathbf{l})\,d\omega} = \frac{E(\mathbf{n})}{\pi}$$

with $E(\mathbf{n})$ the irradiance — precisely what a Lambertian IBL term wants
stored, since `ibl.glsl` computes diffuse as `irradiance * albedo` with no
$1/\pi$. The trick is sound.

Its cost is not noise, though. `hammersley()` is a fixed Van der Corput sequence
with no per-texel decorrelation, so every texel draws the same $\xi$ pattern in
its own tangent frame:

{{cite shaders/compute/prefilter_envmap.comp "return vec2(float(i) / float(N), radicalInverse_VdC(i));"}}

and the estimator normalizes by a second finite sum, $\sum_k(\mathbf{n}\cdot
\mathbf{l}_k)$, so it is a ratio estimator: biased at finite $N$, with a residual
that is deterministic structured error keyed to that tangent frame rather than
zero-mean variance more samples would average away. The reflection map also
halves the budget: $\theta_l = 2\theta_h$ sends every sample with
$\theta_h > \pi/4$ below the horizon, and cosine-weighted sampling puts exactly
half its mass there, so the `NdotL > 0` test discards half of every texel's taps.
With the sample count the next section uncovers, each irradiance texel survives
on roughly sixteen.

## The push constant that renames a field

The C++ side pushes four words — roughness, mip index, face size, pad:

{{cite ohao/render/ibl/ibl_processor.cpp "} pc = {roughness, mip, mipSize, 0};"}}

The shader declares three, and its third is not `faceSize`:

{{cite shaders/compute/prefilter_envmap.comp "uint sampleCount;"}}

Both blocks put a `float` at offset 0 and `uint`s at 4 and 8, so the write lands
cleanly — into the wrong meaning. The shader's importance-sample loop count
becomes whatever face size the host pushed:

{{cite ohao/render/ibl/ibl_processor.cpp "uint32_t mipSize = PREFILTER_SIZE >> mip;"}}

With `PREFILTER_SIZE` at 128 that is 128 samples for the mirror-smooth mip 0,
then 64, 32, 16, and 8 for roughness 1.0. The budget shrinks exactly as the lobe
widens, which is backwards — a near-mirror lobe converges in a handful of
samples, a hemispherical one needs hundreds. The irradiance pass pushes
`IRRADIANCE_SIZE`, integrating the hemisphere with 32.

## Five dispatches, one mip

The prefilter loop is the sharpest defect. Each iteration rewrites a *single*
descriptor set to point at that mip's view, then binds and dispatches:

{{cite ohao/render/ibl/ibl_processor.cpp "updateDescriptorSet(m_prefilterDescSet, m_envCubemapView, m_cubemapSampler, m_prefilteredMipViews[mip]);"}}

`vkUpdateDescriptorSets` takes effect on the host immediately; the command buffer
is not submitted until after the loop. All five recorded `vkCmdBindDescriptorSets`
calls therefore name a set whose contents, at execution time, are the last write —
mip 4's view. Updating a set already bound into a recording command buffer is
invalid usage anyway, so the outcome is formally undefined; the straightforward
one is that every dispatch targets the 8×8 smallest mip and levels 0–3 are never
written. Push constants, recorded into the buffer, *do* vary per dispatch, so mip
4 ends up holding the roughness-1.0 result.

The pool remembers the design that would have worked: sized for twenty sets, with
a comment naming why, while `createDescriptors` allocates four.

{{cite ohao/render/ibl/ibl_processor.cpp "descriptorCount = 20;  // More for prefilter mip levels"}}

The dispatch grid is wrong too, by a factor of two on each axis. Each of the
three compute shaders declares a 16×16 local size — here is the prefilter's —
while every `vkCmdDispatch` in the file rounds the work up for an 8×8 group:

{{cite shaders/compute/prefilter_envmap.comp "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;"}}
{{cite ohao/render/ibl/ibl_processor.cpp "vkCmdDispatch(cmd, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);"}}

That is four invocations per texel, three of which do nothing. It is the one
mismatch in this file that costs nothing but time, and only because every shader
early-outs against `imageSize()` before it writes:

{{cite shaders/compute/prefilter_envmap.comp "if (coord.x >= size.x || coord.y >= size.y || coord.z >= 6) {"}}

## What one validation-layer run would have caught

The BRDF LUT shader declares its output storage image at **binding 0**:

{{cite shaders/compute/brdf_lut.comp "layout(set = 0, binding = 0, rg16f) writeonly uniform image2D brdfLUT;"}}

The shared descriptor layout puts a `COMBINED_IMAGE_SAMPLER` at binding 0 and the
storage image at binding 1, and `executeBRDFIntegration` writes the LUT view to
binding 1 — the one the shader does not declare:

{{cite ohao/render/ibl/ibl_processor.cpp "write.dstBinding = 1;  // Output binding"}}

Binding 0 is never written at all: the LUT pass cannot produce a LUT.

The equirect upload has an ordering inversion. `transitionImageLayout` allocates,
submits and `vkQueueWaitIdle`s its *own* command buffer, so both of
`createEquirectTexture`'s transitions complete before the copy recorded into the
outer buffer is ever submitted:

{{cite ohao/render/ibl/ibl_processor.cpp "vkCmdCopyBufferToImage(cmd, stagingBuffer, m_equirectImage,"}}

By the time the copy runs the image sits in `SHADER_READ_ONLY_OPTIMAL` while the
copy declares `TRANSFER_DST_OPTIMAL` — a layout mismatch that leaves the texels
undefined, at the cost of three full queue stalls per HDR load.

And because the irradiance pipeline is an alias of the prefilter pipeline,
`cleanup()` destroys the same `VkPipeline` handle twice:

{{cite ohao/render/ibl/ibl_processor.cpp "if (m_irradiancePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_irradiancePipeline, nullptr);"}}

:::key
Every defect here is the same shape: a contract stated twice, once in C++ and
once in GLSL, where the two statements agree closely enough to compile and
disagree in meaning — a push-constant field, a descriptor binding number, a
workgroup size, a command-buffer ordering. None prevents compilation or pipeline
creation, and none has ever been checked, because the class has no call site.
Wiring it up is not the last step; it is the first debugging session.
:::

## Contracts

- The environment cubemap is created with **one** mip level, so the prefilter's filtered-importance-sampling step (`textureLod` at a variance-derived LOD) always clamps to level 0. Giving the env cubemap a mip chain is a prerequisite for that line to do anything. {{cite ohao/render/ibl/ibl_processor.cpp "ENV_CUBEMAP_SIZE, 1, hdrFormat, cubemapUsage)"}}
- That same heuristic mis-parenthesizes and then squares the GGX density, so even with mips present the LOD would be wrong. It affects noise only, never energy: the output weight is `NdotL / Σ NdotL` regardless. {{cite shaders/compute/prefilter_envmap.comp "float mipLevel = params.roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);"}}
- `initialize()` must run before `generateBRDFLUT()`: the BRDF descriptor set is allocated in `createDescriptors()`, and `generateBRDFLUT` neither checks for it nor creates it.
- `cleanup()` nulls almost nothing it destroys: only the `destroyImage` lambda clears its handles. The pipelines, layout, pool and samplers are left dangling, and so are the five per-mip image views — the loop that destroys them neither nulls the elements nor clears the vector. The destructor calls `cleanup()`, so calling it explicitly and then letting the object die double-frees every one of those. {{cite ohao/render/ibl/ibl_processor.cpp "for (auto view : m_prefilteredMipViews) {"}}
