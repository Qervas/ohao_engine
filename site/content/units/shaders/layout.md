---
module: shaders
id: layout
title: Shader tree layout
standard: v2
figures: [shaders-layout-resolution]
---

## One CMake file and no permutation system

The whole GLSL build is two `file(GLOB_RECURSE)` calls over `shaders/` — one
collecting sources, one collecting headers for dependency tracking — plus one
`glslc` command per matched source and a custom target that depends on the
results; Slang sources get a third glob of their own (below). There is no shader
database, no variant matrix, no reflection step.

{{cite shaders/CMakeLists.txt "file(GLOB_RECURSE SHADER_SOURCES"}}

The glob runs at configure time and, since sub-project 4, carries `CONFIGURE_DEPENDS`:
the build re-checks it and re-runs the configure step when a stage source is added or
removed, so a new `.rgen` is compiled by the next build. It also recurses without exclusions:
every stage source anywhere under `shaders/` is globbed and compiled, whether or
not any C++ loads it — 21 GLSL sources, 21 SPIR-V modules. Headers are the opposite
case: a `.glsl` file is compiled only as text spliced into a stage, so a header no
stage includes is never parsed at all.

The compile line is where the architecture actually shows:

{{cite shaders/CMakeLists.txt "COMMAND ${GLSLC} --target-env=vulkan1.3"}}

Two include roots — `shaders/` and `shaders/includes/` — and **zero `-D` flags**.
That omission decides how variation is expressed everywhere in the engine. A
shader cannot be compiled twice with different defines, so there are no variants of
a file: the path tracer is one Slang file, `rt/path_tracer.slang`, with four entry
points, and both RT profiles load the same four modules. What differs between them —
the bounce count, whether the trace keeps a running mean, the AOVs, the firefly clamp —
arrives at run time, in the push-constant block:

{{cite shaders/rt/path_tracer.slang "uint4 control;   // x flags, y frames already in the running mean, z motion vectors valid, w the environment CDF's width"}}

There are no SPIR-V specialization constants either: no shader in the tree declares
one, and the pipeline builder hands each stage only its module and its entry point's
name:

