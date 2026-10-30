---
module: sampling
id: env-cdf
title: Environment CDF
standard: v2
---

## The light you cannot sample uniformly

An HDR environment map is an area light with W×H emitters and several orders of
magnitude between its brightest texel and its dimmest. Pick a direction uniformly
on the sphere and the chance of landing on the solar disc is its solid angle over
4π: a half-degree sun subtends about 6×10⁻⁵ sr, so roughly five samples in a
million. Every other sample returns sky-blue instead of sun, and the estimator
converges like a lottery. The fix is the textbook one: tabulate a discrete 2D
distribution over texels proportional to the light each carries, and invert it by
binary search on the GPU.

OHAO builds that table on the CPU, once, from the float buffer `stbi_loadf`
returned — after that buffer has already been memcpy'd into a staging buffer,
copied into a `DEVICE_LOCAL` image and waited on. The build is the last read of
the host copy before `stbi_image_free` drops it, not a step on the way to the
upload:

{{cite ohao/gpu/vulkan/env_upload.cpp "envCDF.build(hdrPixels, ew, eh);"}}

Out come a conditional CDF per row (W floats × H rows) and a marginal CDF over
rows (H floats), handed to the path tracer with the map's width, height and
unnormalised integral. One sampler reads them: the path tracer's own `sampleEnv`
and `envPdf` in `rt/path_tracer.slang`, which both RT profiles run.

## The sinθ that has to be there

The distribution is tabulated in texture coordinates, but the path tracer needs a
density in solid angle. For the equirectangular parameterisation the shader uses
— longitude φ = (u − ½)·2π, colatitude θ = vπ — the two measures differ by

$$d\omega \;=\; \sin\theta\,d\theta\,d\varphi \;=\; 2\pi^2 \sin\theta \; du\,dv$$

so a density that is flat in (u,v) is *not* flat on the sphere: it crowds the
poles, where a row of texels covers almost no solid angle. Converting, a target
of p(ω) ∝ L(ω) requires the tabulated weight to be L·sinθ. That factor is
exactly what the builder writes, with θ taken at the row centre,
θ_y = π(y + ½)/H, and L the Rec.709 luminance of the texel:

{{cite ohao/render/rt/env_cdf.cpp "float w = luminance(pixels[base], pixels[base + 1], pixels[base + 2]) * sinTheta;"}}

Each row CDF is normalised by its own row sum S_y and the marginal by the grand
total T, so the probability of a texel is the product of one step from each
table, and the row sums cancel:

$$P(x,y) \;=\; \frac{w_{x,y}}{S_y}\cdot\frac{S_y}{T} \;=\; \frac{w_{x,y}}{T}
\qquad p_{uv} = P\,W H, \qquad p_\omega = \frac{p_{uv}}{2\pi^2\sin\theta} \;\approx\; \frac{L_{x,y}}{T}\cdot\frac{WH}{2\pi^2}$$

The sinθ inserted at build time is cancelled by the Jacobian at sample time
(exactly at the row centre, to within the row's variation of sinθ elsewhere in
it), leaving a solid-angle density proportional to luminance. T is absent from
that chain: both tables are already normalised, so sampling never needs the
integral. The host still sends it as `tuning.z`, and no shader reads it:

{{cite ohao/render/rt/path_tracer.hpp "glm::vec4 tuning;               // x=fireflyClamp, y=envCDFHeight, z=envIntegral, w=unused"}}

## Two binary searches per sample

Sampling is a `lower_bound` over the marginal to choose a row, then a second one
over that row's conditional to choose a column:

{{cite shaders/rt/path_tracer.slang "const uint x = searchCdf(envConditional, y * W, W, rng.next());"}}

For a 4096×2048 HDRI that is 11 plus 12 dependent, warp-divergent loads to locate
the texel, once per vertex of the path. The direction is then placed uniformly
inside the chosen texel in (u, v), so the strategy covers the whole texel rather
than a grid of fixed directions:

{{cite shaders/rt/path_tracer.slang "const float u = (float(x) + rng.next()) / float(W);"}}

The four uniforms — row, column and the point inside the texel — are one 4-D point of
the path's Sobol sequence (the vertex's environment group, *Owen-scrambled Sobol*), so
they are jointly stratified across a pixel's samples:

{{cite shaders/rt/path_tracer.slang "// bounce d's light choice and point in group 1 + 3d, its environment sample in 2 + 3d, its BSDF"}}

## One density for both sides of a weight

`envPdf` turns any direction back into that density: it bins the direction into
its texel, differences the two tables (four further loads) and divides by the
Jacobian at the direction's own sinθ, clamped to 1e-4 so a texel that subtends
almost nothing at a pole cannot run the density away:

{{cite shaders/rt/path_tracer.slang "return max(cond * marg * float(W) * float(H) / (kTwoPi * kPi * max(sin(theta), 1e-4)), 0.0);"}}

That is the exact solid-angle density of `sampleEnv`'s sampling: piecewise
constant in (u, v), divided by the true Jacobian at ω. The same CDF answers
"which texel" and "how likely", so there is no separate PDF image to keep in sync.
Next-event estimation calls `envPdf` on the direction it drew, and a bounce ray
that escapes calls it on the direction the BSDF drew, so both halves of the power
heuristic's weight come from one function and sum to one:

{{cite shaders/rt/path_tracer.slang "const float w = depth == 0u ? 1.0 : powerHeuristic(lastPdf, envPdf(dir));"}}

