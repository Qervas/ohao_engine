---
module: architecture
id: module-map
title: Module ownership map
standard: v2
figures: [architecture-module-map-link-graph]
---

## Two graphs, and only one cycle

There are two dependency graphs in this tree and they do not agree. The declared
one — what `target_link_libraries` says — is a layering: core at the bottom, scene
above it, GPU above that, render above that, the ECS above render, the runtime on
top, physics and audio hanging off the side. The real one, the `#include` graph,
has a single cycle in it between GPU and render. Every example in the repo admits
this, in a single line:

{{cite examples/CMakeLists.txt "-Wl,--start-group ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core -Wl,--end-group"}}

`--start-group` tells GNU ld to re-scan the enclosed archives repeatedly until no
new undefined symbol appears. A left-to-right single pass — the default, and all a
genuine DAG needs — would leave symbols unresolved here. The MSVC branch of the
same function lists the archives without a group and adds `/FORCE:MULTIPLE`;
`link.exe` resolves archive members without caring about order, so the cycle costs
it nothing visible. On the GNU side the flag is not a build quirk; it is the
load-bearing evidence for what this page is about.

{{figure architecture-module-map-link-graph "Conceptual. Solid edges are read from the target_link_libraries calls in each module's CMakeLists.txt; dashed edges are #include lines that have no corresponding link edge. Third-party edges (Jolt, miniaudio, Vulkan, VMA, assimp, EnTT, GLFW) and transitive core and scene edges are omitted. Not a measured link-time symbol graph."}}

## Ten archives, and a directory that is not one

`ohao/CMakeLists.txt` adds exactly ten subdirectories, and ten archives come
out — but not one target per directory:

{{cite ohao/CMakeLists.txt "add_subdirectory(render)"}}

`ohao/gpu/` declares no library at all; its entire `CMakeLists.txt` is one
`add_subdirectory(vulkan)` line, and `ohao_gpu_vulkan` is defined a level down. Two
of the other nine are conditional rather than literal: `ohao/core/` degrades to an
`INTERFACE` library if its `.cpp` files ever disappear, and `ohao/render/` omits
the `STATIC` keyword, so `ohao_renderer`'s type follows `BUILD_SHARED_LIBS` — `OFF`
in this configuration, hence static in practice.

`ohao_core` is the true leaf: zero includes of scene, gpu, render, physics, audio
or Vulkan anywhere under `ohao/core/`. `ohao_audio` and `ohao_physics` are leaves
on the other side: they depend on core and third-party code only. Audio is core, glm
and miniaudio, plus `Threads::Threads`, `dl` and `m` on UNIX, and nothing under
`ohao/` includes an `audio/` header:

{{cite ohao/audio/CMakeLists.txt "Threads::Threads"}}

Physics is core, glm, nlohmann_json and Jolt, and its sources include nothing from
scene, gpu, render or ecs — a rigid body is addressed by its handle, not by a
component pointer. Its one client in the engine is the ECS's physics system
(`ecs/physics_scene.cpp`), which builds a level's colliders through the backend
interface; the edge points from the ECS down, never back. The physics archive is
where that module's mass is — it carries 194 distinct undefined `JPH::` symbols that
only Jolt resolves.

{{cite ohao/physics/CMakeLists.txt "target_link_libraries(ohao_physics PUBLIC Jolt)"}}

`ohao_reflect` sits beside core: the type registry, schema export and the JSON
reader and writer, with no engine dependency at all (glm, and fast_float privately).

The next three archives sit above the rest and add no edge back down. `ohao_ecs` is
the game world — an EnTT registry, the ordered systems (spin, transform, instantiate,
physics, the render extract), the scene codec and snapshots. It links core, the
renderer, scene, reflect and physics, and it is the only engine archive that sees
EnTT:

{{cite ohao/ecs/CMakeLists.txt "target_link_libraries(ohao_ecs PUBLIC ohao_core ohao_renderer ohao_scene ohao_reflect ohao_physics glm EnTT::EnTT"}}

