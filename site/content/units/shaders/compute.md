---
module: shaders
id: compute
title: Compute shaders
standard: v2
---

## Two compute shaders, and a build that ignores them

Nothing in the build depends on the shaders. `add_custom_target(shaders ...)` is
declared without `ALL`, and no `add_dependencies` call anywhere in the tree names
it, so a default `cmake --build build` compiles no SPIR-V at all — `--target
shaders` is a second, manual step.

{{cite shaders/CMakeLists.txt "add_custom_target(shaders"}}

{{cite shaders/CMakeLists.txt "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"}}

## SSAO: the one on the raster path

`compute_ssao.comp.spv` is loaded by `SSAOPass` and dispatched from the deferred
render graph. Its `local_size 8×8` and `(N + 7) / 8` group count agree. It reads the GBuffer normal attachment — an octahedral pair in `.rg` of
an `R16G16B16A16_SFLOAT` target — through `decodeNormalOctahedron` in
`includes/common/encoding.glsl`, which opens by undoing a [0,1] remap:

{{cite shaders/includes/common/encoding.glsl "vec2 f = encoded * 2.0 - 1.0;"}}

The matching `* 0.5 + 0.5` is not in that include: it is applied at the call site
in `gbuffer.frag`, against an `encodeNormalOctahedron` that file redefines
locally. The shared encoder of the same name returns [-1,1] — its doc comment
claiming [0,1] is wrong — so the round trip survives only because `gbuffer.frag`
does not use the include. "De-duplicating" it onto the shared helper breaks the
pairing silently.

{{cite shaders/core/gbuffer.frag "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

It is off unless asked for:

{{cite ohao/render/deferred/post_processing_pipeline.hpp "bool m_ssaoEnabled{false};"}}

Three call sites ask — the model viewer, the renderer pipeline test, and the
Python bindings, which expose the setter by name.

{{cite examples/model_viewer.cpp "pp->setSSAOEnabled(true);"}}
{{cite tests/python/renderer_bindings.cpp "set_ssao_enabled"}}

The kernel is GTAO-shaped: eight slice directions — `m_sampleCount` defaults to 8
and nothing calls `setSampleCount` on this pass — each swept both ways for four
steps, so 64 depth taps per pixel.

{{cite ohao/render/deferred/ssao_pass.hpp "uint32_t m_sampleCount{8};"}}
{{cite shaders/compute/ssao.comp "int stepsPerDirection = 4;"}}

Each slice closes with Jimenez's analytic arc integral, which gives the visible
cosine-weighted solid angle between two horizons in closed form instead of
counting occluded samples:

$$a(h) = \tfrac{1}{4}\bigl(-\cos(2h - n) + \cos n + 2h\sin n\bigr)$$

$h$ is a horizon angle, $n$ the angle of the projected normal, both measured
inside the slice plane; the slice's occlusion is $a(h_1) + a(h_2)$.

{{cite shaders/compute/ssao.comp "float a = (-cos(2.0 * h1 - n) + cos(n) + 2.0 * h1 * sin(n)) / 4.0;"}}

The derivation requires $h_1$, $h_2$ and $n$ to share a frame anchored on the view
vector. This implementation does not: `n` is taken against `normalize(-viewPos)`,
but the horizon search maxes its cosine against the in-plane sweep direction
passed in as `viewDir`, and the back-sweep references the same `+dir` axis rather
than its negation.

{{cite shaders/compute/ssao.comp "float h = dot(normalize(diff), viewDir);"}}

What ships is a horizon-based heuristic with GTAO's closed form bolted on, not the
ground-truth integral the file header claims. The `bias` push-constant field is
declared and never read.

## dlss_tonemap: display space, behind a build gate

The fifth pipeline is the DLSS Ray Reconstruction display pass, dispatched after
RR writes its denoised HDR image:

{{cite ohao/render/rt/path_tracer_render.cpp "m_dlssRR->tonemap(cmd, m_dlssColorOutView, m_outputView, m_width, m_height);"}}

That call site is inside `#ifdef OHAO_DLSS_ENABLED`, and so is the whole of
`dlss_rr.cpp`, the only translation unit that names the `.spv`.

{{cite ohao/render/rt/path_tracer_render.cpp "#endif  // OHAO_DLSS_ENABLED"}}
{{cite ohao/render/rt/denoise/dlss_rr.cpp "#ifdef OHAO_DLSS_ENABLED"}}

The option defaults ON only when a vendored NGX static library is already on disk,
and `.gitignore` excludes the directory that would hold it — so a clean clone
compiles the file empty. The `.spv` is still produced by the glob; nothing loads
it.

{{cite CMakeLists.txt "set(OHAO_DLSS_STATIC_LIB"}}
{{cite .gitignore "external/DLSS/"}}

Where it does run, it takes `RGBA16F` linear to `RGBA8` with the same ACES curve
and the same 0.5 exposure prescale as `pt_raygen.rgen`, so switching denoisers
does not shift the tone curve underneath a comparison.

{{cite shaders/compute/dlss_tonemap.comp "vec3 ldr = ACES(hdr * 0.5);"}}

Everything after the curve — contrast, split-tone, saturation, vignette, grain —
is scaled by one push-constant master read from `OHAO_GRADE`, where 0.0 reproduces
the pre-cinematic output exactly. Chromatic aberration is the exception: it runs
*before* ACES, offsetting the R and B `imageLoad`s, so it resamples HDR radiance
rather than the graded image. Its offset is also truncated to whole texels, so it
snaps where the expression crosses one pixel rather than ramping.

{{cite shaders/compute/dlss_tonemap.comp "ivec2 off = ivec2(d * s * r2 * 6.0);"}}

None of it feeds back into the trace — the pass reads DLSS's `COLOR_OUT` and
writes the RGBA8 beauty — so the tracer stays unbiased.

## Contracts

- Nothing depends on the `shaders` target, so `cmake --build build` never recompiles SPIR-V and a stale `.spv` loads without complaint. `--target shaders` is a separate step. Compilation is then by glob, so adding a `.comp` yields a `.spv` and no wiring: a loader must ask for the path-flattened name, `compute/<name>.comp` → `compute_<name>.comp.spv`.
- `compute_dlss_tonemap.comp.spv` is named only from inside `#ifdef OHAO_DLSS_ENABLED`. Without `external/DLSS/` at configure time the pipeline is never created, so any claim that two compute shaders reach a frame is a claim about a machine with the NGX SDK vendored in.
