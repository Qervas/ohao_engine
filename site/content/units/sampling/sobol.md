---
module: sampling
id: sobol
title: Sobol + Owen
standard: v2
---

## A host generator with no shader behind it

`ohao/render/rt/` holds a Sobol generator for four dimensions and Burley's
hash-based Owen scramble, both pure C++. No shader in the tree draws Sobol
points: the path tracer, the RT shadow trace and RT GI's trace all take their
uniforms from PCG hashes (*PCG stream*). The two files are compiled into the
renderer library by the source glob and have no engine caller; `sobol_test` is
the only code that calls them. Their headers name the role they are kept for: the
CPU twin of an Owen-scrambled Sobol sampler in `rt/path_tracer.slang`, which the
tracer does not yet have.

{{cite ohao/render/rt/sobol_generator.hpp "static float sample1D(uint32_t index, uint32_t dim);"}}
{{cite tests/renderer/CMakeLists.txt "add_executable(sobol_test sobol_test.cpp   # Sobol + Owen scramble"}}

What follows is what that code computes, and what its tests do and do not hold.

## Direct bit expansion

The direction-number table is four dimensions wide and labelled Joe-Kuo
`new-joe-kuo-6.21201`. Dimension 0 is van der Corput — the identity table
$2^{31}, 2^{30}, \dots, 2^0$ — and dimension 1 is the primitive polynomial $x{+}1$
with $m_1 = 1$.

{{cite ohao/render/rt/sobol_generator.cpp "constexpr uint32_t kDirectionNumbers[SobolGenerator::kDimensions * 32] = {"}}

A Sobol value is built by direct bit expansion: for sample index $i$ with binary
digits $b_j$, the value in dimension $d$ is the XOR of the direction numbers
whose bit position is set,

$$x_i^{(d)} \;=\; igoplus_{j\,:\,b_j = 1} v_j^{(d)}, \qquad i = \sum_j b_j 2^{\,j}$$

where $v_j^{(d)}$ is the 32-bit direction number for bit $j$ of dimension $d$.
That is the whole generator — one XOR per set bit, no Gray-code recurrence, so an
arbitrary index is evaluated with no state from the previous sample.

{{cite ohao/render/rt/sobol_generator.cpp "result ^= dirs[bit];"}}

## The scramble

Raw Sobol has two problems. Its first point is the origin — every dimension of
index 0 is exactly zero — and it is deterministic, so its structure aliases into
visible patterns. Owen scrambling permutes the digits of the value: the permutation
is measure-preserving (an estimator stays unbiased) and maps dyadic intervals to
dyadic intervals (the stratification survives). `owenScramble` is Burley's 2020
construction — reverse the bits so the hash acts on the *most* significant digits
first, run the XOR-multiply rounds folded with the seed, reverse back:

{{cite ohao/render/rt/owen_scramble.cpp "v ^= v * 0x3d20adeau;"}}

The tests hold it deterministic, decorrelated across seeds and valid at input 0;
they do not hold it to a reference implementation's values.

{{cite tests/renderer/sobol_test.cpp "TEST(Owen, DifferentSeedsDecorrelate) {"}}

## Eight bits thrown away on purpose

The float conversion discards the low byte and scales by $2^{-24}$ rather than
dividing the full 32-bit value by $2^{32}$.

{{cite ohao/render/rt/sobol_generator.cpp "return static_cast<float>(v >> 8) * (1.0f / static_cast<float>(1u << 24));"}}

This buys the half-open interval for free. A 32-bit integer near `0xFFFFFFFF` has
more significant bits than a `float` mantissa holds, so round-to-nearest lands on
exactly $2^{32}$ and the division returns 1.0. After the shift the value is at most
$2^{24}-1$, exactly representable, so the product is strictly below 1. It is not the
only way — pbrt keeps all 32 bits and clamps the result to `OneMinusEpsilon` — but
it is the one that costs no instructions. A host regression test guards it at index
$2^{31}$ in dimension 1, whose direction number is `0xFFFFFFFF`:

{{cite ohao/render/rt/sobol_generator.cpp "cast of values near 0xFFFFFFFF rounds up to exactly 1.0 in IEEE-754."}}
{{cite tests/renderer/sobol_test.cpp "float v = SobolGenerator::sample1D(2147483648u, 1);"}}

