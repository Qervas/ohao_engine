---
module: shaders
id: compute
title: Compute shaders
standard: v2
---

## Compute shaders, and the target that builds them

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

## SSAO on the raster path

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

{{cite shaders/includes/scene/gbuffer_body.glsl "vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;"}}

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

## light_cluster: the froxel light lists

`compute/light_cluster.slang` has one entry point, `build`, so it compiles to
`compute_light_cluster.slang.spv`, which `LightClusters` loads by that name:

{{cite ohao/render/deferred/light_clusters.cpp "if (!rtc::createComputePipeline(m_device, "compute_light_cluster.slang.spv", "build", m_setLayout,"}}

It runs one 64-thread workgroup per froxel, 3,456 workgroups in a one-dimensional
dispatch, and imports its `LightGpu` record, its `ClusterPush` block and every
`kCluster*` constant from the generated `shared.layout` module, the same description
`deferred_lighting.frag` reads through `shared/layout.glsl`. The kernel and why its lists
leave the lit image unchanged are the deferred clustered-lighting unit's subject.

{{cite shaders/compute/light_cluster.slang "[numthreads(kClusterGroupSize, 1, 1)]"}}

## cull/: the draw lists and the depth pyramid

GPU culling's modules sit in their own directory, `shaders/cull/`, and compile by
the same Slang glob. `instances.slang` has five entry points, so five files, which
`GpuCulling` loads by entry name with one shared set layout and push range:

{{cite ohao/render/deferred/gpu_culling.cpp "{"cull_instances.slang.classifyMeshlets.spv", "classifyMeshlets", &m_classifyMeshlets},"}}

`classify` and `scatter` run one thread per draw in workgroups of `kCullGroupSize`
(256), `classifyMeshlets` and `scatterMeshlets` (the mesh-shader fallback) one per
meshlet entry, and `scanBlocks` a single workgroup of the same size; the host
dispatches `(items + 255) / 256` groups. `common.slang` has no entry point: it is the
module of culling tests that `instances.slang` and `meshlets.slang` both import.
`meshlets.slang` is not compute: its task, mesh and pull entries are the G-buffer's
meshlet geometry stages, loaded by `GBufferPass`. `hiz.slang`'s `fromDepth` and `downsample` build the
max-depth pyramid in 8×8 workgroups, one dispatch per level. Their push blocks,
`CullPush` and `HiZPush`, and the `DrawRecord` they read come from
`shaders/shared/layout.json`. The algorithm — phases, prefix-sum compaction and the
occlusion test — is the GPU culling unit's subject.

{{cite shaders/cull/instances.slang "[[vk::push_constant]] ConstantBuffer<CullPush> pc;"}}

## dlss_tonemap: display space, behind a build gate

The last is the DLSS Ray Reconstruction display pass, dispatched after
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
- `compute_dlss_tonemap.comp.spv` is named only from inside `#ifdef OHAO_DLSS_ENABLED`. Without `external/DLSS/` at configure time the pipeline is never created, so any claim that it reaches a frame is a claim about a machine with the NGX SDK vendored in.