Every `<entt/entt.hpp>` include in the engine sits under `ohao/ecs/` (and above it, in
`api/` and the tools); the renderer's side of the seam is `render::RenderScene`, a
plain-struct GPU world that knows nothing about registries. The ECS includes render
headers (`RenderScene`, `model_upload`, the camera framer) and calls into
`libohao_renderer.a`, and nothing in core, reflect, scene, gpu or render includes an
`ecs/` header, so the edge points one way.

`ohao_api` is the agent API — the `Session` method table, edit commands and queries —
over the ECS and reflect, and it never touches the GPU: rendering is an `api::Host`
that the host program installs with `Session::setHost`.

{{cite ohao/api/CMakeLists.txt "target_link_libraries(ohao_api PUBLIC ohao_ecs ohao_reflect ohao_unordered_dense)"}}

`ohao_runtime` — window, fixed-step clock, the live host's loop, the shared frame
capture — is the host layer. It links the API, core, the ECS, the level system (which a
host installs, so a `Level` component expands into entities), the GPU archive and GLFW:

{{cite ohao/runtime/CMakeLists.txt "target_link_libraries(ohao_runtime PUBLIC ohao_api ohao_core ohao_ecs ohao_level_world ohao_gpu_vulkan glm glfw"}}

Nothing else in `ohao/` includes a `runtime/` header. `ohao_game`, the CLI's
`ohao_host` and `runtime_tests` link it; `cornell_box`, `model_viewer` and
`scene_view_gpu_test` include only its header-only `scene_view_apply.hpp`.
`runtime::LiveHost` owns the window, the renderer, the `ecs::World` and an
`api::Session`, so the archive is the one place where all of those layers meet; it
adds a layer and no edge back down. The engine's cyclic surface is the pair of
archives below it, not the host.

## The one cycle: GPU calls up into render

`ohao_renderer` declares `ohao_gpu_vulkan` among its links, alongside scene and
core:

{{cite ohao/render/CMakeLists.txt "ohao_gpu_vulkan"}}

`ohao_gpu_vulkan` declares scene and core, never renderer:

{{cite ohao/gpu/vulkan/CMakeLists.txt "target_link_libraries(ohao_gpu_vulkan PUBLIC"}}

Yet 21 `#include` lines under `ohao/gpu/vulkan/` reach into `render/`, and the
deepest of them sits in the renderer's own header. `VulkanRenderer` keeps its two
RT profiles behind `std::unique_ptr`, which looks like insulation:

{{cite ohao/gpu/vulkan/renderer.hpp "std::unique_ptr<RTRealtimeRenderer> m_rtRealtimeRenderer;"}}

It is not. `renderer.hpp` includes `rt_profile_renderer.hpp` to declare those
types, and their shared base holds a `PathTracer` *by value*, so every
`ohao_gpu_vulkan` translation unit that sees `renderer.hpp` instantiates a complete
`PathTracer` layout:

{{cite ohao/render/rt/rt_profile_renderer.hpp "PathTracer m_pathTracer;"}}

That is not a header-only relationship. `render_dispatch.cpp` calls
`DeferredRenderer::setRenderScene`, `setCameraData` and friends, and the RT path
hands the `RenderScene` buffers to the profile through `setSceneBuffers`; their
definitions live in `libohao_renderer.a`. So `ohao_gpu_vulkan.a` carries 50
distinct undefined `ohao::` symbols that only `ohao_renderer.a` provides — while
`ohao_renderer` is listed *before* it on the link line and itself needs nine of the
GPU archive's symbols. That is precisely the shape a single pass cannot resolve,
and exactly why the group exists.

No other pair of archives forms a cycle. `ohao_scene` carries no undefined symbol
that another engine archive defines, and physics reaches nobody.
The `Material` header under `gpu/vulkan/` is still a Vulkan-free struct,

{{cite ohao/gpu/vulkan/material.hpp "struct Material {"}}

and `scene/asset/model.cpp` still includes it, but uses no `Material` member — the
loaders produce `MaterialData` — so the include is unused and adds no edge:

{{cite ohao/scene/asset/model.cpp "#include "gpu/vulkan/material.hpp""}}

Directory name is not ownership in this tree. A header living under `gpu/vulkan/`
that links to nothing is as easy to misread as one that does.

## The only Vulkan scene owns is a vertex layout