## What the tests hold

Exactly one test checks against numbers from outside the tree, and it covers
dimensions 0 and 1. The dimension-2 and dimension-3 tests state in their own
comments that their references were computed from the direction numbers under
test, so they pass for whatever the table holds.

{{cite tests/renderer/sobol_test.cpp "TEST(Sobol, First8PointsMatchJoeKuoReference) {"}}
{{cite tests/renderer/sobol_test.cpp "// Dim 2 reference values, computed from the dim-2 direction numbers"}}

## Two of the four columns are not Sobol

The initial numbers are recoverable from the table. Direction number $v_{k-1}$
stores $m_k \cdot 2^{\,32-k}$, so $m_k = v_{k-1} \gg (32-k)$. Feeding those back
through the Sobol recurrence for a degree-$s$ polynomial with coefficients $a_i$,

$$m_k \;=\; \bigoplus_{i=1}^{s-1} 2^{\,i} a_i\, m_{k-i} \;\oplus\; 2^{\,s} m_{k-s} \;\oplus\; m_{k-s}$$

regenerates the rest of the column — if the column came from that polynomial.
Dimension 1 regenerates for all 32 numbers under $x{+}1$ with $m_1 = 1$, matching
its comment. The other two columns do not match theirs.

Dimension 2's comment names $x^2{+}x{+}1$ with $m = \{1, 3\}$, but $v_2$ is
`0x40000000`, i.e. $m_2 = 1$. Recovered, the column reads
$m = \{1, 1, 7, 5, 19, 47, \dots\}$, and neither admissible start reproduces it:
$\{1,1\}$ generates $1, 1, 7, 11$ and $\{1,3\}$ generates $1, 3, 3, 9$. A search
over every recurrence of the form above up to degree 8 — all $2^{\,s-1}$
coefficient choices, primitive or not, seeded from the column's own leading
values — matches at most $m_1 \dots m_9$ before diverging, and eight of those
nine are the seed. That column is not a Sobol dimension from any polynomial in
that range.

{{cite ohao/render/rt/sobol_generator.cpp "// Dimension 2: primitive polynomial x^2+x+1, initial m = {1, 3}"}}
{{cite ohao/render/rt/sobol_generator.cpp "0x80000000u, 0x40000000u, 0xE0000000u, 0x50000000u,"}}

Dimension 3's polynomial is right and its comment is wrong the other way: $v_2 =$
`0xC0000000` and $v_3 =$ `0x20000000` give $m = \{1, 3, 1\}$, not the $\{1,1,1\}$
the comment claims. With $\{1,3,1\}$, $x^3{+}x{+}1$ regenerates $m_1$ through
$m_{17}$ and then breaks: the recurrence requires $v_{18} =$ `0x51474000`
($m_{18} = 83229$), the table holds `0x51CBC000` ($m_{18} = 83759$). Seventeen of
its 32 direction numbers are the ones the polynomial dictates.

{{cite ohao/render/rt/sobol_generator.cpp "// Dimension 3: primitive polynomial x^3+x+1, initial m = {1, 1, 1}"}}
{{cite ohao/render/rt/sobol_generator.cpp "0x208F8000u, 0x51CBC000u, 0xFBEA2000u, 0x75AD5000u,"}}

Two consequences. Dimensions 2 and 3 carry no equidistribution guarantee beyond
whatever they happen to have; replaying the table offline, of the six dimension
pairs only (0, 1) is a (0,2)-net at N = 16, 64 and 256. And the two tests that
would have caught this are the two that were written from the table instead of
from a reference.

## Contracts

- No shader consumes this code. A shader sampler built on it must take its table and scramble from these files (or a generated copy checked against them) and be tested against `SobolGenerator::sample1D` and `owenScramble` on the GPU, which nothing does today.
- Only four dimensions exist, and only (0, 1) is a jointly stratified pair. Dimensions 2 and 3 must be regenerated from their polynomials, with tests from an outside reference, before anything relies on them.
- Sample values must stay in [0, 1). The `>> 8` before the float multiply is load-bearing.
- An Owen seed that varies with the sample index turns a progressive Sobol estimate into white noise while still passing every test in `sobol_test`; the seed belongs to the pixel (and the dimension group), the index to the sequence.
