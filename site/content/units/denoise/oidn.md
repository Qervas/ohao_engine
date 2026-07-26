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

{{cite ohao/gpu/vulkan/render_dispatch.cpp "bool ok = readbackImage(rtRenderer->getAccumImage(), beauty);"}}

Which raygen wrote that image is not readable off `PathTracer`, whose default
shader set names `rt_pt_raygen.rgen.spv`. Nothing ever runs it: the only code
that constructs a `PathTracer` is `RTProfileRendererBase`, and its `init()`
replaces the shader set unconditionally — with the realtime or the offline
raygen, the only two profiles the renderer hands out. `pt_raygen.rgen` is
compiled by the shader directory's `GLOB_RECURSE` and never loaded.

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}

So the shader that produces OIDN's input is `pt_raygen_offline.rgen`. What it
writes is not a radiance *sum*: `.rgb` holds a running mean and `.w` the sample
count, one dispatch per sample, folded in as a plain progressive average over the
pixel's own history.

{{cite shaders/rt/pt_raygen_offline.rgen "acc = (prev.rgb * prev.w + radiance) / count;"}}
{{cite shaders/rt/pt_raygen_offline.rgen "imageStore(accumBuffer, pixel, vec4(acc, count));"}}

The reprojected-history branch sitting beside it is dead in this shader — it is
forced off, because reading a neighbour pixel that sibling invocations are
concurrently writing is a race with no defined ordering, and the offline camera
is static anyway.

{{cite shaders/rt/pt_raygen_offline.rgen "// own-pixel progressive-accumulation branch."}}

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
is structural, not tunable. The only thing that keeps it usable in the GLFW
viewer is a one-shot cache: `render()` invalidates it, the first `getPixels()`
after that pays for the whole chain, and repeat calls in the same frame are free.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseCacheValid) {"}}

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

The dispatch reflects that: `getPixels()` early-returns for `None`, `Atrous` and
`DLSSRR` — all three already have their result in `m_pixelBuffer`, the last two
because they filter on the GPU into the output image — and `NRD` takes its own
GPU readback branch. OIDN is never tested for; it is the fall-through.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseMode == DenoiseMode::None || m_denoiseMode == DenoiseMode::Atrous ||"}}

The `bool denoised = false;` guard immediately above the call is a fossil of the
era when OptiX was tried first and OIDN was the fallback; `--denoise=optix` now
parses straight to OIDN with a warning.

{{cite ohao/render/rt/denoise/denoise_types.cpp "--denoise=optix is no longer supported"}}

The practical consequence: any `DenoiseMode` added later without an entry in that
early-return will silently be routed through OIDN's readback path.

## The guides, and the one that is encoded wrong

Albedo and normal guides let the filter preserve texture and geometric edges it
would otherwise smear. OHAO always supplies both — the header supports empty
spans to skip a guide, but the single in-engine caller passes all three buffers,
and auxiliary AOVs default to on.

{{cite ohao/render/rt/rt_settings.hpp "bool enableAuxiliaryAOVs{true};"}}

Two things about those guides do not match what OIDN documents. First, the normal
AOV is stored in a half-range encoding:

{{cite shaders/rt/pt_raygen_offline.rgen "imageStore(normalAOV, pixel, vec4(N * 0.5 + 0.5, 1.0));"}}

and it is passed to the filter with no decode step anywhere between the readback
and `setImage`:

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "const_cast<float*>(normal.data()), oidn::Format::Float3"}}

The OIDN manual is unusually explicit here: the `normal` image must hold
world- or view-space vectors in $[-1, 1]$, and normals mapped to $[0, 1]$ are
*not* acceptable and must be remapped. What the filter sees is therefore an
all-positive, half-scale vector field — still edge-correlated, so it is not
useless, but not the signal the network was trained on. The fix is one decode,
either in the raygen's AOV write or between readback and `setImage`.

The same raygen already writes true $[-1,1]$ normals into binding 7 — with
perceptual roughness in `.w` — but only behind the `PT_FLAG_DLSSRR` bit. The
encoding is therefore selected by `DenoiseMode`, not by render profile:
`RTRenderProfile` has exactly two values, `Realtime` and `Offline`, and DLSS-RR
is not one of them. OIDN is simply on the wrong side of that gate.

{{cite shaders/rt/pt_raygen_offline.rgen "imageStore(normalAOV, pixel, vec4(N, roughness));"}}

Second, the guides are not accumulated. Bindings 6 and 7 are overwritten by every
sample, while the camera jitters within the pixel each sample, so the guide that
survives to readback is the *last* sample's first hit rather than the pixel's
average — OIDN's manual asks for anti-aliased, accumulated features. On a primary
miss the albedo AOV is filled with environment radiance, which for an HDRI
routinely exceeds the $[0,1]$ range the albedo guide is specified over.

{{cite shaders/rt/pt_raygen_offline.rgen "imageStore(albedoAOV, pixel, vec4(payload.color, 1.0));"}}

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
The coefficients live once in C++ and once in GLSL:

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;"}}
{{cite shaders/rt/pt_raygen_offline.rgen "vec3 ldr = ACES(denoised * 0.5);"}}

