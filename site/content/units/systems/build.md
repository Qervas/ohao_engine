---
module: systems
id: build
title: Build system
standard: v2
figures: [systems-build-shader-artifact-path]
---

## Ten archives and one edge nobody declared

`ohao/CMakeLists.txt` is ten `add_subdirectory` lines and a comment; each leaf
directory globs its own sources into a static library. The *declared* dependency
graph is a clean DAG — `ohao_runtime` → `ohao_ecs` → `ohao_renderer` →
`ohao_gpu_vulkan` → `ohao_scene` → `ohao_core`, with `ohao_api` sitting on
`ohao_ecs` and `ohao_reflect` beneath both. The *reference* graph is not.
`VulkanRenderer`, compiled into `ohao_gpu_vulkan`, owns a
`std::unique_ptr<DeferredRenderer>` and calls through it every frame, while
`DeferredRenderer` is compiled into `ohao_renderer`:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setRenderScene(m_renderScene.get());"}}

That back edge is never told to CMake. A GNU static archive is scanned once, in the
order it appears on the link line, and by the time `ld` reaches
`libohao_gpu_vulkan.a` it has already discarded the `ohao_renderer` members it now
needs. The examples and the standalone `renderer_test` therefore hand the linker the
whole set as one re-scannable group instead of an ordered list:

{{cite examples/CMakeLists.txt "-Wl,--start-group ohao_ecs ohao_renderer ohao_gpu_vulkan"}}

The MSVC branch of the same function lists the archives without a group and takes
`/FORCE:MULTIPLE` where the GNU branch takes `--allow-multiple-definition`:

{{cite examples/CMakeLists.txt "target_link_options(${name} PRIVATE /FORCE:MULTIPLE)"}}

The workaround is copied per call site rather than expressed once as a dependency, and
the copies have drifted. `tests/shadow_system` (off by default) links `ohao_renderer`
and `ohao_scene` in plain order with no group at all, and on the GNU branch
`tests/engine`, `tests/physics` and `tests/force_generators` open a group they never
close.

{{cite tests/shadow_system/CMakeLists.txt "target_link_libraries(shadow_system_tests PRIVATE"}}

The second link flag is doing quieter damage. `STB_IMAGE_IMPLEMENTATION` is expanded
with external linkage in two translation units that land in two different archives
— the bindless texture uploader in `ohao_gpu_vulkan` and the GLTF loader in
`ohao_scene` — so `stbi_load` and friends are defined twice before the executable's
own object file is even considered.

`stb_image_write` duplicates the same way and is the more immediate per-executable
trigger: `model_gltf.cpp` expands it inside `ohao_scene`, and so does every
`examples/*.cpp` main and `tests/renderer/renderer_test.cpp` — a second
`stbi_write_png` in the executable's own object file. Exactly one engine TU avoids the
collision, and it does so by hand: the FBX loader prefixes `STB_IMAGE_STATIC`, which
gives its copy internal linkage.

{{cite ohao/scene/asset/model_fbx.cpp "#define STB_IMAGE_STATIC"}}

:::why
Both are papered over at link time rather than fixed at the source: `--start-group`
re-scans until fixpoint instead of declaring the edge, and
`-Wl,--allow-multiple-definition` silences the duplicate-symbol error instead of
isolating stb into one TU. The first costs a few link passes. The second costs the
diagnostic itself, for the whole executable — any genuine ODR collision introduced
anywhere in the engine now links silently, resolving to whichever definition the
linker saw first.
:::

{{cite examples/CMakeLists.txt "target_link_options(${name} PRIVATE -Wl,--allow-multiple-definition)"}}

The same trade appears in one more place. NRD's NRI backend carries its own build of
the Vulkan Memory Allocator with the same global symbols, and under
`/FORCE:MULTIPLE` whichever object the linker pulls first wins. `ohao_gpu_vulkan` marks
`vmaCreateAllocator` as a required symbol on its *interface*, so every consumer pulls
OHAO's object before NRI's regardless of library order:

