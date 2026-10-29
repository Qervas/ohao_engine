---
module: denoise
id: oidn
title: OIDN
standard: v2
---

## The buffer OIDN is actually handed

A denoiser is only as good as the estimator underneath it, so the first question
is which image leaves the GPU. It is not the tonemapped RGBA8 frame the viewer
blits; it is the path tracer's accumulation image (binding 1, `RGBA32F`), copied
back through a staging buffer along with the albedo and normal AOVs.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readImageRGBA(rtRenderer->getAccumImage(), false, VK_IMAGE_LAYOUT_GENERAL, kRT, beauty);"}}

Which shader wrote that image is the profile's choice. The only code that
constructs a `PathTracer` is `RTProfileRendererBase`, and its `init()` hands the
tracer the profile's shader set: the realtime profile names the GLSL raygen, the
offline profile takes `PathTracerShaderSet`'s default, the Slang reference path
tracer.

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};"}}

So the shader that produces OIDN's input is `path_tracer.slang`'s `raygen`. What
it writes is not a radiance *sum*: `.rgb` holds a running mean and `.w` the sample
count, one dispatch per sample, folded in as a plain progressive average over the
pixel's own history.

{{cite shaders/rt/path_tracer.slang "acc = (prev.rgb * prev.w + radiance) / count;"}}
{{cite shaders/rt/path_tracer.slang "accumBuffer[pixel] = float4(acc, count);"}}

The history is this pixel's alone, read from the previous frame's image of a
ping-pong pair: the offline camera is still, and any view change resets the
accumulation.

{{cite shaders/rt/path_tracer.slang "// The running mean of every sample since the last reset (the offline camera is still)."}}

So OIDN receives the Monte-Carlo mean at whatever spp has accrued, in linear HDR,
pre-tonemap — which is precisely the input its `RT` filter is trained on. The
alpha channel is dropped on the way: `rgba32fToFloat3` copies three of four
floats per pixel, discarding the sample count and the 16-byte pixel stride.

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "rgb[i * 3 + 0] = rgba[i * 4 + 0];"}}

The repacked beauty vector is then bound as both `color` and `output`, so the
filter denoises in place and no fourth allocation appears:

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "width, height);  // in-place"}}

## Why this backend is the offline one

Every call builds the whole OIDN stack from scratch: a device and its `commit()`,
then an `RT` filter and its own `commit()`. All of it is function-local, so no
device, engine or filter state survives to the next invocation.

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "oidn::DeviceRef device = oidn::newDevice(oidn::DeviceType::Default);"}}
{{cite ohao/render/rt/denoise/oidn_denoise.cpp "filter.commit();"}}

Add the three full-resolution readbacks — each
allocating its own staging buffer, submitting a command buffer and blocking on
`vkQueueWaitIdle`, preceded by a `vkDeviceWaitIdle` — and the per-invocation cost
is structural, not tunable. It runs only for a frame someone asked for (a
`requestReadback()` before its `render()`), inside that frame's `readback()`, and a
one-shot cache bounds it to once per frame: `render()` invalidates it, the first
`readback()` after that pays for the whole chain, and repeat calls are free.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseCacheValid) return true;"}}

:::why
OIDN can run on a CUDA/SYCL/HIP device and import Vulkan allocations as external
memory, which would keep the whole thing on-GPU. OHAO does not do that — and the
`DeviceType::Default` request is not what stops it: `Default` asks OIDN for a
device, not for the CPU. What forces the PCIe round trip is the API level the
call site uses. `oidnDenoise` takes host spans and hands `setImage` the plain
pointers inside them, never external-memory-backed `oidn::BufferRef`s, so the
pixels must be resident on the CPU before `execute()` runs, whatever device it
runs on.

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "bool oidnDenoise(std::span<float> beauty, std::span<const float> albedo,"}}

The rejected alternative buys frame time the offline profile does not need, at
the cost of a second device/allocator lifetime to keep in sync with the Vulkan
one — and the realtime slots are already filled by NRD, à-trous and DLSS-RR, all
of which stay GPU-side. OIDN is the reference backend; latency is explicitly not
its job.
:::

The dispatch reflects that. `readback()` returns the requested frame's staging
copy for `None`, `Atrous` and `DLSSRR`, whose result is already in the output
image the frame copied (the last two filter on the GPU), and hands only `OIDN` and
`NRD` to the denoising path, by name.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (isRTRenderMode(m_renderMode) && (m_denoiseMode == DenoiseMode::OIDN || m_denoiseMode == DenoiseMode::NRD) &&"}}

`--denoise=optix`, from the era when OptiX was tried first and OIDN was the
fallback, now parses straight to OIDN with a warning.

{{cite ohao/render/rt/denoise/denoise_types.cpp "--denoise=optix is no longer supported"}}

A `DenoiseMode` added later reads back its frame's staging copy unless it is named
there.

## The guides, and the one that is encoded wrong

Albedo and normal guides let the filter preserve texture and geometric edges it
would otherwise smear. OHAO always supplies both — the header supports empty
spans to skip a guide, but the single in-engine caller passes all three buffers,
and auxiliary AOVs default to on.

{{cite ohao/render/rt/rt_settings.hpp "bool enableAuxiliaryAOVs{true};"}}

Two things about those guides do not match what OIDN documents. First, the normal
AOV is stored in a half-range encoding unless DLSS-RR is the denoiser:

{{cite shaders/rt/path_tracer.slang "normalAOV[pixel] = (flags & kFlagDLSSRR) != 0u ? float4(n, hit.specularRoughness) : float4(n * 0.5 + 0.5, 1.0);"}}

and it is passed to the filter with no decode step anywhere between the readback
and `setImage`:

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "const_cast<float*>(normal.data()), oidn::Format::Float3"}}

