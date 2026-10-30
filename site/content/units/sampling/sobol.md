---
module: sampling
id: sobol
title: Owen-scrambled Sobol
standard: v2
---

## The path tracer's random numbers

Every draw the path tracer makes, in both RT profiles, comes from one module,
`shaders/sampling/sobol.slang`: Burley's shuffled, Owen-scrambled Sobol sequence in
four dimensions (Burley 2020, "Practical Hash-based Owen Scrambling"). A pixel's
samples are one sequence, so its frames are stratified against each other however
many there are; pixels are decorrelated by a seed.

{{cite shaders/rt/path_tracer.slang "import sampling.sobol;"}}

The sampler is built once per launch from the sample index and a pixel seed. The
index is `params.z`, which the host advances once a frame and puts back at the render
seed on a reset; the seed is a PCG hash of the pixel alone (*PCG hash*), so it does
not change from frame to frame:

{{cite shaders/rt/path_tracer.slang "Sampler rng = Sampler(pc.params.z, pcg(pixel.x ^ pcg(pixel.y ^ 0x7F4A7C15u)));"}}
{{cite ohao/render/rt/path_tracer.cpp "m_sampleIndex = m_renderSeed;"}}

:::why
The seed belongs to the pixel and the index to the sequence. A seed that changed
with the frame would draw each frame's point from a different scrambled sequence,
and the running mean of a pixel would converge like white noise. Keeping the seed
fixed is what lets frame $k$ fill the strata frames $0 \dots k{-}1$ left empty.
:::

## Four dimensions of direct bit expansion

The direction numbers are Joe and Kuo's (`new-joe-kuo-6.21201`): dimension 0 is the
bit reversal (the van der Corput sequence), dimensions 1 to 3 are their polynomials
$d = 2, 3, 4$, expanded to 32 numbers each.

{{cite shaders/sampling/sobol.slang "static const uint kSobolDirections[128] = {"}}

A value is the XOR of the direction numbers whose bit is set in the index $i$,

$$x_i^{(d)} = \bigoplus_{j\,:\,b_j = 1} v_j^{(d)}, \qquad i = \sum_j b_j 2^{\,j},$$

where $v_j^{(d)}$ is the 32-bit direction number for bit $j$ of dimension $d$. There is
no Gray-code recurrence, so any index is evaluated without state from the previous
sample:

{{cite shaders/sampling/sobol.slang "if ((index & 1u) != 0u) x ^= kSobolDirections[dim * 32u + bit];"}}

## The scramble and the shuffle

Owen scrambling permutes a value's binary digits, each digit flipped by a choice that
depends only on the digits above it. The permutation preserves measure (the estimate
stays unbiased) and maps dyadic intervals to dyadic intervals (the net's
stratification survives). `owenScramble` is Burley's hash form of it: reverse the
bits so the hash, which only carries information upward, acts on the most
significant digits first; mix with Vegdahl's constants and the seed; reverse back.

{{cite shaders/sampling/sobol.slang "v ^= v * 0x3d20adeau;"}}

`reverseBits` is written out with masks and shifts because slangc's C++ target, which
the CPU twin compiles through, has no `reversebits`:

{{cite shaders/sampling/sobol.slang "// The bits of v in reverse order (written out: slangc's C++ target has no reversebits)."}}