## One latitude convention: the file's

The sampler and the density measure latitude from the zenith, so row 0 is
straight up, as `stbi_loadf` hands the file's first row over (θ = acos y in
`envPdf`). The radiance lookup, and every other lookup of the environment
(deferred lighting, the prefiltered and irradiance images, RT GI's escaped rays,
the forward and particle passes), writes
v = ½ − asin(y)/π, which is the same number, since acos(y)/π = ½ − asin(y)/π:

{{cite shaders/rt/path_tracer.slang "const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / kPi);   // row 0 the zenith"}}

The unit tests cannot see a mismatch here: the sinθ weight is mirror-invariant
(sin θ = sin(π − θ)), and they assert only that the CDFs are monotone, normalised,
and concentrate around a hot texel, all true of a mirrored table. The convention is
held by a frame instead: `check_env_orientation.py` lights a white floor with an
environment blue above the horizon and orange below, and every renderer must light
it blue.

{{cite tests/renderer/env_cdf_test.cpp "TEST(EnvCDF, HotSpotConcentratesCDF) {"}}
{{cite tests/renderer/check_env_orientation.py "MIN_RATIO = 1.5"}}

:::key
The firefly clamp is the one biasing operation near this sampler, and it is not
in it: the path's whole radiance is scaled down to the `tuning.x` luminance
ceiling, once, and only when the host sets the flag. The host sets it only for a
profile that asks for it and whose trace does not accumulate (a temporal denoiser
fed one sample a frame); a running mean converges without it.
:::

{{cite shaders/rt/path_tracer.slang "if (lum > pc.tuning.x) radiance *= pc.tuning.x / lum;"}}
{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.enableFireflyClamp && !tracerAccumulates(m_renderSettings))"}}

## What happens with no environment map

Bindings 17 and 18 are declared unconditionally in the RT descriptor layout, so
something must be bound there in every scene, including a Cornell box with no
HDRI at all.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "add(17, BUFFER, RGEN | MISS);   // env marginal CDF"}}

:::why
Rather than compile a second pipeline for the no-env case, the upload path binds a
one-float dummy and signals "no environment" out of band: width and height go out
as zero in the push constants, and the shader gates every environment sample on
`envSampled()`, which checks the width, the height and the environment's texture
index. One pipeline, one layout, two four-byte allocations.
:::

{{cite ohao/gpu/vulkan/env_upload.cpp "const float v = 1.0f; std::memcpy(dp, &v, sizeof(float));"}}
{{cite shaders/rt/path_tracer.slang "bool envSampled() { return envTexture() < 4096u && pc.control.w > 0u && pc.tuning.y > 0.0; }"}}

The builder has a second degenerate case. A completely black row sums to zero, so
its conditional CDF would be all zeros; the builder fills it with a uniform ramp
instead:

{{cite ohao/render/rt/env_cdf.cpp "// Degenerate black row — fall back to uniform"}}

In a map with any light in it that ramp is unreachable. `searchCdf` returns the
lowest index whose entry reaches u, and a zero-sum row's marginal entry is the
same running total over the same grand total as its predecessor's — identical
bits — so for any u > 0 the search resolves to the earlier row and never lands on
the black one.

It earns its keep in exactly one case: a map whose *every* row is black. The
marginal then falls back to a uniform ramp too, every row becomes reachable, and
the two ramps compose into uniform sampling in (u,v) — the correctly normalised
density for that sampling. Without the row fallback a row search would end at the
last column with a zero step, and `envPdf` would be 0 for every sample.

## Where the tables live

The HDR image is staged through a transfer buffer into `DEVICE_LOCAL` memory. The
CDFs are not: they are allocated `HOST_VISIBLE | HOST_COHERENT`, mapped,
memcpy'd, and left there for the GPU to binary-search in place.

{{cite ohao/gpu/vulkan/env_upload.cpp "void* dp = nullptr; vkMapMemory(m_device, outMem, 0, sz, 0, &dp);"}}

For a 4096×2048 map the conditional table alone is 8.4 M floats — 32 MiB — of
randomly-accessed data on a heap whose residency depends on which memory type the
driver reports first for those flags.

The branch that builds them fires whenever the requested env path differs from the
loaded one. It first idles the graphics queue and destroys the previous env image,
because frames in flight may still sample it, and `destroyEnvCDFBuffers` frees the
previous CDF pair before the new one is uploaded — so switching HDRIs at runtime
replaces both tables rather than leaking them. The same two calls run at shutdown.

## Contracts

- Bindings 17/18 are re-written from `m_envMarginalCDFBuffer` / `m_envConditionalCDFBuffer` on every `PathTracer::render`, but the write is skipped when the handle is null. The dummy pair `env_upload.cpp` allocates is what keeps a scene that never loaded an environment from dispatching against unwritten descriptors.
- Width and height travel in push constants (`control.w`, `tuning.y`), not in the buffers. A width of 0 means "skip environment sampling entirely", not "the map is 0 wide". The integral (`tuning.z`) is sent and read by no shader.
- `sampleEnv` and `envPdf` share the tables, the acos latitude and the texel binning; both sides of every environment MIS weight call `envPdf`. A new strategy that samples the environment must report its density through the same function, or the weights stop summing to one.
- The CDF is rebuilt only when the environment *path* changes, so `envIntensity` relighting never touches it — correctly, since a uniform scale leaves the normalised distribution identical.