The OIDN manual is unusually explicit here: the `normal` image must hold
world- or view-space vectors in $[-1, 1]$, and normals mapped to $[0, 1]$ are
*not* acceptable and must be remapped. What the filter sees is therefore an
all-positive, half-scale vector field — still edge-correlated, so it is not
useless, but not the signal the network was trained on. The fix is one decode,
either in the raygen's AOV write or between readback and `setImage`.

The same line writes true $[-1,1]$ normals — with specular roughness in `.w` —
behind the DLSS-RR flag. The encoding is therefore selected by `DenoiseMode`, not
by render profile: `RTRenderProfile` has exactly two values, `Realtime` and
`Offline`, and DLSS-RR is not one of them. OIDN is simply on the wrong side of that
gate.

Second, the guides are not accumulated. Bindings 6 and 7 are overwritten by every
sample, while the camera jitters within the pixel each sample, so the guide that
survives to readback is the *last* sample's first hit rather than the pixel's
average — OIDN's manual asks for anti-aliased, accumulated features. The albedo
guide at a surface is the OpenPBR base's directional albedo toward the viewer,
specular included:

{{cite shaders/rt/path_tracer.slang "const float3 a = base.albedo(wo, tables);"}}

On a primary miss, or a hit on a light's emitter mesh, it is filled with what the
camera ray brought back — environment or emitter radiance, which for an HDRI or a
light routinely exceeds the $[0,1]$ range the albedo guide is specified over.

{{cite shaders/rt/path_tracer.slang "albedoAOV[pixel] = float4(add, 1.0);"}}

Neither defect is compensated for. `cleanAux` — the hint that would tell OIDN
these guides are already clean — appears nowhere in the tree: it is not left at
its default as a decision, it is simply never considered.

## Denoise in linear, tonemap after

Only after filtering does the HDR result become 8-bit. In the offline profile the
CPU-side tonemap is a deliberate mirror of the one in the raygen, so that
toggling `--denoise=oidn` changes the noise and nothing else. Both apply the
Narkowicz ACES fit

$$\text{ACES}(x) = \operatorname{clamp}\!\left(\frac{x\,(a x + b)}{x\,(c x + d) + e},\, 0,\, 1\right),\quad (a,b,c,d,e) = (2.51,\,0.03,\,2.43,\,0.59,\,0.14)$$

to exposure-scaled linear radiance $x = 0.5\,L$, then encode with $\gamma = 2.2$.
The coefficients live once in C++ and once in the Slang tracer:

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;"}}
{{cite shaders/rt/path_tracer.slang "outputImage[pixel] = float4(pow(aces(acc * 0.5), float3(1.0 / 2.2)), 1.0);"}}

and the exposure the denoised path passes is the same 0.5 the shader hard-codes.

{{cite ohao/gpu/vulkan/renderer.cpp "m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);"}}

The mirror only holds where the GPU does nothing but tonemap, which is a property
of the profile rather than of the tonemap. `kRealtimeRTSettings` turns the internal
spatial denoise on — an edge-aware filter and luminance rescale, a compute pass
after the trace — so the realtime GPU frame is tonemapped *after* filtering while
the OIDN path tonemaps the raw accumulation — same ACES fit, different input.

{{cite ohao/render/rt/rt_settings.hpp ".enableInternalDenoise = true,"}}
{{cite shaders/rt/pt_spatial_denoise.comp "const vec3 ldr = pow(ACES(denoised * 0.5), vec3(1.0 / 2.2));"}}

That pairing is reachable: the shared example parser takes `rt_realtime` and
`--denoise=` as independent tokens, the override is applied on top of the preset,
and nothing rejects it — `allowExternalDenoiser`, `false` for realtime, is only
ever printed in a startup log. `rt_realtime --denoise=oidn` moves the grade, not
just the noise.

{{cite examples/example_cli.hpp "opts.denoiseOverride = parseDenoiseMode(arg.substr(10));"}}
{{cite ohao/gpu/vulkan/renderer.cpp "externalDenoiser="}}

The two tonemaps are not bit-identical either. The GPU store into an `rgba8`
UNORM image rounds to nearest; the CPU path truncates.

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "rgba8[i * 4 + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));"}}

That is a uniform sub-LSB downward bias — invisible by eye, but enough to move a
per-pixel diff against a `--denoise=none` reference, which matters if you are
using such diffs as a regression signal.

## Contracts

- `oidnDenoise` reads the accumulation image, whose `.rgb` is already a running mean. Anything that changes that to a raw sum breaks the denoise path silently: OIDN's HDR filter would see radiance scaled by the sample count.
- The frame to denoise is requested (`requestReadback()` before its `render()`) and read with `readback()` before the next `render()`, which clears the denoise cache. The readback re-submits on the graphics queue with a device-wide wait.
- The normal guide is `N * 0.5 + 0.5`, not the `[-1,1]` OIDN specifies. Adding a decode is a quality fix that no test will notice: the only committed regression corpus, `tests/golden/manifest.json`, names OIDN for none of its scenes, so no golden image in the tree has ever passed through OIDN.
- Three tonemaps must move together, or `--denoise=none` and `--denoise=oidn` stop being comparable in grade: `float3ToRGBA8` in C++, plus the one in each raygen, `path_tracer.slang`'s `aces` and `pt_raygen_realtime.rgen`'s `ACES`.
- The build takes whatever OIDN the host provides — `find_package(OpenImageDenoise REQUIRED)` pins no version — so a host-side OIDN upgrade changes the filter under unchanged source.
- The LDR path (`hdr = false`) has no in-engine caller; every call site passes `hdr = true`.