{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pName = m_shaderSet.raygenEntry;"}}

:::why
The obvious alternative — a define-based permutation system, which is where every
large engine ends up — is rejected here in favour of runtime flags. The trade is
real: every branch a flag selects is compiled into the one module and costs a
uniform branch at run time, and a feature that only one profile uses still has to
compile in both. What is bought is that there is no permutation explosion and no
cache key to invalidate — each compile is one source file in, one `.spv` out per
entry point, with no defines to reconstruct — and that a fix lands in every profile at
once. The converse does not hold, because nothing prunes: a shader that is moved or
deleted leaves its old module in `build/shaders/` until someone removes it by hand.
:::

## The flattening that became an ABI

Output names are the source path with directory separators replaced by
underscores, then `.spv` appended:

{{cite shaders/CMakeLists.txt "string(REPLACE "/" "_" SHADER_FLAT_NAME ${SHADER_REL})"}}

So `shaders/rt/rt_svgf_temporal.comp` becomes `rt_rt_svgf_temporal.comp.spv` —
the stutter is the directory name surviving into the filename. That mangled
string is not an internal detail; it is hard-coded at every C++ load site, which
makes the flattening rule a binary interface between the build system and the
renderer:

{{cite ohao/render/rt/denoise/atrous_denoise.cpp "rt_rt_svgf_temporal.comp.spv"}}

The mapping is not injective — `a/b.frag` and `a_b.frag` collide — and nothing in
the build detects that. No collision exists in the tree today, but the invariant
is maintained by nobody.

{{figure shaders-layout-resolution "Conceptual: how one source path becomes a flat SPIR-V name and how the loader finds it. Paths, the mirror step and the three-rung fallback ladder are read from shaders/CMakeLists.txt, tests/renderer/CMakeLists.txt and path_tracer_pipeline.cpp; the diagram is not a captured trace."}}

## Slang beside GLSL

Since sub-project 2b step 6 the same target also compiles Slang, and new shaders are
written in it. `slangc` is looked for in the Vulkan SDK before `PATH`, and the version
the SPIR-V was made with is pinned in `shaders/slangc.version`; another version only
warns, because it still builds, but may emit different code:

{{cite shaders/CMakeLists.txt "find_program(SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" ${OHAO_VULKAN_SDK_BINS})"}}

A `.slang` file is compiled once per entry point, and the entry points are found by
reading the source: a function under a `[shader("…")]` attribute line. One entry keeps
the flattening rule (`bench/gpu_calibrate.slang` → `bench_gpu_calibrate.slang.spv`),
several add the entry's name, and a file with none is a module that is only imported.
The entry keeps its own name in the SPIR-V, so the pipeline's `pName` is the
function's name, not `main` (`rt/shadow_trace.slang`'s `trace` becomes
`rt_shadow_trace.slang.spv`, loaded with entry `"trace"`; the reference path tracer's
four entries become `rt_path_tracer.slang.raygen.spv`, `.closestHit`, `.anyHit` and
`.miss`, which `PathTracerShaderSet` names with their entries):

{{cite shaders/CMakeLists.txt "-target spirv -profile spirv_1_6 ${SLANG_CAPS} -entry ${ENTRY} -fvk-use-entrypoint-name"}}

`SLANG_CAPS` is empty unless the source uses inline ray queries: a file whose text
contains `RayQuery<` is compiled with `-capability spvRayQueryKHR`, which the RT
shadow trace needs and nothing else in the tree asks for.

{{cite shaders/CMakeLists.txt "set(SLANG_CAPS -capability spvRayQueryKHR)"}}

`ohao doctor` scans entry points the same way, so `shaders.complete` expects the same
names and `shaders.fresh` follows `import` as it follows `#include`:

{{cite tools/ohao/doctor.cpp "// Slang: import a.b;  ->  a/b.slang"}}

GLSL and Slang share no source. What C++ and a shader must agree on — sizes, push
constants — lives in `shaders/shared/layout.json`, from which `tools/gen_layout.py`
writes a Slang module, a GLSL header and a C++ header that asserts every std430
offset; the `check_layout` suite fails while they are out of date:

{{cite shaders/bench/gpu_calibrate.slang "[numthreads(kCalibrateGroupSize, 1, 1)]"}}

## Where the SPIR-V is, and how each pipeline finds it

Every path is relative to the process working directory. There is no `install()`
rule for shaders anywhere in the project, no environment variable, no absolute
path, and no `.spv` committed to the repository — the binaries only ever exist in
a build tree.

The RT stack names its modules under `bin/shaders/`:

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};"}}

`glslc` writes `build/shaders/`, so that prefix looks stale — but the build does
still produce it. The `shaders` target mirrors the compiler's output directory
into a `bin/shaders` beside the executables,

{{cite shaders/CMakeLists.txt "-E make_directory "${OHAO_SHADER_RUNTIME_DIR}/bin/shaders""}}

and because the runtime output directory is the binary directory itself, that
destination is `build/bin/shaders/` (or `build/<CONFIG>/bin/shaders/` under a
multi-config generator):

{{cite CMakeLists.txt "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})"}}

The copy belongs to the `shaders` target rather than to each consumer because two
POST_BUILD `copy_directory` commands writing the same destination race under
`-j8`; the consumers only declare a dependency on it:

{{cite tests/renderer/CMakeLists.txt "add_dependencies(renderer_test shaders)"}}

Every example and every rendering test declares the same dependency, and
`renderer_test` is configured by default, so an ordinary configure-and-build
produces both trees:

{{cite CMakeLists.txt "option(BUILD_RENDERER_TESTS "Build renderer pipeline tests" ON)"}}

The loader's first candidate is the literal string, unmodified; its second strips
the directory and retries under `build/shaders/`:

{{cite ohao/render/rt/path_tracer_pipeline.cpp "std::vector<std::string> searchPaths = {"}}

So rung one hits when the process working directory is `build/`, and rung two hits
when it is the repository root. Two directories work, not one — and they are not
interchangeable. The mirror is a `copy_directory` snapshot taken when the `shaders`
target finishes, and `copy_directory` only ever adds, so the mirror accumulates its own stale
modules independently of the compile directory: the two can disagree in *both*
directions, each holding outputs the other does not.

The deferred stack is different again: `VulkanRenderer` probes seven
candidate directories at construction, testing each with a single sentinel file,

{{cite ohao/gpu/vulkan/renderer.cpp "std::ifstream test(path + "core_deferred_lighting.slang.spv");"}}

and then publishes whatever it found to every render pass through a static:

{{cite ohao/gpu/vulkan/renderer.cpp "RenderPassBase::setShaderBasePath(m_shaderBasePath);"}}

