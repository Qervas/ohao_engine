---
module: denoise
id: atrous
title: À-trous / SVGF-style
standard: v2
figures: [denoise-atrous-pingpong]
---

## Filtering the wrong image on purpose

Every other denoiser here wants linear radiance: NRD demodulated by albedo and
split diffuse/specular, OIDN as HDR colour with albedo and normal AOVs.
OHAO's own SVGF takes the opposite input — the RGBA8 beauty, already run through
ACES at exposure 0.5 and a 1/2.2 gamma curve by the path tracer's raygen:

{{cite shaders/rt/path_tracer.slang "outputImage[pixel] = float4(pow(aces(acc * 0.5), float3(1.0 / 2.2)), 1.0);"}}

It is also the last pass's destination — overwritten in place, so nothing
downstream changes.

{{cite ohao/render/rt/denoise/atrous_denoise.hpp "beautyImage  = VK_NULL_HANDLE; // m_outputImage (RGBA8)"}}

That input is this frame's sample alone. À-trous is one of the temporal
denoisers `tracerAccumulates` names, so the trace keeps no running mean of its
own under it, and the firefly clamp, which applies only when the trace does not
accumulate, is on by the realtime profile's defaults (the offline profile's is off):

{{cite ohao/render/rt/rt_meta.hpp "case DenoiseMode::Atrous:"}}

:::why
Demodulation — divide out albedo, filter the illumination, multiply back — is the
standard move, and it is what produced NRD's failure modes here: black metals and
magenta casts from a mismatched guide. Filtering the composited image cannot
produce either. The price: temporal average and spatial kernel both run on
gamma-encoded values, so the mean of N frames is not the gamma of the mean
radiance.
:::

## Pass 1: reprojection, and the two ways history dies

The temporal pass reads the motion-vector AOV — pixel-space `currPix - prevPix`,
so the previous location is `pixel - motion`, snapped to the nearest texel.
History must survive two per-pixel rejections: landing inside the image, and
matching the geometry history this pass wrote last frame — view Z plus normal:

{{cite shaders/rt/rt_svgf_temporal.comp "if (zrel > 0.1 || ndot < 0.9) valid = false;"}}

`zrel` normalises the depth difference by the larger of the two depths, so the
10% tolerance is scale-free; `ndot < 0.9` rejects beyond roughly 26° of rotation.
Survivors blend into an exponential moving average that decays as history
accumulates:

$$c_n = (1-\alpha)\,c_{n-1} + \alpha\,c,\qquad \alpha = \max\!\left(\tfrac{1}{n},\ 0.05\right)$$

where $c$ is this frame's beauty, $c_{n-1}$ the reprojected history, and $n$ the
per-pixel history length, clamped at 32. Without the floor the image stops
responding to lighting changes; 0.05 caps the effective window at 20 frames — and
since the floor binds at $n = 20$, the 32-frame clamp never changes $\alpha$.

{{cite shaders/rt/rt_svgf_temporal.comp "max(1.0 / newLen, 0.05)"}}

One storage detail is load-bearing. The depth AOV writes `1e30` for background,
but the geometry history is RGBA16F, whose largest finite value is 65504 — `1e30`
cannot round-trip. Clamping Z into fp16 range before storing is what makes
background compare cleanly against foreground at a silhouette:

{{cite shaders/rt/rt_svgf_temporal.comp "float curZc = min(curZ, 1.0e4);"}}

## The moments, and why the first four frames are different

The same EMA is applied to the first and second raw moments of luminance,
$m_1 = \mathbb{E}[l]$ and $m_2 = \mathbb{E}[l^2]$, giving a running variance:

$$\mathrm{Var} = m_2 - m_1^2$$

