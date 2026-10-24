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
not any C++ loads it — 41 GLSL sources, 41 SPIR-V modules. Headers are the opposite
case: a `.glsl` file is compiled only as text spliced into a stage, so a header no
stage includes is never parsed at all.

The compile line is where the architecture actually shows:

{{cite shaders/CMakeLists.txt "COMMAND ${GLSLC} --target-env=vulkan1.3"}}

Two include roots — `shaders/` and `shaders/includes/` — and **zero `-D` flags**.
That omission decides how variation is expressed everywhere in the engine. A
shader cannot be compiled twice with different defines, so the three path-tracer
profiles are three separate source files (`pt_raygen.rgen`,
`pt_raygen_realtime.rgen`, `pt_raygen_offline.rgen`) rather than one file with a
`#define PROFILE`. Copying does not keep them in step, and they have not drifted
equally: against the 1270-line `pt_raygen.rgen`, `diff` reports 141 added or
removed lines for the 1355-line `pt_raygen_offline.rgen` and 1075 for the
1925-line `pt_raygen_realtime.rgen`. Two of the three are near-duplicates; the
realtime profile is a fork.

What stays configurable is pushed into SPIR-V specialization constants patched at
pipeline creation. There is exactly one `constant_id` in the entire shader tree —
the path tracer's sampler choice, in a header all three raygens include:

{{cite shaders/includes/rt/sampler_api.glsl "layout(constant_id = 0) const uint SAMPLER_TYPE"}}

It is supplied to the raygen stage alone, so changing samplers rebuilds the
pipeline object and not the SPIR-V:

{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pSpecializationInfo = &samplerSpecInfo;"}}

The cost is visible in `pt_raygen_realtime.rgen`, which keeps its original glossy
sampler, and the pdf that matches it, behind `#ifdef OHAO_CRUDE_GLOSSY` — a switch
nothing in the build ever supplies:

{{cite shaders/rt/pt_raygen_realtime.rgen "// A/B baseline: original crude cosine-scatter sampler (flicker source)."}}

Both `OHAO_CRUDE_GLOSSY` branches are therefore never compiled. They are
not dead by deletion, they are dead by there being no `-D` in the build — and
because they are never compiled, nothing checks that they still would.

:::why
The obvious alternative — a define-based permutation system, which is where every
large engine ends up — is rejected here in favour of file-level duplication plus
specialization constants. The trade is real: duplicated raygen files drift, so a
fix landed in one profile does not reach the other two. What is bought is that
there is no permutation explosion and no cache key to invalidate — each compile is
one source file in, one `.spv` out, with no defines to reconstruct. The converse
does not hold, because nothing prunes: a shader that is moved or deleted leaves
its old module in `build/shaders/` until someone removes it by hand.
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
`rt_shadow_trace.slang.spv`, loaded with entry `"trace"`):

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

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_pt_raygen.rgen.spv"};"}}

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

{{cite ohao/gpu/vulkan/renderer.cpp "std::ifstream test(path + "core_deferred_lighting.frag.spv");"}}

and then publishes whatever it found to every render pass through a static:

{{cite ohao/gpu/vulkan/renderer.cpp "RenderPassBase::setShaderBasePath(m_shaderBasePath);"}}

The sentinel is the deferred lighting module, so the discovery key is at least a
shader the pipeline it configures actually loads. The probe assigns only on a hit,
but the publish is unconditional: if none of the seven candidates holds
`core_deferred_lighting.frag.spv`, an empty string is pushed, and `RenderPassBase`'s own default prefix is overwritten rather than left
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

Touching `includes/common/math.glsl` therefore recompiles all 40 modules, including
the 39 whose include closure never reaches it — a deliberate over-approximation that avoids depfile plumbing. The
under-approximation next to it is the dangerous half. `SHADER_INCLUDES` is built
from a pattern with a directory component in the middle:

{{cite shaders/CMakeLists.txt ""${CMAKE_CURRENT_SOURCE_DIR}/includes/**/*.glsl""}}

## Two files named pbr_unpack.glsl

The tree contains `shaders/includes/pbr_unpack.glsl` and
`shaders/rt/includes/pbr_unpack.glsl`, byte-identical today. All three raygens
ask for the same string:

{{cite shaders/rt/pt_raygen.rgen "includes/pbr_unpack.glsl"}}

A quoted include is resolved relative to the *including file's* directory before
any `-I` root is consulted, and `shaders/rt/` has an `includes/` of its own — so
`glslc -M` reports that the raygens compile `shaders/rt/includes/pbr_unpack.glsl`
and never touch the copy under `shaders/includes/`. Neither the include roots nor
the flat naming rule are involved; the winner is decided by directory adjacency.

:::key
Combine the two facts above and you get a silent-failure machine: edit the
top-level `includes/pbr_unpack.glsl` and (a) it is the wrong copy — the raygens
read the `rt/includes/` one — and (b) even editing the right copy triggers no
recompilation, because neither file is in the dependency glob. The build will
report success and the SPIR-V will be unchanged.
:::

## Contracts

- The `shaders` target is declared without `ALL`, so it is not a root of the default build in its own right. It is pulled in through `add_dependencies` from every target that loads SPIR-V — the `ohao` CLI, the examples, `renderer_test`, `engine_tests`, `ecs_tests` and the `*_gpu_test`s — so building any of them recompiles shaders; `python tools/check.py` builds it first explicitly. `--target shaders` remains the way to say it directly. {{cite shaders/CMakeLists.txt "add_custom_target(shaders"}}
- The mirror from `build/shaders/` into `bin/shaders/` is owned by the `shaders` target itself, not by its consumers. Two consumers each attaching their own POST_BUILD `copy_directory` into that one destination raced under `-j8`; the single copy also closes the ordering gap where a consumer's mirror could snapshot SPIR-V from before the shaders were rebuilt. {{cite shaders/CMakeLists.txt "add_custom_command(TARGET shaders POST_BUILD"}}
- Nothing deletes stale outputs. A renamed or moved shader leaves its old `.spv` in `build/shaders/`, where the runtime's basename fallback will happily load it.
- Every shader path in the engine is CWD-relative, and on a single-config build exactly two working directories resolve: the repository root (via the `build/shaders/` + basename rung) and `build/` (via the `bin/shaders/` literal, served by the `shaders` target's mirror). They are not equivalent for anything else — `model_viewer`'s scene names its environment map as a project-relative asset, which the example's `FileAssetResolver` resolves against the current directory, and the file exists only under the source tree, so that example still has to run from the root. {{cite examples/example_scenes.hpp "addEnvironment(world, ecs::AssetRef{"assets/test_models/env_studio.hdr", {}});"}}
- Renaming a shader file, or moving it between directories, renames its SPIR-V and breaks every C++ load site that spells the flat name. The build reports success either way.