Other pages in this monograph state the rule "scene must not include vulkan.h". Two
files under `ohao/scene/` violate it — `asset/model.hpp` and the `.cpp` beside it —
and only the header propagates, so it is the one worth stating precisely:

{{cite ohao/scene/asset/model.hpp "#include <vulkan/vulkan_core.h>"}}

`Vertex` declares its own binding and attribute descriptions as static members, so
the vertex input layout — a pipeline object — is authored by the module that owns
the vertex struct rather than by the module that builds pipelines. Across all of
`ohao/scene/` — which holds only `asset/`, the model loaders and the primitive
mesh generator — that costs six mentions of a `Vk*` type and zero calls to any
`vk*` entry point: the loaders genuinely are Vulkan-free. That is a statement about
ownership, not about linkage — `ohao_scene` puts `${Vulkan_LIBRARIES}` in its
`PUBLIC` link list unconditionally, called or not.

{{cite ohao/scene/CMakeLists.txt "${Vulkan_LIBRARIES}"}}

`ohao_physics` still adds Vulkan's include directory while linking no Vulkan
library and containing not one `Vk` type or `vk` call of its own:

{{cite ohao/physics/CMakeLists.txt "${Vulkan_INCLUDE_DIRS}"}}

Nothing in physics includes a Vulkan header, so the line is vestigial: it enables
no dependency that physics has.

:::why
The alternative — describing the vertex layout in a GPU-side translation unit and
keeping scene header-clean — was not taken. What it buys is that the `offsetof`
assertions on `Vertex` sit next to the fields they guard, so reordering one of them
breaks the build in the file where the mistake was made rather than in a distant
pipeline TU. What it does not buy is coverage: only `position`, `color` and
`normal` are asserted in `model.hpp`, and `Vertex` never goes through
`OHAO_ASSERT_GPU_LAYOUT`. The total size is pinned elsewhere — `render_scene_types.hpp`
asserts `sizeof(Vertex) == 100` because the RT shaders read it as 25 floats — but a
reorder of `texCoord1`, `tangent`, `boneIndices` or `boneWeights` keeps the size
and compiles silently.
:::

## The ABI belongs to no module at all

Given three cycles, what stops this from collapsing into "everything includes
everything"? Not the directory layout. It is that the byte layout of anything
crossing to a shader is a contract checked at compile time in every TU that sees
the header. `ohao_core` owns the check:

{{cite ohao/core/concepts.hpp "#define OHAO_ASSERT_GPU_LAYOUT(Type, Bytes)"}}

It does not own the numbers. Those are constants in `ohao/gpu/layout_meta.hpp`,
deliberately separated from the renderer so a module can assert against the GPU ABI
without including `renderer.hpp`: 128 bytes for `LightData`, 80 for `GPULight`,
256 for the path tracer's push block, 48 for one material row.

{{cite ohao/gpu/layout_meta.hpp "static_assert(MaterialGpuPack::kBytes == 48,"}}

And that header belongs to no archive. `ohao/gpu/CMakeLists.txt` declares no target,
and `ohao_gpu_vulkan`'s recursive glob is rooted at `ohao/gpu/vulkan/`, so nothing
sitting directly in `ohao/gpu/` is a source of anything:

{{cite ohao/gpu/vulkan/CMakeLists.txt "file(GLOB_RECURSE GPU_VK_HEADERS"}}

`layout_meta.hpp` reaches its five consumers purely through the repo-wide
`-I ohao` path. That is the real ownership boundary,
and it is not a module. `GPULight` is declared in `render/rt/` and written by
`render/scene/light_table.cpp`; neither file owns the size — the 80-byte constant
does, and both fail to compile if it drifts.

## Private macros are where the boundary actually bites

A macro defined `PRIVATE` on one target is invisible to the other. Combined with
the GPU-includes-render cycle, that asymmetry becomes an ODR hazard rather than a
convenience. `OHAO_NRD_ENABLED` is private to `ohao_renderer`, but `path_tracer.hpp`
reaches `ohao_gpu_vulkan` twice over — `renderer.hpp` includes it directly, and
again through the by-value `PathTracer` inside `rt_profile_renderer.hpp`. If the
header wrapped its denoiser members in `#ifdef`, the two archives would compile two
different `PathTracer` layouts and the link group would happily join them:

{{cite ohao/render/rt/path_tracer.hpp "OHAO_NRD_ENABLED macro is PRIVATE to ohao_renderer"}}

The fix there was unconditional forward declarations. `OHAO_DLSS_ENABLED` took the
opposite one — it is defined on `ohao_gpu_vulkan` too — but layout parity is only
the second of its two reasons. The first is that the macro is live code in that
library: `device_setup.cpp` gates a `VK_KHR_get_physical_device_properties2` push
at instance creation and an NGX extension block at device creation on it.

{{cite ohao/gpu/vulkan/device_setup.cpp "DLSS-RR (NGX RayReconstruction) device extensions."}}

No NGX headers and no NGX link are needed for that — those are plain Vulkan
extension-name macros, and the NGX calls themselves stay in `ohao_renderer`.

{{cite ohao/gpu/vulkan/CMakeLists.txt "target_compile_definitions(ohao_gpu_vulkan PRIVATE OHAO_DLSS_ENABLED)"}}

:::key
The cheapest way to break this engine silently is to add an `#ifdef`-guarded data
member to a header that both `ohao_gpu_vulkan` and `ohao_renderer` include. It
compiles. It links — the archive re-scan cannot detect a layout mismatch. It
corrupts member offsets at runtime.
:::

## shaders/ is a build target, not a module

`shaders/` owns no library and no include relationship with C++; it is a custom
target that flattens each source path into one SPIR-V file, mangling the directory
separator into an underscore, so `shaders/rt/pt_raygen.rgen` becomes
`rt_pt_raygen.rgen.spv`:

{{cite shaders/CMakeLists.txt "SHADER_FLAT_NAME ${SHADER_REL}"}}

Two properties of that seam are worth knowing before editing a shader. First, the
`DEPENDS` list that marks SPIR-V stale globs `includes/**/*.glsl`, and CMake's glob
does not match `.glsl` files sitting *directly* in `shaders/includes/`:

{{cite shaders/CMakeLists.txt "includes/**/*.glsl"}}

Second, there are two independent answers to "where are the SPIR-V files". Raster
passes probe a candidate list at `VulkanRenderer` construction, using
`core_deferred_lighting.frag.spv` as the sentinel and then broadcasting the winner to
`RenderPassBase`:

{{cite ohao/gpu/vulkan/renderer.cpp "std::ifstream test(path + "}}

The RT pipeline ignores that result entirely — `PathTracerShaderSet` hardcodes
relative `bin/shaders/` paths, and `RTRealtimeRenderer` / `RTOfflineRenderer` pass
their own hardcoded variants:

{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv"}}

So the path tracer's shader lookup is working-directory sensitive in a way the
raster path is not, and the two can disagree about which build's SPIR-V they loaded.

## Contracts

- The archive list must stay inside `-Wl,--start-group`/`--end-group`. The one cycle (gpu ↔ render) needs the re-scan; removing the group breaks the GNU link, not the design.
- EnTT belongs to `ohao_ecs`. No header under `render/`, `gpu/`, `scene/` or `physics/` may include `<entt/entt.hpp>` or an `ecs/` header; the ECS reaches the renderer only through `RenderScene`'s plain-struct API.
- Any struct crossing to a shader asserts its size via `OHAO_ASSERT_GPU_LAYOUT` against a constant in `gpu/layout_meta.hpp`. Adding a field without updating both is a compile error by construction — keep it that way. `Vertex` is the standing exception: three field offsets asserted in `model.hpp` and a 100-byte size in `render_scene_types.hpp`, five fields unguarded by offset.
- No `#ifdef`-guarded data member may appear in a header included by both `ohao_gpu_vulkan` and `ohao_renderer`. Forward-declare, or define the macro on both targets.
- `ohao/scene/` may name `Vk*` types (vertex input descriptions only) but must not call any `vk*` entry point. This is an ownership rule, not a linkage one — `ohao_scene` links `${Vulkan_LIBRARIES}` either way.
- Editing a `.glsl` directly under `shaders/includes/` does not trigger shader recompilation. Touch a consuming stage or clean `build/shaders/`.
