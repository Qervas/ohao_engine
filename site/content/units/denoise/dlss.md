---
module: denoise
id: dlss
title: DLSS Ray Reconstruction
standard: v2
figures: [denoise-dlss-guides]
---

## A denoiser that rewrites the renderer

OIDN is a post-process with a shopping list: the finished frame plus the albedo and
normal AOVs the tracer already writes. Its one structural demand on the pipeline is
a CPU readback — it is the only mode `denoiseNeedsCpuReadback` names.

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr bool denoiseNeedsCpuReadback(DenoiseMode m) noexcept { return m == DenoiseMode::OIDN; }"}}

DLSS Ray Reconstruction is a different contract — a trained reconstruction network
that owns the temporal history, so it dictates what the path tracer must emit
before it produces anything at all. Passing `--denoise=dlssrr` stops the raygen
accumulating, adds a deterministic Halton sub-pixel offset to the primary ray,
repurposes one AOV binding into a different encoding, and moves tonemapping into a
compute pass. This page is about that blast radius, and about the two pieces of it
that are built but not wired.

## Two gates before a single ray is denoised

The wrapper keeps NGX out of every header — the feature handle and parameter block
are plain `void*` members only `dlss_rr.cpp` casts back — and the whole
implementation sits inside `#ifdef OHAO_DLSS_ENABLED` with a stub tail, so an
`-DOHAO_DLSS=OFF` build still links and `parseDenoiseMode("dlssrr")` degrades to
`None` with a warning.

Then two runtime gates. NGX initialises through the ProjectID form (no NVIDIA
app-id whitelist needed for a custom engine), and availability is *not* something
the driver volunteers — you query it, and when the answer is no, four further
parameters explain why, including a minimum driver version the code prints
verbatim rather than guessing.

{{cite ohao/render/rt/denoise/dlss_rr.cpp "params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &ssdAvailable);"}}

Feature creation is deferred to the first frame that actually runs in DLSSRR mode,
because NGX records real GPU setup work and the wrapper borrows the caller's
command buffer rather than opening its own. A one-shot latch means a failed init
costs one frame, not an NGX round-trip every frame forever.

## The guide buffer that is two guides

The interesting part of a DLSS-RR integration is not the call, it is the
demodulated guide set. `EvalInputs` names seven images, and six of them already
existed from earlier denoiser work, all at render resolution: the RGBA32F
accumulation buffer as `pInColor`, the RGBA8 diffuse-albedo (binding 24) and
specular-colour (binding 25) demodulation guides, the RGBA32F normal AOV, the linear
view-Z depth AOV, and the RG16F motion vectors. The seventh, `COLOR_OUT`, is new for
DLSS and is the one image *not* at render resolution — it is allocated at output
size, because that is what the network reconstructs into:

{{cite ohao/render/rt/path_tracer_images.cpp "// OUTPUT/display resolution: DLSS-RR upscales the render-res guide buffers"}}

Binding 25 is not F0, whatever `pInSpecularAlbedo` suggests. The raygen writes
the primary hit's directional albedo toward the viewer (OpenPBR's `albedo(wo)`,
from its energy tables) minus its diffuse albedo: the share of the surface's
reflectance the specular lobes carry at this view, Fresnel and roughness included.

{{cite shaders/rt/path_tracer.slang "specColor = max(a - diffAlbedo, float3(0.0));"}}

One of the six does double duty. DLSS-RR accepts normals and roughness as one image
if the feature is created in packed mode, which is what the create params ask for:

{{cite ohao/render/rt/denoise/dlss_rr.cpp "createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;"}}

and then the *same* `NVSDK_NGX_Resource_VK` is handed to both eval slots:

{{cite ohao/render/rt/denoise/dlss_rr.cpp "evalParams.pInRoughness = &normRough;"}}

No image is dedicated to that encoding — the R10G10B10A2 pack at binding 26 is
NRD's canonical encoder and its format is unsigned, while DLSS wants signed
world-space normals. So binding 7 — the RGBA32F normal AOV every other denoiser
reads as `N * 0.5 + 0.5` — holds the normal in `xyz` and the specular roughness in
`w` instead, chosen by the DLSSRR push-constant flag in the one store the raygen
makes to it, so other modes stay byte-identical:

{{cite shaders/rt/path_tracer.slang "normalAOV[pixel] = (flags & kFlagDLSSRR) != 0u ? float4(n, hit.specularRoughness) : float4(n * 0.5 + 0.5, 1.0);"}}

{{figure denoise-dlss-guides "Conceptual: the guide-to-parameter mapping as it exists in dlss_rr.cpp and path_tracer_render.cpp. Binding numbers and formats read from the code; not a captured render."}}