The sentinel is the deferred lighting module, so the discovery key is at least a
shader the pipeline it configures actually loads. The probe assigns only on a hit,
but the publish is unconditional: if none of the seven candidates holds
`core_deferred_lighting.slang.spv`, an empty string is pushed, and `RenderPassBase`'s own default prefix is overwritten rather than left
standing:

{{cite ohao/render/deferred/render_pass_base.hpp "static inline std::string s_shaderBasePath = "bin/shaders/";"}}

What the passes fall back to is not a per-pass guess: every deferred pass loads
through one `RenderPassBase::loadShaderModule`, whose three rungs are
`s_shaderBasePath + name`, a hard-coded `build/shaders/`, and a
`build/Release/bin/shaders/` for multi-config generators. With the base path
emptied the first rung degenerates to a bare flat filename — a hit only if the
working directory *is* the SPIR-V directory — so in practice the middle rung
carries every load, and only from the repository root:

{{cite ohao/render/deferred/render_pass_base.cpp ""build/shaders/" + pathStr"}}

The failure is loud but misdirected. The throw names the flat filename the pass
asked for and never the directory it searched, so a wrong working directory and a
genuinely missing module produce the same message:

{{cite ohao/render/deferred/render_pass_base.cpp "throw std::runtime_error("Failed to open shader file: " + pathStr);"}}

## The dependency edge that is missing

Every shader is declared to depend on the full include list, so touching one
header rebuilds the world:

{{cite shaders/CMakeLists.txt "DEPENDS ${SHADER} ${SHADER_INCLUDES}"}}

Touching `includes/scene/scene_data.glsl` therefore recompiles all 21 GLSL modules,
including those whose include closure never reaches it — a deliberate over-approximation that avoids depfile plumbing. The
under-approximation next to it is the dangerous half. `SHADER_INCLUDES` is built
from a pattern with a directory component in the middle:

{{cite shaders/CMakeLists.txt ""${CMAKE_CURRENT_SOURCE_DIR}/includes/**/*.glsl""}}

That pattern matches only headers at least one directory below `includes/`. Every
GLSL header in the tree sits there today — `includes/rt/nrd_frontend.glsl` and
`includes/scene/`'s two — so the list is complete, but a header placed directly in
`includes/` would be outside it.

:::key
A header outside the dependency glob changes nothing when edited: the build reports
success and the SPIR-V is unchanged until a shader that includes it is rebuilt for
another reason. Keep GLSL headers one directory down, or force the `shaders` target
after changing one.
:::

## Contracts

- The `shaders` target is declared without `ALL`, so it is not a root of the default build in its own right. It is pulled in through `add_dependencies` from every target that loads SPIR-V — the `ohao` CLI, the examples, `renderer_test`, `engine_tests`, `ecs_tests` and the `*_gpu_test`s — so building any of them recompiles shaders; `python tools/check.py` builds it first explicitly. `--target shaders` remains the way to say it directly. {{cite shaders/CMakeLists.txt "add_custom_target(shaders"}}
- The mirror from `build/shaders/` into `bin/shaders/` is owned by the `shaders` target itself, not by its consumers. Two consumers each attaching their own POST_BUILD `copy_directory` into that one destination raced under `-j8`; the single copy also closes the ordering gap where a consumer's mirror could snapshot SPIR-V from before the shaders were rebuilt. {{cite shaders/CMakeLists.txt "add_custom_command(TARGET shaders POST_BUILD"}}
- Nothing deletes stale outputs. A renamed or moved shader leaves its old `.spv` in `build/shaders/`, where the runtime's basename fallback will happily load it.
- Every shader path in the engine is CWD-relative, and on a single-config build exactly two working directories resolve: the repository root (via the `build/shaders/` + basename rung) and `build/` (via the `bin/shaders/` literal, served by the `shaders` target's mirror). They are not equivalent for anything else — `model_viewer`'s scene names its environment map as a project-relative asset, which the example's `FileAssetResolver` resolves against the current directory, and the file exists only under the source tree, so that example still has to run from the root. {{cite examples/example_scenes.hpp "addEnvironment(world, ecs::AssetRef{"assets/test_models/env_studio.hdr", {}});"}}
- Renaming a shader file, or moving it between directories, renames its SPIR-V and breaks every C++ load site that spells the flat name. The build reports success either way.