{{cite ohao/gpu/vulkan/CMakeLists.txt "target_link_options(ohao_gpu_vulkan INTERFACE /INCLUDE:vmaCreateAllocator)"}}

## Optional SDKs, and the one that changes a class layout

Two heavy NVIDIA dependencies are `option()`-gated. NRD defaults ON; DLSS defaults
to whether the vendored NGX static wrapper is present on disk, so a fresh clone
without `external/DLSS/` configures cleanly with the feature off.

{{cite CMakeLists.txt "set(OHAO_DLSS_DEFAULT ON)"}}

Both use the same guard shape: the `.cpp` stays in the source glob unconditionally
and its *body* is wrapped in an `#ifdef`, so an OFF build compiles an empty
translation unit rather than dropping a file from the target.

{{cite ohao/render/CMakeLists.txt "target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)"}}

That shape is safe for NRD, whose surface is functions. It is a trap for DLSS,
because `OHAO_DLSS_ENABLED` also gates *members* of a class that both archives see:

{{cite ohao/render/rt/path_tracer.hpp "std::unique_ptr<DlssRR> m_dlssRR;"}}

`sizeof(PathTracer)` depends on the macro. `ohao_gpu_vulkan` pulls `path_tracer.hpp`
in transitively through `renderer.hpp`, and the definition above is `PRIVATE`, so it
does not propagate across the target boundary. The macro is therefore defined a
second time, by hand, on the other library:

{{cite ohao/gpu/vulkan/CMakeLists.txt "if(OHAO_DLSS AND TARGET ngx_dlss)"}}

:::key
Deleting that three-line block in `ohao/gpu/vulkan/CMakeLists.txt` does not fail the
build. It produces two archives that disagree about the layout of a class they both
construct and destroy — the failure surfaces as heap corruption at runtime, in a
DLSS build, far from the cause.
:::

## What is actually optional

It is tempting to read the option list as "the heavy dependencies are all
switchable". The top-level file declares sixteen `option()`s, and fourteen of them are
test suites, the scene benchmark and the Python bindings; the only dependency
switches are `OHAO_NRD` and `OHAO_DLSS`. OpenImageDenoise is neither: it is looked up quietly, and its absence
only turns the OIDN backend into a stub. `ohao_renderer` defines `OHAO_OIDN_ENABLED`
only when the package was found, and the denoise entry point compiles to a warning and
a `false` return otherwise:

{{cite ohao/render/CMakeLists.txt "find_package(OpenImageDenoise QUIET)"}}

So a machine without OIDN configures and builds the whole renderer; it just cannot
denoise with OIDN, and the default offline profile — which asks for it — gets an
undenoised image. `glslc` is the opposite: `find_program` plus a fatal error, which
means you cannot configure even a C++-only build without the Vulkan SDK's shader
compiler on `PATH`:

{{cite shaders/CMakeLists.txt "glslc shader compiler not found"}}

Jolt, Assimp and EnTT are unconditional FetchContent; the `if(TARGET Jolt)` guards
scattered through `ohao/physics` and the example function are a fallback for a fetch
that produced no target, not a user-facing switch. "Bare build" in this tree means
*without the two NVIDIA denoisers* — nothing more.

## Surgery on a dependency's own compile options

NRD pulls in NRI, and NRI puts `-Werror` in `NRI_Shared`'s **PUBLIC** compile options,
so the flag travels down the interface to everything NRI touches. Appending
`-Wno-error` loses, because the interface `-Werror` is appended after it. The module
records the concrete trigger it was written for — `NRI_VK`'s bundled `vk_mem_alloc.h`
emitting `-Wimplicit-fallthrough` on the author's GCC 14; that is the reason on file,
not something this tree reproduces, which builds with GCC 16. The fix does not try to
predict the warning. It reaches into the already-created targets and strips the flag
out of their properties:

{{cite external/cmake/nrd.cmake "list(REMOVE_ITEM _opts "}}

That loop runs *after* `FetchContent_MakeAvailable(NRD)` and mutates targets that
already exist, so it is indifferent to module order. What the include order in
`external/CMakeLists.txt` buys is earlier and different: `nri.cmake` sets `NRD_NRI=ON`
plus NRI's trimmed Vulkan-only cache surface before NRD's fetch reads them.

{{cite external/cmake/nri.cmake "set(NRD_NRI  "}}

Get that order wrong and the failure is not a warning-as-error — there is no `NRI`
target at all, and configuration stops here:

{{cite external/cmake/nrd.cmake "is present but NRI target is missing"}}

{{cite external/CMakeLists.txt "include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/nri.cmake)"}}

## Shaders compile into a flat namespace

Twelve stage extensions are globbed recursively out of `shaders/`, and each path is
flattened by replacing directory separators with underscores before `.spv` is
appended:

{{cite shaders/CMakeLists.txt "SHADER_FLAT_NAME ${SHADER_REL}"}}

So `shaders/rt/pt_raygen.rgen` becomes `rt_pt_raygen.rgen.spv`, and the C++ hardcodes
exactly that mangled string as `PathTracer`'s default raygen. Moving a shader between
directories renames its SPIR-V and every hardcoded path stops resolving — with no
build error, because the flattening is a string operation the compiler never sees.

The glob has no exclusion filter: it compiles every stage file it finds under
`shaders/` — forty today — whether or not any C++ loads the result. A syntax error
in a shader nobody ships fails the whole `shaders` target.

## The include dependency that isn't tracked

Each compile command lists a set of `.glsl` headers as prerequisites, gathered by
this pattern:

{{cite shaders/CMakeLists.txt "/includes/**/*.glsl"}}

The same `DEPENDS` list also names the output *directory*, created by its own custom
command. Under the Makefile generators that prerequisite is written as the relative
path `shaders` — which is already the name GNU make has for the custom target — so the
generated rules list `shaders` as a prerequisite of a file inside `shaders`, and make
prints a *circular dependency dropped* line for each and ignores the edges. The
ordering constraint the `DEPENDS` was written to express is therefore not enforced
there; it survives only because the directory already exists by the time `glslc` runs.

Even where such an edge is honoured it would be inert: overwriting an existing file in
place does not advance its directory's mtime. The target is a genuine fixpoint — a
second `--target shaders` with nothing changed compiles nothing — which is exactly why
a missing include edge is dangerous rather than merely redundant.

The flat directory is never pruned. When a shader source is deleted or renamed, its
old `.spv` stays in the output directory: no rule produces it any more and none
removes it, and a flat namespace gives no signal that a name is unclaimed. A
hardcoded path that still names it keeps loading the stale module.

## Where the SPIR-V lands, and where the engine looks

The shader target is a plain `add_custom_target` with no `ALL`, so it is not a root
of the default build in its own right:

{{cite shaders/CMakeLists.txt "add_custom_target(shaders"}}

It is reached only because other targets name it as a dependency, which is also
what makes `cmake --build build` — the "build all" invocation — produce SPIR-V at
all. The one that is always configured is the `ohao` CLI, which the root file adds
unconditionally:

{{cite tools/ohao/CMakeLists.txt "add_dependencies(ohao shaders)"}}

Since sub-project 4 every target that loads SPIR-V adds the same edge: `renderer_test`
and the `*_gpu_test`s, `engine_tests` and `ecs_tests`, and every example through the
shared `ohao_example` function, so even a targeted `--target cornell_box` compiles the
shaders it is about to load.

{{cite examples/CMakeLists.txt "add_dependencies(${name} shaders)   # every example renders: never with stale SPIR-V"}}