with $l = 0.2126R + 0.7152G + 0.0722B$. This is the point of Pass 1: the spatial
filter needs a per-pixel estimate of *how noisy this pixel still is*, and a
temporal moment pair gives one for free. With the firefly clamp on (the realtime
profile's default), $l$ is clamped luminance, so the moments measure the
clamped estimator's variance, not the unclamped Monte-Carlo variance.

A two-frame history gives a meaningless variance, so below four frames the pass
also computes the spatial 3×3 luminance variance of the current beauty and takes
the larger:

{{cite shaders/rt/rt_svgf_temporal.comp "if (newLen < 4.0) {"}}

Freshly disoccluded pixels are driven entirely by that spatial estimate.

## Pass 2: what the variance actually buys

The spatial pass is a 5×5 B3-spline à-trous wavelet — the same 25 taps every
iteration, spacing doubling 1→16. Tap radius stays 2 at every level, so five
dispatches reach a cascaded support radius of $2(1{+}2{+}4{+}8{+}16) = 62$ px for
125 taps; one gather over that 125×125 window would cost 15625. Each tap weight
carries three edge-stopping exponentials:

$$w = h_{ij}\;
e^{-\frac{|l_c - l_q|}{\sigma_l\sqrt{\overline{\mathrm{Var}}}+\varepsilon}}\;
e^{-\frac{\max(1-n_c\cdot n_q,\,0)}{\sigma_n^2+\varepsilon}}\;
e^{-\frac{|z_c - z_q|}{\sigma_z^2+\varepsilon}}$$

$h_{ij}$ is the separable B3-spline weight, $c$ and $q$ index the centre and the
tap, $z$ is linear view Z, and $\overline{\mathrm{Var}}$ is the centre variance
after a 3×3 Gaussian prefilter, raw per-pixel variance being too noisy to weight
on. $n$ is the world normal decoded from the `N*0.5+0.5` AOV but *not*
renormalised as Pass 1 renormalises, so $n_c\cdot n_q$ can exceed 1 — two
background texels decode to $(-1,-1,-1)$ and dot to 3. The clamp is what stops
that from amplifying the weight instead of attenuating it. The luminance term is
what differs from a plain à-trous, which weights squared RGB distance against a
constant — the same for converged and noisy pixels:

{{cite shaders/rt/rt_svgf_atrous.comp "w *= exp(-abs(cL - sL) / (sqrtVar * pc.sigmaL + 1e-6));"}}

:::key
Variance is the whole algorithm. Where $\mathrm{Var}$ is small the denominator
collapses, any luminance difference drives $w \to 0$, and a converged pixel keeps
its detail; where it is large the weight stays near 1 and the filter averages
aggressively. The kernel never changes — only its willingness to cross an edge.
:::

Variance is filtered alongside colour, but with the *square* of each tap weight:
the variance of a weighted mean of independent samples scales as
$\sum w^2 / (\sum w)^2$, not as the mean of the variances.

## σ_l = 0.4, and how it was chosen

The three sensitivities are compile-time constants, overridable by env var for
sweeps, and the source records the measurement that settled `kSigmaL`: at
σ_l = 4 the result came out *smoother than* the plain à-trous (mean |Laplacian|
3.25); at σ_l = 0.4 it beat both the plain filter (5.70 vs 4.38) and OIDN (4.98)
at matched flat-metal noise.

{{cite ohao/render/rt/denoise/atrous_denoise.cpp "constexpr float kSigmaL"}}

The recorded rationale is structural: Pass 1 has already removed most of the
noise, so Pass 2 only cleans a residual. A spatial stage doing the heavy
lifting needs a permissive σ_l; one following a strong temporal stage must not.

## The schedule, and the image that is three things at once

{{figure denoise-atrous-pingpong "Per-frame image schedule, transcribed from the inColor/outColor/inVar/outVar tables in atrous_denoise.cpp. Conceptual — a dataflow reading of the source, not a captured trace."}}

Five iterations need only two scratch colour images: the persistent history
doubles as the third. Iteration 0 writes `histColor[parity]`, at once iteration
1's input and next frame's history — SVGF's standard feedback point, the first
wavelet level being stable without losing the detail wider iterations destroy.

{{cite ohao/render/rt/denoise/atrous_denoise.cpp "const VkImageView inColor[kNumIterations]"}}

All five descriptor sets are written before the first dispatch is recorded: a set
bound by a recorded command cannot be updated, so rewriting one between
dispatches would be invalid.

## The motion vectors SVGF actually receives

Under à-trous the push constants carry no running-mean frames, so the trace's
beauty is this frame's sample and the denoiser owns temporal integration. The
raygen writes motion vectors whenever last frame's view is known — the push
constant's `control.z`, set once the tracer has rendered a frame since its last
reset — independent of accumulation:

{{cite shaders/rt/path_tracer.slang "if (pc.control.z != 0u)"}}
{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.z = m_hasPreviousView ? 1u : 0u;"}}

What differs between the profiles is what a moved view does. The realtime
profile only tells the tracer the view moved: the trace's running mean restarts,
the history clock does not, and SVGF reprojects through the vectors. The offline
profile resets accumulation on a view change, which zeroes the history clock and
clears the previous view, so `pc.reset` is raised and Pass 1 discards all history
unconditionally — not per pixel, not by geometry test — for every frame the
camera moves, with zero motion vectors:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bool resetsAccumulationOnViewChange() const override { return true; }"}}

So an `rt_realtime --denoise=atrous` run filters a moving view temporally; an
offline one filters each moved frame alone.

## Contracts

- Beauty, normal, depth and motion must be in `VK_IMAGE_LAYOUT_GENERAL` behind a `RAY_TRACING → COMPUTE` barrier before `dispatch()`; the denoiser barriers its own six dispatches but never transitions borrowed inputs.
- Iteration 0's colour output *is* next frame's history. Editing the ping-pong tables without moving the history binding breaks accumulation silently — the image still denoises, it never converges.
- `resetHistory` fires when `m_historyFrameCount == 0`: first frame, or any `resetAccumulation()`. The realtime profile does not call it on a camera move, so history survives the drag; the offline profile's `resetsAccumulationOnViewChange()` returns true, so every moved frame resets and SVGF filters one frame alone.
- À-trous must stay in `tracerAccumulates`' non-accumulating arms: fed a running mean, its temporal pass would average an average that restarts on every move.
- History images are framebuffer-sized, destroyed and re-created on resize; the first-frame `UNDEFINED → GENERAL` latch re-arms with them.
- `--denoise=atrous` has no build-time gate, unlike `nrd` and `dlssrr`, which fall back to `none` when their libraries were not built.