and the exposure the denoised path passes is the same 0.5 the shader hard-codes.

{{cite ohao/gpu/vulkan/renderer.cpp "m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);"}}

The mirror only holds where the GPU does nothing but tonemap, which is a property
of the profile rather than of the tonemap. `kRealtimeRTSettings` turns the
raygen's own à-trous filter and luminance rescale on, so the realtime GPU frame
is tonemapped *after* filtering while the OIDN path tonemaps the raw
accumulation — same ACES fit, different input.

{{cite ohao/render/rt/rt_settings.hpp ".enableInternalDenoise = true,"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 ldr = ACES(denoised * 0.5);"}}

That pairing is reachable: the GLFW viewer defaults to `RTRealtime` and applies
`--denoise=` on top of the preset, and nothing rejects it — `allowExternalDenoiser`,
`false` for realtime, is only ever printed in a startup log. Toggling the flag
there moves the grade, not just the noise.

{{cite examples/interactive.cpp "cli.rtMode = RenderMode::RTRealtime;"}}
{{cite ohao/gpu/vulkan/renderer.cpp "externalDenoiser="}}

The two tonemaps are not bit-identical either. The GPU store into an `rgba8`
UNORM image rounds to nearest; the CPU path truncates.

{{cite ohao/render/rt/denoise/oidn_denoise.cpp "rgba8[i * 4 + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));"}}

That is a uniform sub-LSB downward bias — invisible by eye, but enough to move a
per-pixel diff against a `--denoise=none` reference, which matters if you are
using such diffs as a regression signal.

## Where OIDN is deliberately forbidden

The inverse-rendering module keeps the denoiser out of the optimizer. Every
render whose pixels feed the image loss — and therefore every finite-difference
gradient taken of that loss — is rendered with `DenoiseMode::None`, in the
synthetic and the lab-capture branch alike.

{{cite ohao/inverse/staged_fit.hpp "(!cfg.labBundle.empty()) ? DenoiseMode::None : DenoiseMode::None;"}}

Presentation renders are a separate slot, and there the default is *on*:
`showDenoise` starts at `DenoiseMode::OIDN`, and the CLI accepts only `none` or
`oidn` for it.

{{cite ohao/inverse/fit_config.hpp "DenoiseMode showDenoise{DenoiseMode::OIDN};"}}
{{cite ohao/inverse/fit_config.hpp "a.cfg.showDenoise != DenoiseMode::None && a.cfg.showDenoise != DenoiseMode::OIDN"}}

That default reaches past presentation. The SHOW RMSE, PSNR and SSIM a run prints
as its headline result are computed on the recovered image rendered with
`showDenoise`, against a synthetic target rendered through `showDenoise` too — so
by default both sides of the comparison have been through the prior.

{{cite ohao/inverse/fit_engine.hpp "labM.showRmse = rmseRGB(recoveredPrimary, tb.targetsShow[0]);"}}
{{cite ohao/inverse/fit_targets.hpp "session.render(v, cfg.show, cfg.seed, cfg.showDenoise);"}}

Real-photo lab mode is the one configuration that forces the presentation
denoiser off, and its holdout and relight evaluations render with `None` as well.

{{cite ohao/inverse/fit_targets.hpp "cfg.showDenoise = DenoiseMode::None;"}}
{{cite ohao/inverse/fit_lab_eval.hpp "constexpr DenoiseMode kLabEvalDenoise = DenoiseMode::None;"}}

:::key
A neural denoiser is a strong, non-linear, spatially-correlated prior. It makes
a converged render look converged, and it makes an unconverged render *also* look
converged — so any measurement taken through it is measuring the prior as much as
the integrator. OHAO holds that line where a biased signal would corrupt the fit
itself — the loss and the finite-difference gradients — and does not hold it for
the reported image metrics, which outside lab mode are computed prior-on.
:::

## Contracts

- `oidnDenoise` reads the accumulation image, whose `.rgb` is already a running mean. Anything that changes that to a raw sum breaks the denoise path silently: OIDN's HDR filter would see radiance scaled by the sample count.
- `getPixels()` must run after `render()` and before the next one; `render()` clears the denoise cache and the readback re-submits on the graphics queue with a device-wide wait.
- The normal guide is `N * 0.5 + 0.5`, not the `[-1,1]` OIDN specifies. Adding a decode is a quality fix that no test will notice: the only committed regression corpus, `tests/golden/manifest.json`, renders both its scenes with `--denoise=none`, so no golden image in the tree has ever passed through OIDN.
- Three tonemaps must move together, or `--denoise=none` and `--denoise=oidn` stop being comparable in grade: `float3ToRGBA8` in C++, plus the one in each live raygen, `pt_raygen_offline.rgen` and `pt_raygen_realtime.rgen`. Editing `pt_raygen.rgen` changes nothing at runtime — no renderer binds it.
- The build takes whatever OIDN the host provides — `find_package(OpenImageDenoise REQUIRED)` pins no version — so a host-side OIDN upgrade changes the filter under unchanged source.
- The LDR path (`hdr = false`) has no in-engine caller; every call site passes `hdr = true`.