The `bin/shaders/` prefix the C++ paths name *first* is written by exactly one rule
anywhere in the tree, and that rule hangs off the `shaders` target itself rather
than off a consumer — two consumers copying into one destination raced under `-j8`.
It resolves the runtime directory per generator: `<config>/bin/shaders` under the
multi-config Visual Studio generators, `bin/shaders` beside the executables under the
single-config ones.

{{cite shaders/CMakeLists.txt "COMMENT "Copying shader SPVs to runtime output directory""}}

A build of only `ohao_core` or another library asks for no shaders, and the mirror is
then whatever the last shader build left. What also rescues hand-run binaries is a three-entry search
list inside the SPV loader. All three entries are relative paths resolved against the
*working* directory; the second is the CMake output tree and the third the
Visual Studio per-config runtime tree:

{{cite ohao/render/rt/path_tracer_pipeline.cpp "build/shaders/"}}

Entry 2 is why `./build/cornell_box` works from the repo root on a single-config
build, and entry 3 is why `build/Release/cornell_box.exe` does on Windows — but only
when entry 1 misses, and entry 1 is not a fixed miss. It is the raw `bin/shaders/…`
string baked into `PathTracerShaderSet`, tried verbatim before any fallback:

{{cite ohao/render/rt/path_tracer.hpp "bin/shaders/rt_pt_raygen.rgen.spv"}}

A `bin/shaders/` directory in the working directory therefore outranks the build's
output. No rule writes a repo-root `bin/`, and a stray one is neither refreshed nor
swept away. The interesting failure is not "no shader found"; it is a silently stale
shader that outranks a fresh one.

{{figure systems-build-shader-artifact-path "Conceptual — the producer and consumer paths for one shader, with names and search order read from shaders/CMakeLists.txt, tests/renderer/CMakeLists.txt and path_tracer_pipeline.cpp. Not a captured build log."}}

## The build type is a default, not a setting

The top-level file sets a build type only when it has to. A single-config generator
given no `CMAKE_BUILD_TYPE` is defaulted to Debug; a multi-config generator such as
Visual Studio is left alone and picks the configuration at build time with `--config`.

{{cite CMakeLists.txt "set(CMAKE_BUILD_TYPE Debug)"}}

An explicit `-DCMAKE_BUILD_TYPE=Release` is honoured. The `-g -O0` appended to
`CMAKE_CXX_FLAGS_DEBUG` is added for non-MSVC compilers only, so it rides the Debug
configuration and nothing else. The consequence is the usual one for a default: a
stock single-config configure with no build type produces an `-O0` build, and any
wall-clock figure taken from it is an `-O0` figure. Name the configuration you are
measuring.

## Contracts

- `external/cmake/nri.cmake` must be included before `nrd.cmake`, or NRI's cache options are set after NRD's FetchContent has already consumed them.
- `OHAO_DLSS_ENABLED` must be defined on **both** `ohao_renderer` and `ohao_gpu_vulkan`. It is `PRIVATE` on each, so removing either definition silently desynchronises `sizeof(PathTracer)` between the two archives.
- The `shaders` target is not in `all`. It is built through the dependencies its consumers declare on it — the `ohao` CLI, every example and every test that renders — so building any of them builds it. A new executable that loads SPIR-V must add `add_dependencies(<target> shaders)`, and `python tools/check.py` builds the target before anything else regardless.
- Every source glob carries `CONFIGURE_DEPENDS` (sub-project 4): a new `.cpp`, header or shader under a globbed directory is picked up by the next build, which re-runs the configure step itself.
- The path tracer's SPV loader tries `bin/shaders/<name>` relative to the CWD *before* `build/shaders/` and `build/Release/bin/shaders/`. A leftover `bin/shaders/` in the working directory silently shadows every freshly compiled raygen, miss and hit shader.
- A new executable that links the engine must list `ohao_ecs` and `ohao_renderer` alongside the other archives (inside the group on GNU) and carry the matching multiple-definition option, or stb's duplicate symbols fail the link.