`sobol4` scrambles twice. The index itself is Owen-scrambled first (Burley's shuffle,
so a pixel visits the sequence's points in a seed-dependent order), and then each
coordinate is scrambled with its own seed, `hashCombine(seed, d)`:

{{cite shaders/sampling/sobol.slang "const uint i = owenScramble(index, seed);"}}
{{cite shaders/sampling/sobol.slang "p[d] = float(owenScramble(sobolBits(i, d), hashCombine(seed, d)) >> 8u) * (1.0 / 16777216.0);"}}

The shift by 8 keeps the top 24 bits, which a float holds exactly, so every
coordinate lies in $[0, 1)$ and `uint(u * n)` stays below `n`.

## Groups of four

A path needs far more than four numbers. `Sampler` hands them out in groups: a group
is one 4-D point, seeded by hashing the pixel's seed with the group's number
(Burley's padding), so dimensions of different groups are independent, while the four
coordinates inside a group are jointly stratified.

{{cite shaders/sampling/sobol.slang "if ((dim & 3u) == 0u) point = sobol4(index, hashCombine(seed, dim >> 2u));"}}

The tracer places related draws in one group. Group 0 is the camera jitter; bounce
$d$ takes its light choice and light point from group $1 + 3d$, its environment
sample from $2 + 3d$, and its BSDF sample and Russian roulette test from $3 + 3d$:

{{cite shaders/rt/path_tracer.slang "// bounce d's light choice and point in group 1 + 3d, its environment sample in 2 + 3d, its BSDF"}}

Because each decision starts at its group's first coordinate, a branch that draws
fewer numbers (no light sample, no environment) moves no later decision onto another
dimension.

:::key
Measured on the closed forms at 64 samples a pixel, against the PCG stream the
tracer otherwise would draw from: the closed box's RMSE is 0.065 (PCG 0.104) and the
white furnace's 0.0112 (PCG 0.0322).
:::

## The CPU twin and its tests

`ohao/render/rt/sobol_generator.*` and `owen_scramble.*` are the module's CPU twin:
the same direction numbers and the same scramble, in C++. The generator's table
says where its numbers come from:

{{cite ohao/render/rt/sobol_generator.cpp "// Dimension 2: Joe-Kuo d=3, s=2, a=1, m={1, 3}"}}

`sobol_test` holds the twin and the module to outside references and to each other:

- `Sobol.DirectionNumbersAreJoeKuos` regenerates all 128 direction numbers from Joe
  and Kuo's $(s, a, m)$ by the Bratley-Fox recurrence and compares them with the table.
- `Sampler.ModuleIsBurleysShuffledScrambledSobol` runs `sobol.slang` itself — compiled
  to C++ by slangc through `tools/sampling/sobol_kernel.slang` into the library
  `ohao_sobol_cpu` — and requires its points to equal a C++ reference of Burley's
  `shuffled_scrambled_sobol4d`, built from the generator and `owenScramble`, bit for
  bit.
- `Sampler.ScramblingKeepsEveryPairsStratification` checks that, for every pair of
  dimensions and every split of the first $2^8$ points into elementary intervals, the
  scrambled points occupy as many intervals as the unscrambled sequence does (all
  $2^m$ for dimensions 0 and 1).
- `Sampler.GroupsAreIndependent` checks that two groups of one sample differ.

{{cite tests/renderer/sobol_test.cpp "TEST(Sobol, DirectionNumbersAreJoeKuos) {"}}
{{cite tests/renderer/sobol_test.cpp "TEST(Sampler, ModuleIsBurleysShuffledScrambledSobol) {"}}
{{cite tools/sampling/CMakeLists.txt "add_library(ohao_sobol_cpu STATIC sobol_cpu.cpp ${OHAO_SOBOL_KERNEL_CPP})"}}
{{cite tests/renderer/CMakeLists.txt "target_link_libraries(sobol_test PRIVATE ohao_sobol_cpu)"}}

The generator's float conversion drops the low byte the same way the shader does, and
a test guards it at index $2^{31}$ in dimension 1, whose direction number is
`0xFFFFFFFF`:

{{cite ohao/render/rt/sobol_generator.cpp "(v >> 8) is at most 2^24-1, so (v >> 8) * (1/2^24) < 1 exactly."}}

## Contracts

- `kSobolDirections` in `sobol.slang` and `kDirectionNumbers` in `sobol_generator.cpp` are one table in two languages; `Sampler.ModuleIsBurleysShuffledScrambledSobol` fails if they part.
- The sequence has four dimensions. A decision needing more than four numbers spans two groups and loses joint stratification across them.
- A new decision in the path takes its own group (or the unused coordinates of an existing one); two decisions drawing from the same coordinates are correlated.
- The pixel seed must not depend on the sample index; the index must advance once per frame and return to the render seed on a reset.
- Draws are in $[0, 1)$; the `>> 8u` before the float multiply is load-bearing.
