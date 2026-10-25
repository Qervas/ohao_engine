---
module: shaders
id: compute
title: Compute shaders
standard: v2
---

## Two compute shaders under compute/, and the target that builds them

`add_custom_target(shaders ...)` is declared without `ALL`, so building some other
target alone compiles no SPIR-V. Every target that loads SPIR-V depends on it — the
`ohao` CLI, the examples, `render_bench` and the rendering tests — so building any of
those builds the shaders first.

{{cite shaders/CMakeLists.txt "add_custom_target(shaders"}}

{{cite examples/CMakeLists.txt "add_dependencies(${name} shaders)   # every example renders: never with stale SPIR-V"}}

GLSL compute files are compiled by a glob; Slang files by a second one, one SPIR-V per
`[shader(...)]` entry point:

{{cite shaders/CMakeLists.txt "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"}}

{{cite shaders/CMakeLists.txt "file(GLOB_RECURSE SLANG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.slang")"}}

## SSAO: the one on the raster path

SSAO is now Slang, `compute/ssao.slang`, with two entry points, so it compiles to two
files, `compute_ssao.slang.occlusion.spv` and `compute_ssao.slang.blur.spv`, which
`SSAOPass` loads by entry name:

{{cite ohao/render/deferred/ssao_pass.cpp "!rtc::createComputePipeline(m_device, "compute_ssao.slang.blur.spv", "blur", m_blurSetLayout,"}}

Both run 8×8 workgroups over the half-resolution image, and the host's
`(N + 7) / 8` group count agrees. Its push block is `SSAOPush`, generated from
`shaders/shared/layout.json` into Slang and C++ alike, and its sixteen noise rotations
travel in that block rather than in a texture. The occlusion entry decodes the
G-buffer's octahedral normal with its own copy of the decoder, which undoes the
`* 0.5 + 0.5` remap `gbuffer.frag` applies at its call site:

{{cite shaders/compute/ssao.slang "const float2 f = e * 2.0 - 1.0;"}}

{{cite shaders/core/gbuffer.frag "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

The shared GLSL encoder of the same name in `includes/common/encoding.glsl` returns
[-1,1] — its doc comment claiming [0,1] is wrong — so the round trip survives only
because `gbuffer.frag` redefines it locally. "De-duplicating" it onto the shared
helper breaks the pairing silently, for SSAO, SSR and lighting at once.

It is off unless asked for:

{{cite ohao/render/deferred/post_processing_pipeline.hpp "bool m_ssaoEnabled{false};"}}

The model viewer, the `ohao_game` host, a live window session and a capture whose
`post` list names `ssao` ask; the Python bindings expose the setter by name.

{{cite examples/model_viewer.cpp "pp->setSSAOEnabled(true);"}}
{{cite tests/python/renderer_bindings.cpp "set_ssao_enabled"}}

The kernel — a horizon search in the surface's tangent plane, eight directions of four
steps from world-space G-buffer positions, then a 4×4 depth-aware blur — and its
effect on the frame are the deferred SSAO unit's subject.

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

- The `shaders` target is not in `ALL`; only the targets that declare `add_dependencies(... shaders)` rebuild SPIR-V before they build, and a new target that loads SPIR-V must declare it too. Compilation is by glob, so adding a `.comp` or `.slang` yields SPIR-V and no wiring: a loader must ask for the path-flattened name, `compute/<name>.comp` → `compute_<name>.comp.spv`, and a Slang module with several entry points `compute_<name>.slang.<entry>.spv`.
- `compute_dlss_tonemap.comp.spv` is named only from inside `#ifdef OHAO_DLSS_ENABLED`. Without `external/DLSS/` at configure time the pipeline is never created, so any claim that two compute shaders reach a frame is a claim about a machine with the NGX SDK vendored in.
