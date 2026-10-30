---
module: sampling
id: pcg
title: PCG stream
standard: v2
---

## One generator, no sampler choice

Every random number the RT passes draw comes from one function: the 32-bit
PCG-RXS-M-XS permutation. The path tracer (`rt/path_tracer.slang`, both RT profiles),
the deferred frame's RT shadow trace (`rt/shadow_trace.slang`) and RT GI's trace
(`rt/gi_trace.slang`) each carry their own copy with the same constants. There is no
sampler setting and no specialization constant: a profile changes bounces, denoiser
and accumulation, never the generator.

{{cite shaders/rt/path_tracer.slang "// ---- random numbers: one PCG stream a pixel and sample ------------------------------------"}}

:::why
A low-discrepancy sequence's cost is per *dimension*: each draw is a function of the
sample index and a dimension number, and the integrator has to keep a ledger of which
dimension every decision spends so no two decisions share one. A path of up to 32
bounces with Russian roulette, a light pick, a light point, an environment direction
and a three-uniform BSDF sample at each vertex spends a variable number of draws per
path. A stream drawn in call order needs no ledger, and the reference is judged by
converging, not by its noise at a fixed sample count.
:::

## A full-period LCG behind a bit-mixing permutation

The function is an LCG step followed by an output permutation:

$$s' = \left(a\,v + c\right) \bmod 2^{32}, \qquad a = 747796405,\; c = 2891336453$$

{{cite shaders/rt/path_tracer.slang "const uint state = v * 747796405u + 2891336453u;"}}

Here $v$ is the 32-bit input and the modulus is free — it is just uint overflow.
Because $a \equiv 1 \pmod 4$ and $c$ is odd, the Hull–Dobell conditions hold for the
LCG alone: iterated by itself it would cycle through all $2^{32}$ values from any start.

A raw LCG is unusable as-is: bit $k$ of the state has period $2^{k+1}$, so the low bit
merely alternates. The output permutation fixes that — with $\gg$ a logical shift and
$\oplus$ xor:

$$k = (s' \gg 28) + 4, \qquad w = \big((s' \gg k) \oplus s'\big)\cdot 277803737, \qquad \text{out} = (w \gg 22) \oplus w$$

{{cite shaders/rt/path_tracer.slang "const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;"}}

The first step is the "random xorshift": the shift distance $k \in [4, 19]$ is chosen
by the value's own top four bits, so mixing depth varies with the input and no fixed
bit-position correlation survives. The odd multiplier propagates low bits upward, and
the final fold pushes the top ten bits back down.

## The path tracer's stream

The path tracer seeds one state per pixel and sample with nested hashes of the pixel
and the sample index:

{{cite shaders/rt/path_tracer.slang "rng.state = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z * 0x9E3779B9u + 0x7F4A7C15u)));"}}

The sample index is `params.z`, which the host advances once a frame and puts back at
the render seed on a reset, so a capture after `resetAccumulation` draws the same
uniforms every run:

{{cite ohao/render/rt/path_tracer.cpp "m_sampleIndex = m_renderSeed;"}}

A change of one pixel or one sample index goes through a full permutation before it
reaches the state, so neighbouring pixels, and one pixel's consecutive samples, start
from unrelated states.

Each draw replaces the state by its own hash and keeps the top 24 bits, so the
uniforms lie in $[0, 1)$ — never exactly 1, which keeps `uint(u * n)` below `n`:

{{cite shaders/rt/path_tracer.slang "return float(state >> 8u) * (1.0 / 16777216.0);"}}

Feeding the output back as the state changes what the period argument above covers:
the state walks the composed map, not the LCG. Every piece of it is invertible — the
LCG step, the random xorshift (its shift distance is set by the top four bits, which
the shift leaves untouched), the odd multiply, the final fold — so the map permutes
the 32-bit words and two states never merge, but the length of the cycle through a
given seed has not been established here. A path draws a few uniforms a vertex, far
below any plausible cycle.

The stream is drawn in call order and has no dimension index, so a branch that draws
more or fewer uniforms (a light with no sample, a path ended by roulette) shifts every
later draw of that path. That changes which uniforms a later decision sees, not their
distribution.

## The hybrid passes: a stateless hash

The shadow and GI traces do not keep a stream. They hash a pixel key (the frame's
seed and the pixel) with the light, the sample and a dimension number into one value
per draw:

{{cite shaders/rt/shadow_trace.slang "const uint h = pcg(pixelKey ^ pcg((light << 24u) ^ (s << 4u) ^ dim));"}}
{{cite shaders/rt/gi_trace.slang "const uint h = pcg(pixelKey ^ pcg((s << 4u) ^ dim));"}}

That makes every draw a pure function of (seed, pixel, light, sample, dimension): the
passes can skip a light or a ray without moving any other draw, which a stream cannot.

:::key
Nothing tests these permutations' values. A wrong constant in any copy would still
produce plausible noise; it would show only as a changed golden or slower
convergence. The copies are three, and they must change together.
:::

## Contracts

- The path tracer's uniforms are in $[0, 1)$; an index formed as `uint(u * n)` is below `n`, and the light pick also clamps to `n - 1`.
- `params.z` is the sample index; it returns to the render seed at every `resetAccumulation`. A host that wants run-to-run identical captures resets before capturing.
- The shadow and GI traces' draws are keyed by dimension number; a new draw there takes a dimension no other draw of that pass uses.
- `shadow_trace.slang`, `gi_trace.slang` and `path_tracer.slang` each define `pcg`; changing the constants in one leaves the others as they were.
