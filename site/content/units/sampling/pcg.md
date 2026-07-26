---
module: sampling
id: pcg
title: PCG realtime
standard: v2
---

## The profile picks the sampler, and the pipeline never hears about it

OHAO ships two samplers behind one GLSL interface, and which one runs is meant to be a
property of the render profile rather than a setting a user turns. `RTRealtimeRenderer`
binds `pt_raygen_realtime.rgen` and is constructed with `kRealtimeRTSettings`, and that
struct names PCG:

{{cite ohao/render/rt/rt_settings.hpp ".samplerType = SamplerType::PCG,"}}

The intent does not survive the initialisation order. `samplerType` has exactly one reader
in the tree — pipeline creation, where it becomes a specialization constant — and that read
happens inside `PathTracer::init`:

{{cite ohao/render/rt/path_tracer.cpp "if (!createRTPipeline()) {"}}

`RTProfileRendererBase::init` calls that first and pushes the profile's settings down
afterwards:

{{cite ohao/render/rt/rt_profile_renderer.hpp "if (!m_pathTracer.init(device, physicalDevice, width, height,"}}
{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setRenderSettings(m_settings);"}}

So at the moment the constant is baked, `m_renderSettings` is still its member-initializer
default:

{{cite ohao/render/rt/path_tracer.hpp "RTRenderSettings m_renderSettings{kOfflineRTSettings};"}}

and that default's sampler is Sobol.

{{cite ohao/render/rt/rt_settings.hpp ".samplerType = SamplerType::Sobol,"}}

`PathTracer::createRTPipeline` has exactly one call site, and neither `PathTracer::setRenderSettings`
nor `PathTracer::resize` rebuilds the pipeline, so the later assignment never reaches any
SPIR-V. Both profiles specialize their raygen to `SAMPLER_SOBOL`; the realtime profile's
`SamplerType::PCG` is live C++ state that no shader ever sees. Everything below describes an
implementation that is compiled, self-consistent, and currently unreachable on the GPU.

No example parses a `--sampler` flag; switching sampler is supposed to mean switching
profile, which also switches raygen, bounce budget and denoiser. This implementation has
exactly one consumer — `sampler_api.glsl` — and its constants appear nowhere else in the
tree: the deferred pipeline's `rt_shadow.rgen` and `rt_gi.rgen` roll their own `hash(vec2)`
and never touch this interface.

:::why
The realtime profile asks for PCG because Sobol's cost is per *dimension*, not per
sample: each `getSample1D_sobol` runs a bit-serial loop over the set bits of the
sample index against a direction-number table, then a Burley Owen scramble of two
full 32-bit bit-reversals and four multiplies. A realtime path consumes dozens of
dimensions per pixel per frame. PCG is one multiply-add plus a fixed seven-operation
permutation per uniform, dimension count irrelevant. What it gives up is
stratification — and the tree measures exactly how much.
:::

The verification log compares them at equal 16 spp on the `envlit_turntable` scene, from
when PCG was still the offline default: Sobol reaches 0.002584 local 5×5 variance against
PCG's 0.002863, logged as a 9.8% improvement.

{{cite tests/reference_scenes/custom/envlit_turntable/verification_log.md "| PCG (previous)  | 0.002863  | 0.069884 |"}}

That understates it, because roughly half of each figure is scene detail rather than
noise — the same log's 4096-spp reference puts the signal-only floor at 0.001383, which is
48% of the PCG number and 54% of the Sobol one.

{{cite tests/reference_scenes/custom/envlit_turntable/verification_log.md "- Ground-truth local 5x5 variance:        0.001383  (signal detail only)"}}

Subtracting that floor, sampling noise falls from 0.001480 to 0.001201: a 19%
reduction, derived from the logged numbers rather than separately measured. Real,
but not the order-of-magnitude gap "low-discrepancy" invites you to expect.

## A full-period LCG behind a bit-mixing permutation

The generator is PCG-RXS-M-XS in its 32-bit form. State advance is a plain linear
congruential step:

$$s_{n+1} = \left(a\,s_n + c\right) \bmod 2^{32}, \qquad a = 747796405,\; c = 2891336453$$

{{cite shaders/includes/rt/sampler_pcg.glsl "_pcg_state = _pcg_state * 747796405u + 2891336453u;"}}

Here $s_n$ is the 32-bit state and the modulus is free — it is just uint overflow.
Because $a \equiv 1 \pmod 4$ and $c$ is odd, the Hull–Dobell conditions hold, so the
state cycles through all $2^{32}$ values from *any* seed. The `+ 1u` in the seed is
therefore not load-bearing for period; nothing about the seed can shorten the cycle.

A raw LCG is unusable as-is: bit $k$ of the state has period $2^{k+1}$, so the low bit
merely alternates. The output permutation fixes that before anything leaves the
generator — with $\gg$ a logical shift and $\oplus$ xor:

$$k = (s \gg 28) + 4, \qquad w = \big((s \gg k) \oplus s\big)\cdot 277803737, \qquad \text{out} = (w \gg 22) \oplus w$$

{{cite shaders/includes/rt/sampler_pcg.glsl "uint w = ((_pcg_state >> ((_pcg_state >> 28u) + 4u)) ^ _pcg_state) * 277803737u;"}}

The first step is the "random xorshift": the shift distance $k \in [4, 19]$ is chosen
by the state's own top four bits, so mixing depth varies with the input and no fixed
bit-position correlation survives. The odd multiplier propagates low bits upward, and
the final fold pushes the top ten bits back down. Only then does the value become a
float — which is why dividing the *output* by $2^{32}$ is legitimate while dividing
the raw state would not be. Ordering matters here too: `_pcg_next` advances before it
permutes, so the seed itself is never observable.

## The seed is a plane through pixel space

Seeding is where the cheapness shows. There is no hash — the state starts as an
affine function of the pixel and the sample index:

$$s_0(x, y, i) = 1973x + 9277y + 26699\,i + 1 \pmod{2^{32}}$$

{{cite shaders/includes/rt/sampler_pcg.glsl "_pcg_state = pixel.x * 1973u + pixel.y * 9277u + sampleIdx * 26699u + 1u;"}}

Two invocations share their whole stream exactly when their seeds collide, so this
line's quality is a lattice question. Within one frame $i$ is fixed and a collision
needs $1973\,\Delta x + 9277\,\Delta y \equiv 0$. At 8K the left side is at most
$1973 \cdot 7680 + 9277 \cdot 4320 \approx 5.5 \times 10^{7}$, far below $2^{32}$, so
the congruence is an ordinary equality; $\gcd(1973, 9277) = 1$ then forces
$9277 \mid \Delta x$, and 9277 exceeds any shipping image width. No two pixels in a
frame ever share a stream.

Across the sample index it is not so clean. Solving
$1973\,\Delta x + 9277\,\Delta y = -26699$ gives the minimal offset
$(\Delta x, \Delta y) = (1966, -421)$, which fits inside a 2560×1440 frame: pixel $p$
at sample $i$ and pixel $p + (1966, -421)$ at sample $i+1$ would draw identical uniforms
for the whole path. Neither estimate is biased by this — each still sees a well-permuted
stream. It is structure that a per-pixel hash (Sobol seeds through a full Murmur-style
finalizer) would not have.

## `dim` is an argument this sampler discards

The interface takes a dimension index. PCG ignores it and returns the next value in
its stream:

{{cite shaders/includes/rt/sampler_pcg.glsl "float getSample1D_pcg(uint dim) {"}}

For Sobol that argument is load-bearing — `getSample1D_sobol(dim)` is a pure function
of (sample index, dimension, pixel seed), with `dim >> 2` selecting an
independently-scrambled 4D pad. For PCG only call *order and count* matter, and the
realtime raygen is laid out as though that were not so. It consumes the sub-pixel jitter
at dimensions 0–1, then re-seeds inside its per-sample loop and resumes bookkeeping at
dimension 2:

{{cite shaders/rt/pt_raygen_realtime.rgen "samplerInit(uvec2(pixel), frameIdx * samplesPerFrame + s);"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "dims 0..1 stay"}}

With the default `samplesPerFrame = 1` that re-seed computes `frameIdx * 1 + 0` — the
same seed used for the jitter a few lines earlier. Sobol is untouched, because dimension 2
is a different function evaluation. PCG would restart the stream: `dimIdx = 2u` buys it
nothing, and the frame's first path uniform comes out numerically equal to
`jitter.x + 0.5`. On a primary hit with a non-empty light SSBO that uniform is the one
that selects the NEE light, so the light chosen at a pixel would become a function of
where inside the pixel the camera ray landed. The jitter partitioned here is the sub-pixel
*x* offset, so with $L$ lights, light $k$ is only ever paired with one $1/L$-wide band of
$x$ — a vertical strip of the pixel footprint rather than a point drawn from all of it.

{{cite shaders/rt/pt_raygen_realtime.rgen "vec2 uv = (vec2(pixel) + 0.5 + jitter + pc.jitter.xy)"}}

None of that happens in any shipping configuration, because the ordering above leaves this
raygen specialized to Sobol. It is exactly the dimension reuse the padded-Sobol accounting
exists to prevent, waiting behind a one-line fix to the initialisation order.

## The half-open interval one sampler promises and the other does not

Sobol shifts right by 8 and scales by $2^{-24}$, capping at $1 - 2^{-24}$, and says so:

{{cite shaders/includes/rt/sampler_sobol.glsl "// Match the CPU 24-bit right-shift approach — guarantees [0, 1)."}}

PCG's 31 lines make no claim about their range at all. They divide the full 32-bit output
by $2^{32}$ in `float` precision:

{{cite shaders/includes/rt/sampler_pcg.glsl "return float(_pcg_next()) / 4294967296.0;"}}

Near $2^{32}$ the spacing of `float32` is 256, so the 128 largest uint outputs round up
to exactly $2^{32}$ and the quotient is exactly 1.0 — probability $2^{-25}$, one sample
in 33.5 million. Nothing downstream would break if that value were ever produced, because
the three kinds of site that turn a uniform into an index are all bounded: every
light-selection site in all three raygen shaders follows `uint(u * lightCount)` with a
`min(..., lightCount - 1u)` clamp; the environment CDF lookup is a binary search with a
pre-clamped upper bound; and ReSTIR-GI's spatial-reuse tap clamps its neighbour offset to
the framebuffer.

{{cite shaders/includes/rt/env_sampling.glsl "int hi = int(H) - 1;"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "ivec2 qPix = clamp(baseCenter + off, ivec2(0), ivec2(pc.params.xy) - 1);"}}

:::key
The clamps, not the sampler, are what make PCG's open-interval violation harmless.
Any new consumer that indexes an array with a raw `getSample1D` result inherits a
one-in-33-million out-of-bounds read that cannot happen under Sobol at all.
:::

## How the choice reaches the GPU

Selection happens at pipeline creation, not per ray: `sampler_api.glsl` declares the
sampler as a specialization constant and branches on it.

{{cite shaders/includes/rt/sampler_api.glsl "layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_SOBOL;"}}

The CPU writes `m_renderSettings.samplerType` into a `VkSpecializationInfo` attached to
exactly one stage.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pSpecializationInfo = &samplerSpecInfo;"}}

Stage 0 is the raygen, which is currently sufficient: `sampler_api.glsl` is included
by exactly three files — `pt_raygen.rgen`, `pt_raygen_offline.rgen`,
`pt_raygen_realtime.rgen` — and by no closest-hit, miss or any-hit shader. Note also
that the GLSL default is `SAMPLER_SOBOL` while the C++ enum gives PCG the value 0: an
*absent* specialization yields Sobol, a *zero-filled* one yields PCG. The specialization
written here is never absent — it is simply always read one step too early.

## Contracts

- `PathTracer::createRTPipeline` reads `m_renderSettings.samplerType` at build time and has exactly one call site, inside `PathTracer::init`. A profile that wants a non-default sampler must push its settings down *before* `init`; none does, so every RT pipeline in the tree is specialized from the `kOfflineRTSettings` member initializer.
- The sampler specialization reaches the raygen stage only. If a closest-hit or miss shader ever includes `sampler_api.glsl`, it compiles with the declared default `SAMPLER_SOBOL` regardless of profile, and the two stages silently draw from different samplers.
- `_pcg_state` is a single module-scope uint: one stream per invocation, and only one. A second decorrelated stream can only be had by re-seeding, which discards the first — so any `dimIdx` reservation scheme built around a re-seed protects Sobol and not PCG.
- `getSample1D` may return exactly 1.0 under PCG and never under Sobol. Consumers must clamp indices or use bounded searches; light selection, the environment CDF lookup and ReSTIR-GI's spatial tap all do.
- Neither sampler's GLSL is value-tested. `sobol_test.cpp` asserts against the C++ `SobolGenerator::sample1D` and `owenScramble` only; the GLSL's claim to mirror them byte-for-byte is a source comment, not an assertion. PCG has no CPU-side reference implementation at all, and the only PCG assertion in the suite is the `isQmcSampler` trait predicate. A typo in either permutation's constants would not fail the test build.