:::key
The two encodings share binding 7 and are told apart only by the DLSSRR flag. A
second store to it elsewhere in the raygen would decide by ordering which survives,
and a wrong one is never NaN: DLSS would silently receive half-space-encoded
normals and reconstruct badly.
:::

## Three conventions, and the one that only looks like one

DLSS and OHAO disagree about signs in two places and about matrix storage in a
third. The jitter offset must be given in render-resolution pixels and with the sign
flipped, which the wrapper does on the way in:

{{cite ohao/render/rt/denoise/dlss_rr.cpp "evalParams.InJitterOffsetX = -in.jitterX;"}}

Be exact about what is jittered: nothing here builds a jittered projection matrix.
The Halton offset is added to the primary ray's pixel coordinate in the raygen, and
the matrix handed to DLSS is `glm::inverse(pc.invProj)` — unjittered. The negation
is nvpro-samples' convention for a projection jitter, and applies unchanged to a
ray-coordinate jitter of the same sub-pixel size.

{{cite shaders/rt/path_tracer.slang "const float2 uv = (float2(pixel) + jit + pc.jitter.xy) / float2(size);"}}

`jit` is the path's own uniform point in the pixel (the anti-aliasing); `pc.jitter`
is the Halton offset, the same for every pixel of the frame.

Motion vectors are the second sign. They are written by the raygen as
`currPix - prevPix` where DLSS wants `prevPix - currPix`. Rather than rewrite the
raygen — NRD and the à-trous denoiser read the same image — the flip is folded into
the scale the caller passes:

{{cite ohao/render/rt/path_tracer_render.cpp "ei.mvScaleX = -1.0f; ei.mvScaleY = -1.0f;"}}

The third looks like a bug and is not. DLSS-RR documents its matrices as
row-major and left-multiplying; glm produces column-major, right-multiplying
matrices; the code passes glm's raw pointer through untouched. Let $A$ be the
world-to-view map applied as $v' = A v$ to a column vector, and $B$ the matrix
DLSS expects, applied as $v'^{\mathsf T} = v^{\mathsf T} B$ to a row vector. Those
agree only when

$$B = A^{\mathsf T}, \qquad \text{rowmajor}(B)_k = \text{colmajor}(A)_k \;\; \forall k$$

— because walking $B$ along rows *is* walking $A$ along columns. The convention
transpose and the storage transpose are the same transpose applied twice, so the
bytes are already correct and any "fix" inserting a `glm::transpose` here breaks
reprojection:

{{cite ohao/render/rt/denoise/dlss_rr.cpp "evalParams.pInWorldToViewMatrix = const_cast<float*>(in.worldToView);"}}

One create-time flag follows from the same care: OHAO's depth AOV is linear view-Z
in R32F, not a hardware depth buffer, so the feature declares it as such.

{{cite ohao/render/rt/denoise/dlss_rr.cpp "createParams.InUseHWDepth    = NVSDK_NGX_DLSS_Depth_Type_Linear;"}}

## Who owns the history

DLSSRR is one of the two modes `tracerAccumulates` refuses in both profiles. The
raygen's running mean is then given zero frames, so the accumulation buffer holds
this frame's sample alone rather than an average — the denoiser, not the tracer,
integrates across time:

{{cite ohao/render/rt/rt_meta.hpp "case DenoiseMode::DLSSRR: return false;"}}
{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.y = tracerAccumulates(m_renderSettings) ? m_accumFrames : 0u;"}}

Consequently `InReset` is raised only on a genuine first frame or an explicit
accumulation reset, never on camera movement. Resetting on movement is the
tempting alternative, and it is wrong for the same reason it is wrong for the
à-trous path: motion vectors exist so the network can reproject *through* camera
motion, and discarding history every moved frame yields raw 1-spp noise for as
long as the user keeps moving.

{{cite ohao/render/rt/path_tracer_render.cpp "ei.reset = (m_historyFrameCount == 0u);"}}

## The clamp in front of the network

Because DLSSRR keeps no running mean, the firefly clamp applies whenever the
profile enables it: the clamp is gated on the trace *not* accumulating, since a
running mean converges without it. The realtime profile enables it at luminance
10.0; the offline profile disables it, which is what the engine's no-bias-offline
rule demands.

{{cite ohao/render/rt/rt_settings.hpp ".enableFireflyClamp = true,"}}

The raygen scales the whole path's radiance down to the threshold, once, before
anything is stored:

{{cite shaders/rt/path_tracer.slang "if (lum > pc.tuning.x) radiance *= pc.tuning.x / lum;"}}

:::why
At one sample a frame a stochastic bright pixel lands somewhere different each
frame; a temporal reconstruction network reads that as motion and blinks. The
rejected alternative is to let the network absorb the spikes — one less bias, but
that blink is exactly what the clamp exists to remove.
:::

## Display space, and why the beauty image still works

DLSS writes HDR-linear RGBA16F, so something has to tonemap it.
`dlss_tonemap.comp` runs in 8×8 groups and applies the same `ACES(hdr * 0.5)` curve
plus 2.2 gamma the raygen uses in `None` mode, which is what keeps a DLSS frame
comparable to an undenoised one:

{{cite shaders/compute/dlss_tonemap.comp "vec3 ldr = ACES(hdr * 0.5);"}}

Everything else sits behind one `OHAO_GRADE` master in an 8-byte push constant; set
it to 0 and pure ACES remains. Most of it — contrast pivot, split tone, saturation,
vignette, animated grain — runs after the curve, in display space. The chromatic
aberration does not: it is a *source-fetch* offset on the HDR loads, before the
curve, splitting the R and B taps radially. That offset is truncated to whole texels,
so although at full strength the float peaks at
$|d| \cdot r^2 \cdot 6 = 0.5 \cdot 0.5 \cdot 6 = 1.5$ per axis in the corner, the
largest shift it can realise is one texel per axis:

{{cite shaders/compute/dlss_tonemap.comp "ivec2 off = ivec2(d * s * r2 * 6.0);"}}

The whole pass is downstream of the estimator, so none of it biases the render; the
firefly clamp above does, and the two should not be confused.

The pass writes back into `m_outputImage`, the same RGBA8 image the raygen would
have written, which is why the DLSS result reaches the screen through the ordinary
readback with no special case. What *is* special-cased is the readback barrier: its
source stage must be `COMPUTE` rather than `RAY_TRACING` when the tonemap ran, or
the copy races the dispatch.

## What is built but not connected

Two subsystems here read as working features and are not.

The specular hit-distance guide is the first. The wrapper forwards a
`pInSpecularHitDistance` whenever `EvalInputs` carries its view, and the tracer
allocates an R32F image for it at render resolution:

{{cite ohao/render/rt/path_tracer_images.cpp "if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specHitDistImage) != VK_SUCCESS) return false;"}}

Nothing writes it — it is in no descriptor set, and the raygen has no store for it
— and the one site that fills `EvalInputs` never assigns `specHitDistView`, so the
guide is always omitted and the glossy-reflection reprojection it would feed is
not in effect.

{{cite ohao/render/rt/denoise/dlss_rr.cpp "const bool haveSpecHitDist = (in.specHitDistView != VK_NULL_HANDLE);"}}

The upscaling presets are the second. `OHAO_DLSS_QUALITY` is parsed into a
`DlssQuality`, and the tracer really does shrink its internal trace resolution to an
even-dimensioned fraction of the output size:

{{cite ohao/render/rt/path_tracer.cpp "scale = dlssRenderScale(dlssQualityFromEnv());"}}

The sole `createFeature` call, however, passes the render dimensions as *both*
input and target and leaves `quality` at its `DLAA` default; `EvalInputs.outW/outH`
are likewise never set, so `evaluate` falls back to render dims for `COLOR_OUT`
and the tonemap dispatches a render-res grid:

{{cite ohao/render/rt/path_tracer_render.cpp "createFeature(cmd, m_width, m_height, m_width, m_height)"}}

DLAA — native-resolution pure denoise, and the default — is therefore the only
configuration that produces a full frame. An upscaling preset reduces the traced
pixel count as advertised but leaves DLSS reconstructing to the reduced size inside
output-sized images.

## Contracts

- Binding 7 carries two encodings, chosen by the DLSSRR flag in the raygen's one store to it. Any second store to binding 7 must respect the same flag.
- DLSSRR must stay in `tracerAccumulates`' refusing arms in both profiles; fed a running mean, the network would see a history that restarts on every moved view.
- `createFeature` records GPU work into the command buffer it is given; the caller must submit that buffer or the feature is never really created.
- Any render- or output-resolution change must release the feature and clear the init latch — an NGX feature is bound to the dimensions it was created with.
- The tonemap's SPIR-V is found by a relative search list (`build/shaders/…`, `bin/shaders/…`), so the binary must launch from a directory where one of those resolves.
- Only DLAA is presently a correct end-to-end configuration; `OHAO_DLSS_QUALITY` shrinks the trace without completing the upscale.
