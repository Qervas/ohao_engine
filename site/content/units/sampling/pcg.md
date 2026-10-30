---
module: sampling
id: pcg
title: PCG hash
standard: v2
---

## A hash, not the sampler

The path tracer's random numbers are Owen-scrambled Sobol (*Owen-scrambled Sobol*).
What it still needs besides them is a hash: something that turns a pixel, a ray or a
triangle into a well-mixed 32-bit word. That is the 32-bit PCG-RXS-M-XS permutation,
and the RT passes each carry a copy with the same constants: the path tracer
(`rt/path_tracer.slang`), the RT shadow trace (`rt/shadow_trace.slang`), the shadow
filter (`rt/shadow_filter.slang`) and RT GI's trace (`rt/gi_trace.slang`).

{{cite shaders/rt/path_tracer.slang "// sample and Russian roulette in 3 + 3d. PCG hashes the pixel's seed and the any-hit's coin."}}

## A full-period LCG behind a bit-mixing permutation

The function is an LCG step followed by an output permutation:

$$s' = \left(a\,v + c\right) \bmod 2^{32}, \qquad a = 747796405,\; c = 2891336453$$

{{cite shaders/rt/path_tracer.slang "const uint state = v * 747796405u + 2891336453u;"}}

Here $v$ is the 32-bit input and the modulus is free — it is just uint overflow.
Because $a \equiv 1 \pmod 4$ and $c$ is odd, the Hull–Dobell conditions hold for the
LCG alone: iterated by itself it would cycle through all $2^{32}$ values from any start.

A raw LCG is a poor hash: bit $k$ of the state has period $2^{k+1}$, so the low bit
merely alternates. The output permutation fixes that — with $\gg$ a logical shift and
$\oplus$ xor:

$$k = (s' \gg 28) + 4, \qquad w = \big((s' \gg k) \oplus s'\big)\cdot 277803737, \qquad \text{out} = (w \gg 22) \oplus w$$

{{cite shaders/rt/path_tracer.slang "const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;"}}

The first step is the "random xorshift": the shift distance $k \in [4, 19]$ is chosen
by the value's own top four bits, so mixing depth varies with the input and no fixed
bit-position correlation survives. The odd multiplier propagates low bits upward, and
the final fold pushes the top ten bits back down. Every step is invertible, so the
whole function permutes the 32-bit words: two different inputs never hash alike.

## The path tracer's two uses

The sampler's pixel seed is a nested hash of the pixel's coordinates and a constant.
It does not involve the sample index — that is the sequence's index, not part of the
seed:

{{cite shaders/rt/path_tracer.slang "Sampler rng = Sampler(pc.params.z, pcg(pixel.x ^ pcg(pixel.y ^ 0x7F4A7C15u)));"}}

The any-hit shader's coin for a blended surface hashes the launch pixel, the
triangle and the instance, then the ray's origin and direction, and keeps 24 bits as
a uniform in $[0, 1)$; the candidate survives when the uniform is below its opacity:

{{cite shaders/rt/path_tracer.slang "uint h = pcg(launch.x ^ pcg(launch.y ^ pcg(PrimitiveIndex() ^ pcg(InstanceID()))));"}}

The coin is a function of the ray, not of the sampler's state, so an any-hit call —
which the traversal may make in any order and any number of times — draws nothing
from the path's sequence.

## The hybrid passes: a stateless hash

The shadow and GI traces hash a pixel key (the frame's seed and the pixel) with the
light, the sample and a dimension number into one value per draw:

{{cite shaders/rt/shadow_trace.slang "const uint h = pcg(pixelKey ^ pcg((light << 24u) ^ (s << 4u) ^ dim));"}}
{{cite shaders/rt/gi_trace.slang "const uint h = pcg(pixelKey ^ pcg((s << 4u) ^ dim));"}}

That makes every draw a pure function of (seed, pixel, light, sample, dimension): the
passes can skip a light or a ray without moving any other draw. GI's light
candidates take their own sequence from the same key, and the shadow filter dithers
its packed visibility with a hash of the frame's seed and the pixel:

{{cite shaders/rt/gi_trace.slang "const uint stream = pcg(key ^ pcg((s << 4u) ^ 3u));   // the candidates' own sequence"}}
{{cite shaders/rt/shadow_filter.slang "const float dither = float(pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y)))) >> 8u) * (1.0 / 16777216.0);"}}

:::key
Nothing tests these permutations' values. A wrong constant in any copy would still
produce plausible noise; it would show only as a changed golden or slower
convergence. The copies are four, and they must change together.
:::

## Contracts

- The pixel seed is a function of the pixel only; the sample index belongs to the Sobol sequence.
- The any-hit coin is a pure function of the ray and the triangle; it must not draw from the path's sampler.
- The shadow and GI traces' draws are keyed by dimension number; a new draw there takes a dimension no other draw of that pass uses.
- `path_tracer.slang`, `shadow_trace.slang`, `shadow_filter.slang` and `gi_trace.slang` each define `pcg`; changing the constants in one leaves the others as they were.
