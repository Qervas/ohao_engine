# ECS World + Persistent GPU Scene (Sub-project 2a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `Scene`/`Actor`/`Component` with an EnTT world whose renderable state is extracted incrementally into a persistent GPU render world (`RenderScene`) that the deferred passes and every RT pass read, so moving objects work everywhere and per-frame upload scales with change.

**Architecture:** `ohao/ecs` owns the registry, components and a fixed-order system list (`TransformSystem`, `RenderExtractSystem`). `ohao/render/scene` owns `RenderScene` — mesh arenas with one BLAS per mesh, a material table, an instance table with a free list, a light table, and a TLAS built or refit inside the frame's command buffer. The renderer never includes EnTT. Migration keeps the engine green at every step through a temporary bridge (old `Scene` → `RenderScene`) that lives from Task 7 to Task 9 and is deleted with the old scene code.

**Tech Stack:** C++20, Vulkan 1.3, EnTT v3.13.2, VMA (`GpuAllocator`), GLSL → SPIR-V via glslc, MSVC (Visual Studio 18 2026 generator).

**Spec:** `docs/specs/2026-10-03-ecs-gpu-scene-design.md`

## Global Constraints

- Build: `/c/Users/djmax/scoop/shims/cmake --build build --config Release -j8`; shaders are a separate target: `/c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release`. After deleting or renaming a shader: `rm -f build/shaders/*.spv; rm -rf build/Release/bin/shaders` before rebuilding shaders.
- Commit messages end with `Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN`. Work on branch `ecs-gpu-scene`; merging/pushing is the user's decision.
- "Old scene code: Deleted outright — no adapter." The Task 7–9 bridge is the only temporary code and is deleted in Task 9.
- "The renderer never includes EnTT": no file under `ohao/gpu` or `ohao/render` may include `entt/`. Checked by `grep -rn "entt/" ohao/gpu ohao/render` returning nothing.
- "Gameplay changes `Transform` through `registry.patch<Transform>` or `World::setTransform`."
- "One BLAS per mesh, built at registration, shared by every instance."
- "`instanceCustomIndex` = instance slot."
- "Raster keeps its 8-light limit in 2a; RT keeps `GPULight` (64 max)."
- "The vertex format stays today's 100-byte `Vertex` in 2a."
- "One bindless texture table (`TextureManager`, native resolution) for deferred, path tracer and RT GI."
- Zero validation messages: `python tests/validation/check_validation.py` and `OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200` (exit 0) at the end of every task that touches rendering.
- Never use `std::cout` in a per-frame path except behind a one-shot `static bool` guard.
- Python heredocs mangle backslashes in this shell: write edit scripts with the Write tool, or build backslashes with `chr(92)`.
- After restoring any file from a backup copy, `touch` it — MSBuild otherwise keeps objects built from the edited version.

## Review Focus

1. Destroying a parent entity with live children: the whole subtree is destroyed and removed from `RenderScene`; no entity keeps a dangling `Parent` — Task 2 `destroy_parent_destroys_subtree`, Task 6 `destroyed_subtree_releases_slots`.
2. An instance slot released and reused in the same frame: the TLAS rebuilds (not refits) and no draw uses the old mesh with the new slot — Task 5 `slot_reuse_forces_tlas_rebuild`.
3. An entity whose `MeshRenderer.materials` has fewer entries than its mesh has submeshes: the missing submeshes use material 0 (default), never read past the reference list — Task 4 `missing_material_refs_fall_back_to_default` (CPU packing) and the shader rule in Task 7/8.
4. `Transform` written via `registry.get<>` instead of `patch`: `ecs::verifyTransforms` reports it — Task 2 `unpatched_write_is_detected`.
5. Arena growth while earlier frames still read the old buffers: old buffers are retired only after `MAX_FRAMES_IN_FLIGHT` frames — Task 4 `arena_growth_defers_destruction`.

---

## File Structure

| Path | Responsibility |
|---|---|
| `external/cmake/entt.cmake` | FetchContent EnTT v3.13.2 (header-only target `EnTT::EnTT`) |
| `ohao/ecs/CMakeLists.txt` | static lib `ohao_ecs` (links `ohao_core`, `ohao_renderer`, `ohao_scene`, `glm`, `EnTT::EnTT`) |
| `ohao/ecs/components.hpp` | all ECS components and tags |
| `ohao/ecs/world.hpp/.cpp` | `World`: registry, create/destroy subtree, parenting, `setTransform`, ordered systems |
| `ohao/ecs/transform_system.hpp/.cpp` | world-matrix propagation, `prevWorld`, dirty tags, `verifyTransforms` |
| `ohao/ecs/render_extract.hpp/.cpp` | `RenderExtractSystem`: registry → `RenderScene` |
| `ohao/ecs/spawn.hpp/.cpp` | `Model` → `MeshData`/`MaterialDesc`/textures/emissive proxy; spawn helpers |
| `ohao/render/scene/handles.hpp` | `MeshHandle`, `MaterialHandle` (no dependencies) |
| `ohao/render/scene/render_scene_types.hpp` | `MeshData`, `SubmeshDesc`, `MaterialDesc`, `EmissiveProxy`, `LightDesc`, GPU structs + layout asserts |
| `ohao/render/scene/mesh_registry.hpp/.cpp` | vertex/index/triangle-slot arenas, mesh table, BLAS per mesh, growth + retire |
| `ohao/render/scene/material_table.hpp/.cpp` | material packs; index 0 = default |
| `ohao/render/scene/instance_table.hpp/.cpp` | slots, free list, dirty set, material reference lists |
| `ohao/render/scene/light_table.hpp/.cpp` | raster `LightData[8]` + RT `GPULight[64]` packing, emissive proxies |
| `ohao/render/scene/render_scene.hpp/.cpp` | owns the above + `GpuAllocator`, staging per frame slot, `recordUpdates`, TLAS decision/build, retire queue, stats |
| `ohao/render/rt/rt_acceleration_structure.*` | + `buildTLASFromDevice(cmd, instancesAddress, count, rebuild)` |
| `shaders/core/gbuffer.vert/.frag`, `shaders/shadow/shadow_csm.vert` | instance + material from storage buffers |
| `shaders/includes/scene/scene_data.glsl` | shared GLSL structs/bindings for instances, meshes, materials |
| `shaders/rt/pt_closesthit.rchit`, `pt_anyhit.rahit`, `rt_gi.rchit` | instance → mesh → triangle lookup |
| `tests/ecs/ecs_tests.cpp`, `tests/ecs/CMakeLists.txt` | CPU tests for the ECS |
| `tests/engine/engine_tests.cpp` | GPU tests for `RenderScene` (headless `VulkanRenderer`) |
| deleted in Task 9 | `ohao/scene/scene.*`, `actor/`, `scene_object.*`, `transform.*`, `component/` (all), `default_scene_factory.*`, `scene_module.hpp`; `ohao/physics/components/physics_component.*` (Task 3); `scene_upload.cpp`; actor loops in `rt_build.cpp`, `light_upload.cpp`, `buffer_setup.cpp` |

`ohao/scene/asset/` (Model, loaders, primitive generator) stays in `ohao_scene`.

---

### Task 1: Branch, deferred goldens, light-order record

**Files:**
- Modify: `tests/golden/render_golden.py` (add `--only <name>`)
- Modify: `tests/golden/manifest.json` (two deferred scenes)
- Create: `tests/golden/cornell_box_deferred.png`, `tests/golden/damaged_helmet_deferred.png`

**Interfaces:**
- Produces: manifest scenes `cornell_box_deferred`, `damaged_helmet_deferred`; `render_golden.py <manifest> --update --only <name>`.

- [ ] **Step 1: Branch**

```bash
git checkout -b ecs-gpu-scene
```

- [ ] **Step 2: Add `--only` to the golden runner**

In `tests/golden/render_golden.py` `main()`, after parsing the mode flag, accept an optional `--only NAME` and skip scenes whose `name` differs:

```python
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    ...
    for sc in manifest["scenes"]:
        if only is not None and sc["name"] != only:
            continue
```

Run: `python tests/golden/render_golden.py tests/golden/manifest.json --only cornell_box`
Expected: only `cornell_box` is checked, `[PASS]`.

- [ ] **Step 3: Add the deferred scenes to the manifest**

Append to `scenes`:

```json
{
  "name": "cornell_box_deferred",
  "command": "./build/cornell_box {out} 4 deferred --denoise=none",
  "golden": "tests/golden/cornell_box_deferred.png",
  "max_abs_diff": 4,
  "max_diff_frac": 0.01,
  "_note": "Deferred hybrid (G-buffer + RT shadows/GI where supported). Captured on master before sub-project 2a so the ECS/RenderScene migration is checked against the old renderer's own output."
},
{
  "name": "damaged_helmet_deferred",
  "command": "./build/model_viewer assets/test_models/DamagedHelmet.glb {out} 4 deferred --denoise=none",
  "golden": "tests/golden/damaged_helmet_deferred.png",
  "max_abs_diff": 4,
  "max_diff_frac": 0.01,
  "_note": "Deferred hybrid on the helmet; same purpose as cornell_box_deferred."
}
```

- [ ] **Step 4: Capture and prove determinism**

```bash
python tests/golden/render_golden.py tests/golden/manifest.json --update --only cornell_box_deferred
python tests/golden/render_golden.py tests/golden/manifest.json --update --only damaged_helmet_deferred
python tests/golden/render_golden.py tests/golden/manifest.json
```
Expected: four `[PASS]` (two RT, two deferred) on the second run. If a deferred scene fails against its own fresh golden, the deferred path is non-deterministic run-to-run: stop and report (stop condition — the gate would be meaningless).

- [ ] **Step 5: Record today's RT light order (not committed)**

Temporarily add, at the end of the light loop in `VulkanRenderer::uploadLightBuffer` (`ohao/gpu/vulkan/light_upload.cpp`), a one-shot print of each `GPULight.positionAndType` in order; run `./build/Release/cornell_box.exe /tmp/x.png 1 --denoise=none`; compare the order with the creation order of the 12 light actors in `examples/cornell_box.cpp:136-144`. Revert the print (`git checkout ohao/gpu/vulkan/light_upload.cpp`, then `touch` it). Ledger: `Task 1: RT light order = <creation order | other: list>`. Task 6 makes `LightTable` order by creation sequence; if the recorded order is not creation order, Task 8's cornell RT golden comparison expects a sample-sequence change and must be verified by eye/converged render instead (ruling).

- [ ] **Step 6: Commit**

```bash
git add tests/golden/render_golden.py tests/golden/manifest.json tests/golden/cornell_box_deferred.png tests/golden/damaged_helmet_deferred.png
git commit -m "test(golden): deferred goldens for cornell and helmet; --only <name>"
```

---

### Task 2: EnTT and `ohao/ecs` (World, components, TransformSystem)

**Files:**
- Create: `external/cmake/entt.cmake`; Modify: `external/CMakeLists.txt` (include it after glm)
- Create: `ohao/render/scene/handles.hpp`
- Create: `ohao/ecs/CMakeLists.txt`, `ohao/ecs/components.hpp`, `ohao/ecs/world.hpp`, `ohao/ecs/world.cpp`, `ohao/ecs/transform_system.hpp`, `ohao/ecs/transform_system.cpp`
- Modify: `ohao/CMakeLists.txt` (`add_subdirectory(ecs)` after `physics`)
- Create: `tests/ecs/CMakeLists.txt`, `tests/ecs/ecs_tests.cpp`; Modify: root `CMakeLists.txt` (option `BUILD_ECS_TESTS` ON, `add_subdirectory(tests/ecs)`)

**Interfaces:**
- Produces:
  - `ohao::render::MeshHandle { uint32_t index; uint32_t generation; bool valid() const; }`, `MaterialHandle` same shape, `inline constexpr uint32_t kInvalidIndex = UINT32_MAX;`
  - components (namespace `ohao::ecs`): `Name`, `Transform`, `WorldTransform`, `Parent`, `Children`, `MeshRenderer`, `LightType`, `Light`, `EmissiveProxyLight`, tags `TransformDirty`, `RenderDirty`, `MovedLastFrame`
  - `class World` with `registry()`, `create(std::string_view name, const Transform& = {})`, `destroy(entt::entity)`, `setParent(entt::entity child, entt::entity parent)`, `setTransform(entt::entity, const Transform&)`, `addSystem(std::string name, System)`, `update(float dt)`, `systemNames()`
  - `void ecs::updateTransforms(entt::registry&)`; `std::size_t ecs::verifyTransforms(const entt::registry&)`

- [ ] **Step 1: EnTT dependency**

`external/cmake/entt.cmake`:

```cmake
# EnTT -- header-only ECS. Pinned; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.2
)
FetchContent_GetProperties(entt)
if(NOT entt_POPULATED)
    FetchContent_Populate(entt)
endif()
if(NOT TARGET EnTT::EnTT)
    add_library(EnTT INTERFACE)
    add_library(EnTT::EnTT ALIAS EnTT)
    target_include_directories(EnTT INTERFACE ${entt_SOURCE_DIR}/src)
    target_compile_features(EnTT INTERFACE cxx_std_17)
endif()
message(STATUS "EnTT included at: ${entt_SOURCE_DIR}")
```

In `external/CMakeLists.txt`, after the glm include line: `include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/entt.cmake)`.

- [ ] **Step 2: Handles header**

`ohao/render/scene/handles.hpp`:

```cpp
#pragma once
#include <cstdint>

namespace ohao::render {

inline constexpr uint32_t kInvalidIndex = UINT32_MAX;

// Generational handles: a slot index plus the generation it was issued at. A handle
// whose generation no longer matches its slot refers to something that was released.
struct MeshHandle {
    uint32_t index{kInvalidIndex};
    uint32_t generation{0};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    constexpr bool operator==(const MeshHandle&) const = default;
};

struct MaterialHandle {
    uint32_t index{kInvalidIndex};
    uint32_t generation{0};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    constexpr bool operator==(const MaterialHandle&) const = default;
};

} // namespace ohao::render
```

- [ ] **Step 3: Components**

`ohao/ecs/components.hpp`:

```cpp
#pragma once
#include "render/scene/handles.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ohao::ecs {

struct Name { std::string value; };

// Local transform. Write it with registry.patch<Transform>() or World::setTransform();
// a plain get<>() write is not tracked (see verifyTransforms).
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    [[nodiscard]] glm::mat4 matrix() const;
};

// world: this frame's object-to-world. prevWorld: last frame's (motion vectors).
struct WorldTransform {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
};

struct Parent { entt::entity entity{entt::null}; };
struct Children { std::vector<entt::entity> entities; };

// One material per submesh, in submesh order. Fewer entries than submeshes is legal:
// the missing submeshes render with the default material.
struct MeshRenderer {
    render::MeshHandle mesh{};
    std::vector<render::MaterialHandle> materials;
    bool visible{true};
};

enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };

// Position and direction come from WorldTransform: direction is the entity's -Z.
struct Light {
    LightType type{LightType::Point};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1.0f, 0.0f, 0.0f};   // AreaRect, local space
    glm::vec3 areaEdge2{0.0f, 0.0f, 1.0f};
};

// A point light approximating a mesh's emissive texture (ported from the renderer's
// emissive auto-light). localCenter/radius are in the entity's model space.
struct EmissiveProxyLight {
    glm::vec3 localCenter{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float intensity{0.0f};
};

struct TransformDirty {};
struct RenderDirty {};
struct MovedLastFrame {};

} // namespace ohao::ecs
```

- [ ] **Step 4: Write the failing tests**

`tests/ecs/ecs_tests.cpp`:

```cpp
// ECS tests. No GPU.
#include <cmath>
#include <cstdio>

#include "ecs/components.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/matrix_transform.hpp>

static int g_pass = 0, g_fail = 0;
#define CHECK(expr, msg)                                                                     \
    do {                                                                                     \
        if (!(expr)) { std::printf("    FAIL: %s  [%s]\n", msg, #expr); ++g_fail; return; }  \
    } while (0)
#define RUN(fn)                                                       \
    do {                                                              \
        std::printf("  %-52s", #fn);                                  \
        int f0 = g_fail;                                              \
        fn();                                                         \
        if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); }        \
    } while (0)

using namespace ohao::ecs;

static bool near(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::fabs(a[c][r] - b[c][r]) > 1e-5f) return false;
    return true;
}

static void world_matrix_of_a_root() {
    World w;
    auto e = w.create("root", Transform{{1, 2, 3}, {1, 0, 0, 0}, {2, 2, 2}});
    w.update(0.0f);
    const auto& wt = w.registry().get<WorldTransform>(e);
    glm::mat4 expect = glm::translate(glm::mat4(1), {1, 2, 3}) * glm::scale(glm::mat4(1), {2, 2, 2});
    CHECK(near(wt.world, expect), "root world = T*R*S of its local transform");
    CHECK(near(wt.prevWorld, wt.world), "a new entity has no motion: prevWorld = world");
}

static void child_composes_parent() {
    World w;
    auto p = w.create("p", Transform{{10, 0, 0}});
    auto c = w.create("c", Transform{{0, 1, 0}});
    w.setParent(c, p);
    w.update(0.0f);
    glm::vec3 pos = glm::vec3(w.registry().get<WorldTransform>(c).world[3]);
    CHECK(std::fabs(pos.x - 10) < 1e-5f && std::fabs(pos.y - 1) < 1e-5f, "child world = parent * local");
}

static void moving_parent_moves_child_and_tracks_prev() {
    World w;
    auto p = w.create("p");
    auto c = w.create("c", Transform{{0, 1, 0}});
    w.setParent(c, p);
    w.update(0.0f);
    w.setTransform(p, Transform{{5, 0, 0}});
    w.update(0.0f);
    const auto& wt = w.registry().get<WorldTransform>(c);
    CHECK(std::fabs(wt.world[3].x - 5) < 1e-5f, "child follows the moved parent");
    CHECK(std::fabs(wt.prevWorld[3].x - 0) < 1e-5f, "prevWorld holds last frame's position");
    CHECK(w.registry().all_of<RenderDirty>(c), "a moved child is render-dirty");
    w.registry().clear<RenderDirty>();
    w.update(0.0f);   // no movement this frame
    const auto& wt2 = w.registry().get<WorldTransform>(c);
    CHECK(near(wt2.prevWorld, wt2.world), "the frame after a move, prevWorld catches up");
    CHECK(w.registry().all_of<RenderDirty>(c), "...and the catch-up is uploaded");
    w.registry().clear<RenderDirty>();
    w.update(0.0f);
    CHECK(!w.registry().all_of<RenderDirty>(c), "a still entity is not re-uploaded");
}

static void patch_marks_dirty() {
    World w;
    auto e = w.create("e");
    w.update(0.0f);
    w.registry().clear<RenderDirty>();
    w.registry().patch<Transform>(e, [](Transform& t) { t.position.x = 3; });
    CHECK(w.registry().all_of<TransformDirty>(e), "patch<Transform> tags TransformDirty");
    w.update(0.0f);
    CHECK(std::fabs(w.registry().get<WorldTransform>(e).world[3].x - 3) < 1e-5f, "patched value reaches world");
}

static void unpatched_write_is_detected() {
    World w;
    auto e = w.create("e");
    w.update(0.0f);
    CHECK(verifyTransforms(w.registry()) == 0, "consistent after update");
    w.registry().get<Transform>(e).position.x = 7;   // the forbidden write
    CHECK(verifyTransforms(w.registry()) == 1, "an untracked write is reported");
}

static void destroy_parent_destroys_subtree() {
    World w;
    auto p = w.create("p");
    auto c = w.create("c");
    auto g = w.create("g");
    w.setParent(c, p);
    w.setParent(g, c);
    w.destroy(p);
    CHECK(!w.registry().valid(p) && !w.registry().valid(c) && !w.registry().valid(g),
          "destroying a parent destroys its whole subtree");
}

static void destroy_child_detaches_from_parent() {
    World w;
    auto p = w.create("p");
    auto c = w.create("c");
    w.setParent(c, p);
    w.destroy(c);
    CHECK(w.registry().get<Children>(p).entities.empty(), "parent forgets a destroyed child");
}

static void reparent_keeps_local() {
    World w;
    auto a = w.create("a", Transform{{1, 0, 0}});
    auto b = w.create("b", Transform{{0, 0, 5}});
    auto c = w.create("c", Transform{{0, 1, 0}});
    w.setParent(c, a);
    w.update(0.0f);
    w.setParent(c, b);
    w.update(0.0f);
    glm::vec3 pos = glm::vec3(w.registry().get<WorldTransform>(c).world[3]);
    CHECK(std::fabs(pos.z - 5) < 1e-5f && std::fabs(pos.x) < 1e-5f, "reparenting composes with the new parent");
    CHECK(w.registry().get<Children>(a).entities.empty(), "old parent forgets the child");
}

static void systems_run_in_registration_order() {
    World w;
    std::string order;
    w.addSystem("first", [&](World&, float) { order += "1"; });
    w.addSystem("second", [&](World&, float) { order += "2"; });
    w.update(0.0f);
    CHECK(order == "12", "systems run in the order they were added");
}

int main() {
    std::printf("ECS tests\n");
    RUN(world_matrix_of_a_root);
    RUN(child_composes_parent);
    RUN(moving_parent_moves_child_and_tracks_prev);
    RUN(patch_marks_dirty);
    RUN(unpatched_write_is_detected);
    RUN(destroy_parent_destroys_subtree);
    RUN(destroy_child_detaches_from_parent);
    RUN(reparent_keeps_local);
    RUN(systems_run_in_registration_order);
    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
```

`tests/ecs/CMakeLists.txt`:

```cmake
add_executable(ecs_tests ecs_tests.cpp)
target_compile_features(ecs_tests PRIVATE cxx_std_20)
target_link_libraries(ecs_tests PRIVATE ohao_ecs)
if(MSVC)
    target_link_options(ecs_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_test(NAME EcsTests COMMAND ecs_tests)
```

Root `CMakeLists.txt`, next to `BUILD_RUNTIME_TESTS`:

```cmake
option(BUILD_ECS_TESTS "Build ECS tests (world, hierarchy, transforms)" ON)
if(BUILD_ECS_TESTS)
    add_subdirectory(tests/ecs)
endif()
```

- [ ] **Step 5: Run to confirm it fails**

Run: `/c/Users/djmax/scoop/shims/cmake -B build -S . && /c/Users/djmax/scoop/shims/cmake --build build --config Release --target ecs_tests`
Expected: FAIL to build — `ecs/world.hpp` / `ohao_ecs` not found.

- [ ] **Step 6: Implement `World` and `TransformSystem`**

`ohao/ecs/CMakeLists.txt`:

```cmake
file(GLOB_RECURSE ECS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ECS_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_ecs STATIC ${ECS_SOURCES} ${ECS_HEADERS})
target_include_directories(ohao_ecs PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_ecs PUBLIC cxx_std_20)
target_link_libraries(ohao_ecs PUBLIC ohao_core glm EnTT::EnTT)
```

(Task 6 adds `ohao_renderer ohao_scene` to the link line.)

`ohao/ecs/world.hpp`:

```cpp
#pragma once
#include "ecs/components.hpp"

#include <entt/entt.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

// The game world: an EnTT registry plus an explicitly ordered list of systems.
// Systems run in exactly the order they were added -- no discovery, no sorting.
class World {
public:
    using System = std::function<void(World&, float dt)>;

    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return m_registry; }

    // Creates an entity with Name, Transform (tagged TransformDirty) and an empty Children.
    entt::entity create(std::string_view name, const Transform& local = {});
    // Destroys the entity and its whole subtree, detaching it from its parent first.
    void destroy(entt::entity e);
    // parent == entt::null detaches. The local transform is kept (world changes).
    void setParent(entt::entity child, entt::entity parent);
    void setTransform(entt::entity e, const Transform& local);

    void addSystem(std::string name, System fn);
    void update(float dt);
    [[nodiscard]] std::vector<std::string> systemNames() const;

private:
    void onTransformChanged(entt::registry& r, entt::entity e);

    entt::registry m_registry;
    std::vector<std::pair<std::string, System>> m_systems;
};

} // namespace ohao::ecs
```

`ohao/ecs/world.cpp`:

```cpp
#include "ecs/world.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace ohao::ecs {

glm::mat4 Transform::matrix() const {
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), scale);
}

World::World() {
    m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);
}

void World::onTransformChanged(entt::registry& r, entt::entity e) {
    r.emplace_or_replace<TransformDirty>(e);
}

entt::entity World::create(std::string_view name, const Transform& local) {
    const entt::entity e = m_registry.create();
    m_registry.emplace<Name>(e, std::string(name));
    m_registry.emplace<Transform>(e, local);
    m_registry.emplace<Children>(e);
    m_registry.emplace<TransformDirty>(e);
    return e;
}

void World::destroy(entt::entity e) {
    if (!m_registry.valid(e)) return;
    setParent(e, entt::null);
    // Depth-first over a copy: destroying children edits the vectors we walk.
    std::vector<entt::entity> stack{e};
    std::vector<entt::entity> order;
    while (!stack.empty()) {
        const entt::entity cur = stack.back();
        stack.pop_back();
        order.push_back(cur);
        if (const auto* ch = m_registry.try_get<Children>(cur))
            stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) m_registry.destroy(*it);
}

void World::setParent(entt::entity child, entt::entity parent) {
    if (!m_registry.valid(child)) return;
    if (const auto* old = m_registry.try_get<Parent>(child)) {
        if (m_registry.valid(old->entity))
            if (auto* sibs = m_registry.try_get<Children>(old->entity))
                std::erase(sibs->entities, child);
        m_registry.remove<Parent>(child);
    }
    if (parent != entt::null && m_registry.valid(parent)) {
        m_registry.emplace<Parent>(child, parent);
        m_registry.get_or_emplace<Children>(parent).entities.push_back(child);
    }
    m_registry.emplace_or_replace<TransformDirty>(child);
}

void World::setTransform(entt::entity e, const Transform& local) {
    m_registry.patch<Transform>(e, [&](Transform& t) { t = local; });
}

void World::addSystem(std::string name, System fn) {
    m_systems.emplace_back(std::move(name), std::move(fn));
}

void World::update(float dt) {
    for (auto& [name, fn] : m_systems) fn(*this, dt);
}

std::vector<std::string> World::systemNames() const {
    std::vector<std::string> out;
    for (const auto& [name, fn] : m_systems) out.push_back(name);
    return out;
}

} // namespace ohao::ecs
```

`World::update` must always run `updateTransforms` first. Make that structural: in `World()`, after connecting the signal, `addSystem("transform", [](World& w, float) { updateTransforms(w.registry()); });` (include `ecs/transform_system.hpp`). The test `systems_run_in_registration_order` then sees `"12"` because the transform system appends nothing to `order`.

`ohao/ecs/transform_system.hpp`:

```cpp
#pragma once
#include <entt/entt.hpp>
#include <cstddef>

namespace ohao::ecs {

// Recomputes WorldTransform for every TransformDirty entity and its descendants,
// parents before children. Moved renderables get RenderDirty and MovedLastFrame;
// the next update rewrites their prevWorld (= world) and tags RenderDirty once more,
// so motion vectors return to zero. Clears TransformDirty.
void updateTransforms(entt::registry& reg);

// Debug check: the number of entities whose WorldTransform disagrees with their
// local Transform composed with the parent's world, while not tagged TransformDirty.
// Nonzero means someone wrote Transform through get<>() instead of patch<>().
[[nodiscard]] std::size_t verifyTransforms(const entt::registry& reg);

} // namespace ohao::ecs
```

`ohao/ecs/transform_system.cpp`:

```cpp
#include "ecs/transform_system.hpp"
#include "ecs/components.hpp"

#include <cmath>
#include <vector>

namespace ohao::ecs {

namespace {

glm::mat4 parentWorld(const entt::registry& reg, entt::entity e) {
    if (const auto* p = reg.try_get<Parent>(e))
        if (reg.valid(p->entity))
            if (const auto* wt = reg.try_get<WorldTransform>(p->entity)) return wt->world;
    return glm::mat4(1.0f);
}

void recompute(entt::registry& reg, entt::entity root) {
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        const glm::mat4 world = parentWorld(reg, e) * reg.get<Transform>(e).matrix();
        if (auto* wt = reg.try_get<WorldTransform>(e)) {
            wt->prevWorld = wt->world;
            wt->world = world;
            reg.emplace_or_replace<MovedLastFrame>(e);
        } else {
            reg.emplace<WorldTransform>(e, WorldTransform{world, world});
        }
        reg.emplace_or_replace<RenderDirty>(e);
        reg.remove<TransformDirty>(e);
        if (const auto* ch = reg.try_get<Children>(e))
            stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
    }
}

bool hasDirtyAncestor(const entt::registry& reg, entt::entity e) {
    for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity);
         p = reg.try_get<Parent>(p->entity))
        if (reg.all_of<TransformDirty>(p->entity)) return true;
    return false;
}

} // namespace

void updateTransforms(entt::registry& reg) {
    // 1. Last frame's movers that did not move again: prevWorld catches up.
    std::vector<entt::entity> settled;
    for (auto e : reg.view<MovedLastFrame>())
        if (!reg.all_of<TransformDirty>(e)) settled.push_back(e);
    for (auto e : settled) {
        auto& wt = reg.get<WorldTransform>(e);
        wt.prevWorld = wt.world;
        reg.remove<MovedLastFrame>(e);
        reg.emplace_or_replace<RenderDirty>(e);
    }
    // 2. Dirty subtrees, each from its topmost dirty entity.
    std::vector<entt::entity> roots;
    for (auto e : reg.view<TransformDirty>())
        if (!hasDirtyAncestor(reg, e)) roots.push_back(e);
    for (auto e : roots)
        if (reg.valid(e)) recompute(reg, e);
}

std::size_t verifyTransforms(const entt::registry& reg) {
    std::size_t bad = 0;
    for (auto [e, t, wt] : reg.view<const Transform, const WorldTransform>().each()) {
        if (reg.all_of<TransformDirty>(e)) continue;
        const glm::mat4 expect = parentWorld(reg, e) * t.matrix();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::fabs(expect[c][r] - wt.world[c][r]) > 1e-4f) { ++bad; goto next; }
    next:;
    }
    return bad;
}

} // namespace ohao::ecs
```

Note: a freshly computed entity also gets `MovedLastFrame` only when it already had a `WorldTransform`; the first computation sets `prevWorld = world` and no motion.

- [ ] **Step 7: Run tests**

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target ecs_tests && build/Release/ecs_tests.exe`
Expected: `Results: 9/9 passed`.

- [ ] **Step 8: Commit**

```bash
git add external/cmake/entt.cmake external/CMakeLists.txt ohao/CMakeLists.txt ohao/ecs ohao/render/scene/handles.hpp tests/ecs CMakeLists.txt
git commit -m "feat(ecs): EnTT world with ordered systems, hierarchy and transform propagation"
```

---

### Task 3: Physics decoupling

**Files:**
- Modify: `ohao/physics/dynamics/rigid_body.hpp/.cpp` (no `PhysicsComponent`; add name)
- Modify: `ohao/physics/world/physics_world.hpp/.cpp` (`createRigidBody(std::string_view name)`; drop `m_componentToBody`; drop component syncs)
- Modify: `ohao/physics/world/simulation_profile.cpp:68` (drop `updateTransformComponent`)
- Delete: `ohao/physics/components/physics_component.hpp`, `ohao/physics/components/physics_component.cpp`
- Modify: old scene code that uses physics (still alive until Task 9): `ohao/scene/scene.hpp/.cpp` (drop `PhysicsWorld`, `m_physicsComponents`, `onPhysicsComponentAdded/Removed`, `updatePhysics`), `ohao/scene/actor/actor.cpp:217-231` (drop physics registration), `ohao/scene/component/component_pack.hpp`, `component_factory.hpp/.cpp`, `default_scene_factory.cpp` (drop physics components from packs/factories); `ohao/gpu/vulkan/renderer.hpp/.cpp` (drop `updatePhysics`)
- Modify: `tests/engine/engine_tests.cpp` (drop/port `PhysicsComponent` uses)
- Test: `tests/physics/*` (existing), new case in `tests/engine/engine_tests.cpp`

**Interfaces:**
- Produces: `std::shared_ptr<dynamics::RigidBody> PhysicsWorld::createRigidBody(std::string_view name = {})`; `RigidBody::RigidBody()`; `RigidBody::setName(std::string)`, `const std::string& RigidBody::getName() const`; `PhysicsWorld::resolveHandleName(handle)` returns the body name.

- [ ] **Step 1: Write the failing test** (in `tests/engine/engine_tests.cpp`, physics section)

```cpp
    TEST_BEGIN("a rigid body exists without any scene component and is named");
    {
        ohao::physics::PhysicsWorld world;   // explicit PhysicsWorld(const PhysicsWorldConfig& = {})
        auto body = world.createRigidBody("crate");
        EXPECT(body != nullptr, "createRigidBody(name) returns a body");
        EXPECT(body->getName() == "crate", "the body carries its name");
        EXPECT_EQ(world.getBodyCount(), size_t{1}, "the world owns it");
        TEST_PASS();
    }
```

- [ ] **Step 2: Run to confirm it fails**

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target engine_tests`
Expected: FAIL to compile — no `createRigidBody(const char*)` / `getName`.

- [ ] **Step 3: Implement**
  - `RigidBody`: replace ctor `RigidBody(PhysicsComponent*)` with `RigidBody()`; delete `m_component`, `getComponent()`, `updateTransformComponent()`, the `#include "scene/component/transform_component.hpp"`; add `std::string m_name;` with `setName`/`getName`.
  - `PhysicsWorld::createRigidBody(std::string_view name)`: `auto body = std::make_shared<dynamics::RigidBody>(); body->setName(std::string(name));` then push and `updateActiveBodyPointers()`. Delete `m_componentToBody` and both `std::erase_if(m_componentToBody, ...)` blocks.
  - `syncPendingBodiesToBackend` (`physics_world.cpp:481-487`): delete the `comp->updateRigidBodyFromTransform()` block — the body's own position/rotation are authoritative.
  - Every `body->updateTransformComponent()` call (`physics_world.cpp:120, 542`, `simulation_profile.cpp:68`): delete. Sub-project 4's ECS physics system reads poses from bodies.
  - `resolveHandleName`: `return body->getName();`.
  - Delete `ohao/physics/components/physics_component.*`; remove every include of it; remove physics from the old scene code listed in **Files** (spheres from `ComponentFactory` become mesh+material only — they were never stepped, so no render changes).
  - Remove `VulkanRenderer::updatePhysics` (no callers).

- [ ] **Step 4: Run tests**

```bash
/c/Users/djmax/scoop/shims/cmake --build build --config Release
for t in engine_tests physics_backend_tests force_generator_tests; do build/Release/$t.exe | tail -1; done
python tests/golden/render_golden.py tests/golden/manifest.json
```
Expected: all suites pass; four golden `[PASS]` (no render change).

- [ ] **Step 5: Commit**

```bash
git add -A ohao/physics ohao/scene ohao/gpu tests/engine
git commit -m "refactor(physics): bodies addressed by handle and name; no scene component coupling"
```

---

### Task 4: RenderScene I — meshes, materials, instances, lights, staged uploads

**Files:**
- Create: `ohao/render/scene/render_scene_types.hpp`, `mesh_registry.hpp/.cpp`, `material_table.hpp/.cpp`, `instance_table.hpp/.cpp`, `light_table.hpp/.cpp`, `render_scene.hpp/.cpp`
- Modify: `ohao/gpu/vulkan/renderer.hpp/.cpp` (own `RenderScene`, init after device + texture manager, `renderScene()` accessor, `RenderScene::recordUpdates(cmd, slot)` at the start of every frame body, `RenderScene::onFrameRetired()` after the slot's fence wait, shutdown before device destruction)
- Test: `tests/engine/engine_tests.cpp` (new section "RenderScene")

**Interfaces:**
- Consumes: `render::MeshHandle`, `render::MaterialHandle` (Task 2); `GpuAllocator`; `Vertex` (`scene/asset/model.hpp`); `LightData` (`renderer.hpp`), `GPULight` (`render/rt/gpu_light.hpp`); `MAX_FRAMES_IN_FLIGHT` (`render/frame/frame_resources.hpp`).
- Produces (namespace `ohao::render`):
  - `struct SubmeshDesc { uint32_t firstIndex; uint32_t indexCount; uint32_t materialSlot; };`
  - `struct MeshData { std::span<const Vertex> vertices; std::span<const uint32_t> indices; std::vector<SubmeshDesc> submeshes; std::vector<uint32_t> triangleMaterialSlot; std::string debugName; };` (empty `submeshes` = one submesh over all indices, slot 0; empty `triangleMaterialSlot` = all 0)
  - `struct MaterialDesc { glm::vec3 baseColor{0.8f}; float roughness{0.5f}; float metallic{0.0f}; uint32_t albedoTex{kNoTexture}, normalTex{kNoTexture}, roughMetalTex{kNoTexture}, emissiveTex{kNoTexture}; float emissiveStrength{0.0f}; };` with `inline constexpr uint32_t kNoTexture = 0xFFFFFFFFu;`
  - `struct InstanceDesc { MeshHandle mesh; std::span<const MaterialHandle> materials; glm::mat4 world; glm::mat4 prevWorld; bool visible; };`
  - `struct LightDesc` (raster+RT fields; see Step 3) with `uint64_t order`
  - GPU structs `InstanceGpu` (160 B), `MeshGpu` (16 B) with `static_assert`s
  - `class RenderScene` (see Step 3) incl. `registerMesh`, `registerMaterial`, `createInstance(const InstanceDesc&) -> uint32_t slot`, `updateInstance(uint32_t, const InstanceDesc&)`, `destroyInstance(uint32_t)`, `setLights(std::span<const LightDesc>)`, `recordUpdates(VkCommandBuffer, uint32_t frameSlot)`, `onFrameRetired()`, accessors for buffers/draw lists, `stats()`.

- [ ] **Step 1: Write the failing tests** (engine_tests, after the existing Vulkan sections)

```cpp
static void runRenderSceneTests() {
    std::cout << "\n=== RenderScene ===" << std::endl;
    using namespace ohao::render;

    auto makeQuad = [](std::vector<ohao::Vertex>& v, std::vector<uint32_t>& i) {
        v.assign(4, ohao::Vertex{});
        v[0].position = {-1, 0, -1}; v[1].position = {1, 0, -1};
        v[2].position = {1, 0, 1};   v[3].position = {-1, 0, 1};
        for (auto& x : v) { x.normal = {0, 1, 0}; x.boneWeights = {1, 0, 0, 0}; }
        i = {0, 1, 2, 0, 2, 3};
    };

    TEST_BEGIN("a registered mesh gets an AABB, a table entry and a BLAS");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        EXPECT(m.valid(), "registerMesh returns a valid handle");
        const MeshInfo* info = rs.meshInfo(m);
        EXPECT(info && info->indexCount == 6 && info->vertexCount == 4, "mesh table entry");
        EXPECT(info->aabbMin.x == -1.0f && info->aabbMax.z == 1.0f, "AABB computed once at registration");
        EXPECT(!rs.rtSupported() || info->blas != ohao::INVALID_BLAS, "one BLAS per mesh when RT is available");
        TEST_PASS();
    }

    TEST_BEGIN("static instances upload once; moving ones upload in proportion");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        std::vector<uint32_t> slots;
        for (int k = 0; k < 10000; ++k)
            slots.push_back(rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true}));
        r.render();
        EXPECT(rs.stats().instanceBytesUploaded >= 10000 * sizeof(InstanceGpu), "first frame uploads everything");
        r.render();
        EXPECT_EQ(rs.stats().instanceBytesUploaded, size_t{0}, "a still scene uploads no instance bytes");
        for (int k = 0; k < 100; ++k)
            rs.updateInstance(slots[k], InstanceDesc{m, {&mat, 1}, glm::mat4(2), glm::mat4(1), true});
        r.render();
        EXPECT_EQ(rs.stats().instanceBytesUploaded, 100 * sizeof(InstanceGpu), "bytes = moved instances");
        TEST_PASS();
    }

    TEST_BEGIN("missing material refs fall back to the default material");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(no Vulkan device -- skipped) "; TEST_PASS(); return; }
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        // Two submeshes, one material given.
        MeshHandle m = rs.registerMesh(MeshData{v, i, {{0, 3, 0}, {3, 3, 1}}, {0, 1}, "two"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{{1, 0, 0}});
        uint32_t s = rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        EXPECT_EQ(rs.materialIndexFor(s, 0), mat.index, "slot 0 uses the given material");
        EXPECT_EQ(rs.materialIndexFor(s, 1), 0u, "slot 1 has no ref: default material 0");
        TEST_PASS();
    }

    TEST_BEGIN("arena growth defers destroying the old buffers past frames in flight");
    {
        _putenv_s("OHAO_VALIDATION", "1");
        ohao::VulkanRenderer r(64, 64);
        const bool ok = r.initialize();
        _putenv_s("OHAO_VALIDATION", "");
        if (!ok || !r.validationEnabled()) { std::cout << "(skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        RenderScene& rs = r.renderScene();
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        const auto grows0 = rs.stats().arenaGrowths;
        for (int k = 0; k < 64 && rs.stats().arenaGrowths == grows0; ++k) {
            std::vector<ohao::Vertex> big(20000, v[0]);
            std::vector<uint32_t> bigI(60000, 0);
            MeshHandle m = rs.registerMesh(MeshData{big, bigI, {}, {}, "big"});
            rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
            r.render();   // frames in flight keep reading the arenas
        }
        EXPECT(rs.stats().arenaGrowths > grows0, "the arena grew during the test");
        for (int k = 0; k < 4; ++k) r.render();
        EXPECT_EQ(r.validationMessages().size(), size_t{0}, "no buffer was destroyed while in use");
        TEST_PASS();
    }
}
```

Call `runRenderSceneTests();` from `main()` after the existing Vulkan sections.

- [ ] **Step 2: Run to confirm it fails**

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target engine_tests`
Expected: FAIL to compile — `render/scene/...` and `renderScene()` do not exist.

- [ ] **Step 3: Types and `RenderScene` interface**

`ohao/render/scene/render_scene_types.hpp`:

```cpp
#pragma once
#include "render/scene/handles.hpp"
#include "render/rt/gpu_light.hpp"
#include "render/rt/rt_acceleration_structure.hpp"
#include "scene/asset/model.hpp"   // Vertex

#include <glm/glm.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ohao::render {

inline constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
inline constexpr uint32_t kMaxMaterialRefsPerInstance = 64;
static_assert(sizeof(Vertex) == 100, "RT shaders read Vertex as 25 floats");

struct SubmeshDesc { uint32_t firstIndex; uint32_t indexCount; uint32_t materialSlot; };

struct MeshData {
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;           // mesh-local
    std::vector<SubmeshDesc> submeshes;          // empty: one submesh, slot 0
    std::vector<uint32_t> triangleMaterialSlot;  // empty: all 0
    std::string debugName;
};

struct MaterialDesc {
    glm::vec3 baseColor{0.8f};
    float roughness{0.5f};
    float metallic{0.0f};
    uint32_t albedoTex{kNoTexture};
    uint32_t normalTex{kNoTexture};
    uint32_t roughMetalTex{kNoTexture};
    uint32_t emissiveTex{kNoTexture};
    float emissiveStrength{0.0f};
};

struct InstanceDesc {
    MeshHandle mesh;
    std::span<const MaterialHandle> materials;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    bool visible{true};
    glm::vec4 giAlbedoPacked{0.73f, 0.73f, 0.73f, 0.95f};   // see InstanceGpu
};

enum class LightKind : uint32_t { Point, Directional, Spot, AreaRect, EmissiveProxy };

// World-space light, already resolved from the ECS. `order` sorts the light list
// (creation sequence) so both light buffers are deterministic.
struct LightDesc {
    LightKind kind{LightKind::Point};
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1, 0, 0};
    glm::vec3 areaEdge2{0, 0, 1};
    uint64_t order{0};
};

// std430, matches shaders/includes/scene/scene_data.glsl
struct InstanceGpu {
    glm::mat4 world;
    glm::mat4 prevWorld;
    uint32_t mesh;
    uint32_t materialRefOffset;
    uint32_t materialRefCount;
    uint32_t flags;          // bit 0 visible
    // LEGACY (ported verbatim from rt_build's materialFullData, read only by rt_gi.rchit):
    // rgb = albedo, a = packed: metallic>0.5 ? -(roughness+0.001) : roughness, then +/-10
    // when the mesh has > 100 vertices. rt_gi treats a < 0.5 as a miss. Delete together
    // with that rule when RT GI is reworked.
    glm::vec4 giAlbedoPacked;
};
static_assert(sizeof(InstanceGpu) == 160);

struct MeshGpu {
    uint32_t vertexOffset;        // in vertices
    uint32_t indexOffset;         // in indices
    uint32_t indexCount;
    uint32_t triangleSlotOffset;  // into the triangle material-slot stream
};
static_assert(sizeof(MeshGpu) == 16);

struct MeshInfo {
    uint32_t vertexOffset, vertexCount, indexOffset, indexCount, triangleSlotOffset;
    std::vector<SubmeshDesc> submeshes;
    glm::vec3 aabbMin, aabbMax;
    BlasHandle blas{INVALID_BLAS};
    std::string debugName;
};

struct RenderSceneStats {
    std::size_t instanceBytesUploaded{0};   // last recordUpdates
    std::size_t lightBytesUploaded{0};
    double recordCpuMs{0.0};                // CPU time of the last recordUpdates (staging writes + copy recording)
    uint32_t arenaGrowths{0};               // cumulative
    enum class TlasOp { None, Build, Refit } lastTlasOp{TlasOp::None};
};

} // namespace ohao::render
```

`ohao/render/scene/render_scene.hpp` — public interface (private members implied by **Files**; keep each table in its own file):

```cpp
#pragma once
#include "render/scene/render_scene_types.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <memory>

namespace ohao::render {

class MeshRegistry; class MaterialTable; class InstanceTable; class LightTable;

class RenderScene {
public:
    struct InitInfo {
        VkInstance instance; VkPhysicalDevice physical; VkDevice device;
        VkQueue queue; uint32_t queueFamily; VkCommandPool commandPool;
        RTAccelerationStructure* rt;   // null when RT is unavailable
    };
    RenderScene();
    ~RenderScene();
    [[nodiscard]] bool initialize(const InitInfo& info);
    void shutdown();

    MeshHandle registerMesh(const MeshData& data);            // uploads + BLAS now (blocking, load time)
    MaterialHandle registerMaterial(const MaterialDesc& desc);
    uint32_t createInstance(const InstanceDesc& desc);        // returns slot
    void updateInstance(uint32_t slot, const InstanceDesc& desc);
    void destroyInstance(uint32_t slot);
    void setLights(std::span<const LightDesc> lights);        // sorted by order inside

    // Start of a frame's command buffer, after the slot's fence was waited.
    void recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot);
    // Called once per frame after the slot's fence wait (retire queue tick).
    void onFrameRetired();

    [[nodiscard]] const MeshInfo* meshInfo(MeshHandle h) const;
    [[nodiscard]] uint32_t materialIndexFor(uint32_t slot, uint32_t materialSlot) const;
    [[nodiscard]] bool rtSupported() const noexcept;

    // Buffers the passes bind.
    [[nodiscard]] VkBuffer vertexArena() const;
    [[nodiscard]] VkBuffer indexArena() const;
    [[nodiscard]] VkBuffer triangleSlotBuffer() const;
    [[nodiscard]] VkBuffer meshTable() const;
    [[nodiscard]] VkBuffer materialBuffer() const;
    [[nodiscard]] VkBuffer materialRefBuffer() const;
    [[nodiscard]] VkBuffer instanceBuffer() const;
    [[nodiscard]] std::span<const LightData> rasterLights() const;   // <= MAX_LIGHTS, host copy for the light UBO
    [[nodiscard]] VkBuffer rtLightBuffer() const;                    // header {count, envTexIdx} + GPULight[]
    [[nodiscard]] uint32_t rtLightCount() const;

    struct Draw { uint32_t slot; uint32_t firstIndex; uint32_t indexCount; int32_t vertexOffset; uint32_t materialSlot; glm::vec3 aabbMin, aabbMax; const glm::mat4* world; };
    // Visible instance x submesh draws; rebuilt only when instances change.
    [[nodiscard]] std::span<const Draw> draws() const;

    [[nodiscard]] const RenderSceneStats& stats() const noexcept;
    [[nodiscard]] VkAccelerationStructureKHR tlas() const;           // Task 5
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::render
```

`LightData` and `MAX_LIGHTS` move from `ohao/gpu/vulkan/renderer.hpp` into `render_scene_types.hpp` (renderer.hpp includes it) so `ohao_renderer` code does not depend on the facade header.

- [ ] **Step 4: Implement the tables**

Rules every implementation follows:
- **Arenas** (`MeshRegistry`): one `GpuBuffer` each for vertices (`VERTEX | STORAGE | TRANSFER_DST | TRANSFER_SRC | SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR`), indices (same with `INDEX` instead of `VERTEX`), triangle slots (`STORAGE | TRANSFER_DST | TRANSFER_SRC`) and the mesh table (`STORAGE | TRANSFER_DST | TRANSFER_SRC`), `AllocationUsage::GpuOnly`. Initial capacity 1 Mi vertices / 4 Mi indices / 1 Mi triangles / 4096 meshes. `registerMesh` appends: if it does not fit, allocate double the required capacity, record a copy old → new and the new data upload in one single-time command buffer (submit + `vkQueueWaitIdle` — load time), push the old buffer to the retire queue with `retireAtFrame = frameCounter + MAX_FRAMES_IN_FLIGHT`, `++stats.arenaGrowths`. Every BLAS keeps device addresses into the arena: after a growth, rebuild every BLAS against the new buffers (`RTAccelerationStructure::destroyBLAS` + `createBLAS` with `vertexByteOffset = vertexOffset * 100`, `indexByteOffset = indexOffset * 4`, stride 100) and force a TLAS rebuild.
- **AABB**: min/max over `vertices[].position` at registration.
- **Material table**: capacity 4096 packs (48 B), index 0 = `MaterialDesc{}` registered in `initialize()`. Pack = `[(baseColor, bits(albedoTex)), (roughness, metallic, bits(normalTex), bits(emissiveTex)), (bits(roughMetalTex), emissiveStrength, 0, 0)]` — the existing `MaterialGpuPack` layout (`gpu/layout_meta.hpp`) with emissive strength in row 2 `.y`. Material writes go to the same staging path as instances (dirty material indices).
- **Spec note:** the spec's `uint firstMaterial` is realized as a per-slot material reference list (`materialRefOffset`, `materialRefCount`), because an instance carries an arbitrary list of material handles rather than a contiguous range of the material table. Ledger this as a ruling when executing.
- **Instance table**: capacity grows ×2 from 1024; free list of released slots, reused lowest-first (deterministic). Each slot owns refs `[slot * 64, slot * 64 + 64)` in the material-ref buffer; `materialRefCount = min(materials.size(), 64)`; extra materials are ignored with a one-shot log. `materialIndexFor(slot, s)` returns `s < count ? refs[s].index : 0`. Dirty slots go in a sorted set; `recordUpdates` writes each dirty `InstanceGpu` (and its 64 refs, only when the material list changed) into the frame slot's staging buffer and records one `VkBufferCopy` per contiguous run. `stats.instanceBytesUploaded` counts only `InstanceGpu` bytes. A visible-flag change or create/destroy rebuilds `draws()`.
- **Draw list**: for each live visible slot, for each submesh: `{slot, mesh.indexOffset + sub.firstIndex, sub.indexCount, (int32_t)mesh.vertexOffset, sub.materialSlot, aabb, &world}` ordered by slot then submesh.
- **Light table**: `setLights` sorts by `order`; raster: port `VulkanRenderer::updateLightBuffer` (`ohao/gpu/vulkan/buffer_setup.cpp:131-211`) verbatim, replacing the `LightComponent`/transform reads with `LightDesc` fields (type remap Point→1, Directional→0, others→2; first Directional/Spot gets shadow index 0; cap `MAX_LIGHTS`; `EmissiveProxy` lights are appended after the regular ones exactly as `m_cachedEmissiveLights` was: `position = (center, 1)`, `direction = (0, -1, 0, radius*10)`, `color = (color, intensity)`, `params = (0, 0, -1, 0)`). The default-sun fallback stays in the renderer (`render_dispatch.cpp`), unchanged. RT: port the `GPULight` packing from `light_upload.cpp:157-250` verbatim (regular lights first, then emissive proxies as sphere type with `dirAndParam.w = radius`), into a device-local buffer with the same 16-byte header (`count`, `envMapTexIdx` supplied by `setEnvMapTextureIndex(uint32_t)`), max 64.
- **Staging**: one host-visible `GpuBuffer` per frame slot (`MAX_FRAMES_IN_FLIGHT`), grown when needed; the slot's fence guarantees it is free. `recordUpdates` ends with one barrier: `TRANSFER_WRITE` → `SHADER_READ` on `VERTEX_SHADER | FRAGMENT_SHADER | COMPUTE_SHADER | RAY_TRACING_SHADER | ACCELERATION_STRUCTURE_BUILD` (the AS stages only when RT is available).
- **Retire queue**: `std::vector<{uint64_t retireAt; GpuBuffer buf;}>`; `onFrameRetired()` increments `frameCounter` and destroys entries whose `retireAt <= frameCounter`; `shutdown()` destroys all after `vkDeviceWaitIdle`.

`VulkanRenderer` wiring:
- member `std::unique_ptr<render::RenderScene> m_renderScene;` created in `initialize()` after the device, command pool and texture manager exist, passing `m_rtAccel.get()` when RT is supported; `[[nodiscard]] render::RenderScene& renderScene() { return *m_renderScene; }`.
- In every frame body (`renderDeferred`, the RT body), right after `vkBeginCommandBuffer`: `m_renderScene->recordUpdates(cmd, m_currentFrame);`; right after the slot's fence wait: `m_renderScene->onFrameRetired();`.
- `shutdown()`: `m_renderScene->shutdown(); m_renderScene.reset();` before the RT/deferred teardown.

- [ ] **Step 5: Run tests**

```bash
/c/Users/djmax/scoop/shims/cmake --build build --config Release
build/Release/engine_tests.exe 2>&1 | grep -E "FAIL|Results"
python tests/golden/render_golden.py tests/golden/manifest.json
python tests/validation/check_validation.py
```
Expected: engine_tests all pass (new RenderScene tests included); goldens unchanged (nothing reads `RenderScene` yet); validation clean.

- [ ] **Step 6: Commit**

```bash
git add ohao/render/scene ohao/gpu/vulkan tests/engine/engine_tests.cpp
git commit -m "feat(render): RenderScene -- mesh arenas with BLAS per mesh, material/instance/light tables, staged uploads"
```

---

### Task 5: RenderScene II — TLAS from the instance table, built in-frame

**Files:**
- Modify: `ohao/render/rt/rt_acceleration_structure.hpp/.cpp` (add `buildTLASFromDevice`)
- Modify: `ohao/render/scene/render_scene.cpp` (TLAS instance buffer, decision, recording, retire of replaced TLAS storage)
- Test: `tests/engine/engine_tests.cpp`

**Interfaces:**
- Consumes: Task 4 `RenderScene`, `RTAccelerationStructure::createBLAS`.
- Produces: `void RTAccelerationStructure::buildTLASFromDevice(VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t count, bool rebuild);` `RenderScene::tlas()`, `stats().lastTlasOp`, `glm::mat4 RenderScene::tlasTransform(uint32_t slot) const` (host copy of the 3×4 row-major transform, expanded).

- [ ] **Step 1: Write the failing tests**

```cpp
    TEST_BEGIN("moving an instance refits the TLAS with the new transform");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        if (!rs.rtSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle m = rs.registerMesh(MeshData{v, i, {}, {}, "quad"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        uint32_t s = rs.createInstance(InstanceDesc{m, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "first frame builds");
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::None, "a still scene does nothing");
        const glm::mat4 moved = glm::translate(glm::mat4(1), {3, 0, 0});
        rs.updateInstance(s, InstanceDesc{m, {&mat, 1}, moved, glm::mat4(1), true});
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Refit, "a transform change refits");
        EXPECT(rs.tlasTransform(s)[3].x == 3.0f, "the TLAS instance carries the new world matrix");
        TEST_PASS();
    }

    TEST_BEGIN("a slot released and reused in one frame forces a TLAS rebuild");
    {
        ohao::VulkanRenderer r(64, 64);
        if (!r.initialize()) { std::cout << "(skipped) "; TEST_PASS(); return; }
        RenderScene& rs = r.renderScene();
        if (!rs.rtSupported()) { std::cout << "(no RT -- skipped) "; TEST_PASS(); return; }
        r.setRenderMode(ohao::RenderMode::Deferred);
        std::vector<ohao::Vertex> v; std::vector<uint32_t> i; makeQuad(v, i);
        MeshHandle a = rs.registerMesh(MeshData{v, i, {}, {}, "a"});
        MeshHandle b = rs.registerMesh(MeshData{v, i, {}, {}, "b"});
        MaterialHandle mat = rs.registerMaterial(MaterialDesc{});
        uint32_t s = rs.createInstance(InstanceDesc{a, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        r.render(); r.render();
        rs.destroyInstance(s);
        uint32_t s2 = rs.createInstance(InstanceDesc{b, {&mat, 1}, glm::mat4(1), glm::mat4(1), true});
        EXPECT_EQ(s2, s, "the released slot is reused");
        r.render();
        EXPECT(rs.stats().lastTlasOp == RenderSceneStats::TlasOp::Build, "slot reuse rebuilds, never refits");
        TEST_PASS();
    }
```

- [ ] **Step 2: Run to confirm they fail**

Expected: compile error (`tlasTransform`, `lastTlasOp` unset) or FAIL.

- [ ] **Step 3: Implement**
  - `buildTLASFromDevice`: same size query and create/recreate logic as `buildTLAS` (`rt_acceleration_structure.cpp:470-522`) but with `instancesData.data.deviceAddress = instances` and the caller's `rebuild` flag instead of `m_forceTlasRebuild`; when recreating, the old TLAS and its buffer go to a caller-visible retire list (`std::vector<RetiredTlas>` returned by `takeRetired()`) instead of being destroyed immediately — the previous frame may still trace it. Record the build on `cmd`; no single-time submit. Barrier before the build: `src = RAY_TRACING_SHADER | ACCELERATION_STRUCTURE_BUILD` (`ACCELERATION_STRUCTURE_READ | WRITE`) → `dst = ACCELERATION_STRUCTURE_BUILD` (`ACCELERATION_STRUCTURE_READ | WRITE`) (the previous frame's trace and scratch use). Barrier after: `ACCELERATION_STRUCTURE_BUILD` → `RAY_TRACING_SHADER | COMPUTE_SHADER` (`ACCELERATION_STRUCTURE_READ`).
  - `RenderScene`: a device-local `VkAccelerationStructureInstanceKHR` buffer (one per slot, `SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY | TRANSFER_DST`) updated from the dirty set through the same staging copy (instance custom index = slot, mask `0xFF`, flags `TRIANGLE_FACING_CULL_DISABLE`, `accelerationStructureReference` = the mesh BLAS device address; an invisible or released slot gets mask `0`). Host mirror for `tlasTransform`.
  - Decision in `recordUpdates`, after the copies: `structural` (create, destroy, mesh change, visibility change, arena growth) → Build; else any dirty transform → Refit; else None. `stats.lastTlasOp` records it.
  - Retired TLAS entries go to the Task 4 retire queue.

- [ ] **Step 4: Run tests**

```bash
/c/Users/djmax/scoop/shims/cmake --build build --config Release
build/Release/engine_tests.exe 2>&1 | grep -E "FAIL|Results"
python tests/validation/check_validation.py
```
Expected: all pass, validation clean.

- [ ] **Step 5: Commit**

```bash
git add ohao/render/rt/rt_acceleration_structure.* ohao/render/scene tests/engine/engine_tests.cpp
git commit -m "feat(render): TLAS from the instance table, built or refit inside the frame"
```

---

### Task 6: ECS → RenderScene extraction and spawning

**Files:**
- Create: `ohao/ecs/render_extract.hpp/.cpp`, `ohao/ecs/spawn.hpp/.cpp`
- Modify: `ohao/ecs/components.hpp` (`GiMaterialLegacy`, `CreationOrder`), `ohao/ecs/world.cpp` (`create` emplaces `CreationOrder{m_nextSeq++}`)
- Modify: `ohao/ecs/CMakeLists.txt` (link `ohao_renderer ohao_scene`)
- Modify: `tests/ecs/CMakeLists.txt` (link `ohao_gpu_vulkan`), `tests/ecs/ecs_tests.cpp` (GPU section with a headless renderer)

**Interfaces:**
- Consumes: `World`, components (Task 2); `RenderScene`, `MeshData`, `MaterialDesc`, `LightDesc`, `InstanceDesc` (Task 4); `BindlessTextureManager::loadTextureFromMemory`; `Model`.
- Produces:
  - `struct RenderSlot { uint32_t slot; };` (component, owned by extract)
  - `void ecs::installRenderExtract(World&, render::RenderScene&)` — adds system `"render_extract"` after `"transform"` and connects `on_destroy<MeshRenderer>` to queue slot release
  - `struct SpawnedModel { render::MeshHandle mesh; std::vector<render::MaterialHandle> materials; std::optional<EmissiveProxyLight> emissive; };`
  - `SpawnedModel ecs::uploadModel(render::RenderScene&, BindlessTextureManager*, const Model&, std::string_view debugName, const MaterialDesc* overrideMaterial = nullptr);`
  - `entt::entity ecs::spawnModel(World&, render::RenderScene&, BindlessTextureManager*, const Model&, std::string_view name, const Transform&, const MaterialDesc* overrideMaterial = nullptr);`
  - `entt::entity ecs::spawnLight(World&, std::string_view name, const Light&, const Transform&);`
  - `glm::quat ecs::lookRotation(glm::vec3 direction);` (−Z aligned to `direction`)
  - `struct GiMaterialLegacy { glm::vec4 packed; };` (component; extract copies it into `InstanceDesc::giAlbedoPacked`)
  - `struct CreationOrder { uint64_t seq; };` (component set by `World::create`)

- [ ] **Step 1: Write the failing tests** (append to `tests/ecs/ecs_tests.cpp`; these need a device)

```cpp
#include "gpu/vulkan/renderer.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include <chrono>

static void extract_creates_updates_and_releases_slots() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model quad;   // two triangles
    quad.vertices.assign(4, ohao::Vertex{});
    quad.vertices[1].position = {1, 0, 0}; quad.vertices[2].position = {1, 0, 1}; quad.vertices[3].position = {0, 0, 1};
    quad.indices = {0, 1, 2, 0, 2, 3};
    auto e = spawnModel(w, r.renderScene(), r.getTextureManager(), quad, "q", Transform{});
    w.update(0.0f); r.render();
    CHECK(w.registry().all_of<RenderSlot>(e), "a spawned renderable gets a slot");
    CHECK(r.renderScene().draws().size() == 1, "one draw");
    w.setTransform(e, Transform{{2, 0, 0}});
    w.update(0.0f); r.render();
    CHECK(r.renderScene().stats().instanceBytesUploaded == sizeof(ohao::render::InstanceGpu), "only the moved one uploads");
    w.destroy(e);
    w.update(0.0f); r.render();
    CHECK(r.renderScene().draws().empty(), "destroying the entity releases its slot");
}

static void destroyed_subtree_releases_slots() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model quad; quad.vertices.assign(3, ohao::Vertex{}); quad.vertices[1].position = {1,0,0}; quad.vertices[2].position = {0,0,1}; quad.indices = {0,1,2};
    auto p = spawnModel(w, r.renderScene(), r.getTextureManager(), quad, "p", Transform{});
    auto c = spawnModel(w, r.renderScene(), r.getTextureManager(), quad, "c", Transform{});
    w.setParent(c, p);
    w.update(0.0f); r.render();
    w.destroy(p);
    w.update(0.0f); r.render();
    CHECK(r.renderScene().draws().empty(), "parent and child slots both released");
}

static void ten_thousand_instances_one_percent_moving_under_half_a_millisecond() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model quad; quad.vertices.assign(3, ohao::Vertex{}); quad.vertices[1].position = {1,0,0}; quad.vertices[2].position = {0,0,1}; quad.indices = {0,1,2};
    SpawnedModel shared = uploadModel(r.renderScene(), r.getTextureManager(), quad, "q");
    std::vector<entt::entity> es;
    for (int k = 0; k < 10000; ++k) {
        auto e = w.create("i", Transform{{float(k), 0, 0}});
        w.registry().emplace<MeshRenderer>(e, MeshRenderer{shared.mesh, shared.materials, true});
        es.push_back(e);
    }
    w.update(0.0f); r.render(); r.render();
    double worst = 0;
    for (int f = 0; f < 20; ++f) {
        for (int k = 0; k < 100; ++k)
            w.registry().patch<Transform>(es[(f * 100 + k) % es.size()], [&](Transform& t) { t.position.y = float(f); });
        auto t0 = std::chrono::steady_clock::now();
        w.update(0.0f);   // TransformSystem + extract
        auto t1 = std::chrono::steady_clock::now();
        r.render();       // recordUpdates writes staging; measured inside RenderScene
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() +
                          r.renderScene().stats().recordCpuMs;
        worst = std::max(worst, ms);
    }
    std::printf("(worst %.3f ms) ", worst);
    CHECK(worst < 0.5, "TransformSystem + extract + staging for 10k instances, 1% moving, < 0.5 ms");
}
```

Register them in `main()` after the CPU tests.

- [ ] **Step 2: Run to confirm it fails**

Expected: compile error (`installRenderExtract`, `spawnModel`).

- [ ] **Step 3: Implement**
  - `render_extract.cpp`: system body, in this order:
    1. `releaseQueue` (filled by `on_destroy<MeshRenderer>` and `on_destroy<RenderSlot>` handlers — the handler reads the entity's `RenderSlot` before it disappears) → `rs.destroyInstance(slot)`.
    2. `view<MeshRenderer, WorldTransform>(entt::exclude<RenderSlot>)` → `createInstance` (same `InstanceDesc` as step 3), emplace `RenderSlot`.
    3. `view<RenderSlot, MeshRenderer, WorldTransform, RenderDirty>` → `updateInstance` (sets `InstanceDesc{mr.mesh, mr.materials, wt.world, wt.prevWorld, mr.visible}`) plus `giAlbedoPacked` from `GiMaterialLegacy` when the entity has one; also `on_update<MeshRenderer>` tags `RenderDirty`.
    4. `registry.clear<RenderDirty>()`.
    5. Lights: when any `Light`/`EmissiveProxyLight` entity is `RenderDirty`, constructed or destroyed this frame (track a `m_lightsDirty` flag via signals), rebuild the `LightDesc` list: `Light` → position `wt.world[3]`, direction `normalize(mat3(wt.world) * (0,0,-1))`, edges `mat3(wt.world) * edge`; `EmissiveProxyLight` → `LightKind::EmissiveProxy`, center `wt.world * (localCenter, 1)`; `order = entt::to_integral(entity)`-independent creation sequence stored in a `CreationOrder { uint64_t seq; }` component set by `World::create` (add it there — a monotonically increasing counter). `rs.setLights(list)`.
  - `spawn.cpp`:
    - `uploadModel`: build `MeshData` from `Model` — submeshes from contiguous runs of `materialPerTriangle` (one submesh per run, `materialSlot` = run's material index); `triangleMaterialSlot = materialPerTriangle` (or empty). Materials: one `MaterialDesc` per `materialColors` entry exactly as `gbuffer_pass.cpp:285-300` derives it (`baseColor = rgb`, `roughness = .w`, `metallic = materialMetallic[i]`), textures from `albedoTextures`/`normalTextures`/`roughMetalTextures`/`emissiveTextures` via `materialTextureIndex`/`materialNormalTexIndex`/`materialRoughMetalTexIndex`/`materialEmissiveTexIndex`, uploaded with `loadTextureFromMemory` using the same formats and types `uploadDeferredTextures` uses (`light_upload.cpp:27-143`; read it and copy the format per texture kind); `emissiveStrength = 1` when an emissive texture exists. No `materialColors`: one material from `overrideMaterial` (or default). The emissive proxy: port the statistics loop from `buffer_setup.cpp:222-262` (center/radius from the model AABB in model space, bright-pixel average color, `totalPower`, `intensity = min(totalPower * 0.1, 20)`, only when `totalPower > 0.1`, first emissive material only).
    - `spawnModel`: `uploadModel` then `create(name, t)`, emplace `MeshRenderer`, and `EmissiveProxyLight` when present.
    - `GiMaterialLegacy` (ported from `rt_build.cpp:897-918`): start from albedo `0.73`, roughness `0.95`, metallic `0`; if `overrideMaterial` is given use its `baseColor`/`roughness`/`metallic` (the old code read the actor's `MaterialComponent`, which is what the examples' override material is); `packed = metallic > 0.5 ? -(roughness + 0.001) : roughness`; if the model has `> 100` vertices, `packed += packed >= 0 ? 10 : -10`. Emplace `GiMaterialLegacy{{albedo, packed}}`.

- [ ] **Step 4: Run tests**

```bash
/c/Users/djmax/scoop/shims/cmake --build build --config Release
build/Release/ecs_tests.exe
```
Expected: all pass; the timing test prints its worst-case time below 0.5 ms.

- [ ] **Step 5: Commit**

```bash
git add ohao/ecs tests/ecs
git commit -m "feat(ecs): render extraction and model spawning into RenderScene"
```

---

### Task 7: Deferred passes read RenderScene (temporary bridge from the old Scene)

**Files:**
- Create: `ohao/gpu/vulkan/scene_bridge.cpp` (temporary; deleted in Task 9) — `VulkanRenderer::bridgeSceneToRenderScene()`
- Create: `shaders/includes/scene/scene_data.glsl`
- Modify: `shaders/core/gbuffer.vert`, `shaders/core/gbuffer.frag`, `shaders/shadow/shadow_csm.vert`
- Modify: `ohao/render/deferred/gbuffer_pass.hpp/.cpp`, `csm_pass.hpp/.cpp`, `deferred_renderer.hpp/.cpp` (scene descriptor set per frame slot, draw from `RenderScene::draws()`)
- Modify: `ohao/gpu/vulkan/renderer.cpp`, `render_dispatch.cpp`, `buffer_setup.cpp` (light UBO from `RenderScene::rasterLights()`), `scene_upload.cpp` (`updateSceneBuffers` calls the bridge instead of building combined buffers for raster)
- Test: deferred goldens

**Interfaces:**
- Consumes: `RenderScene` buffers and `draws()` (Task 4); `spawn` helpers' material derivation rules (Task 6; the bridge uses `ecs`-free equivalents in `ohao/render/scene/model_upload.cpp` — see Step 3).
- Produces: `GBufferPass::setRenderScene(const render::RenderScene*)`, `CSMPass::setRenderScene(...)`; descriptor set 1 "scene" for both pipelines; push constants: G-buffer `{uint materialSlot; uint pad[3];}` (16 B), CSM `{mat4 viewProj;}` (64 B); camera UBO `{mat4 viewProj; mat4 prevViewProj;}` per frame slot.

- [ ] **Step 1: Move the model-upload logic below EnTT**

`uploadModel`'s body (Task 6) has no EnTT dependency. Move it to `ohao/render/scene/model_upload.hpp/.cpp` as `render::UploadedModel render::uploadModel(RenderScene&, BindlessTextureManager*, const Model&, std::string_view, const MaterialDesc*)` (+ `EmissiveProxy` struct in `render_scene_types.hpp`), and make `ecs::uploadModel` a thin wrapper. Re-run `ecs_tests` — Expected: unchanged pass.

- [ ] **Step 2: The bridge**

`ohao/gpu/vulkan/scene_bridge.cpp` (header comment: `// TEMPORARY (sub-project 2a, Tasks 7-9): feeds the old Scene into RenderScene so the deferred and RT passes can move first. Deleted in Task 9.`): on `updateSceneBuffers()`, clear `RenderScene` instances (keep meshes cached by `Model*` so re-calls do not re-upload), then for every actor in `getAllActors()` order with a visible `MeshComponent`: `render::uploadModel` (cached), material override from the actor's `MaterialComponent` exactly like `gbuffer_pass.cpp:301-327`, `createInstance` with `actor->getTransform()->getWorldMatrix()`; lights from `LightComponent`s and the model emissive proxies as `LightDesc` with `order` = position in that iteration; `setLights`. This reproduces today's draw and light order.

- [ ] **Step 3: Shared GLSL**

`shaders/includes/scene/scene_data.glsl`:

```glsl
#ifndef OHAO_SCENE_DATA_GLSL
#define OHAO_SCENE_DATA_GLSL
// Matches ohao/render/scene/render_scene_types.hpp (std430).
struct InstanceGpu {
    mat4 world;
    mat4 prevWorld;
    uint mesh;
    uint materialRefOffset;
    uint materialRefCount;
    uint flags;
    vec4 giAlbedoPacked;   // legacy, rt_gi.rchit only
};
struct MeshGpu { uint vertexOffset; uint indexOffset; uint indexCount; uint triangleSlotOffset; };
const uint NO_TEXTURE = 0xFFFFFFFFu;

// Material index for one submesh/triangle slot; missing refs use material 0 (default).
#define OHAO_MATERIAL_INDEX(inst, slot, refs) \
    ((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)
#endif
```

- [ ] **Step 4: G-buffer shaders**

`gbuffer.vert` — replace the push-constant block and the matrix uses:

```glsl
#extension GL_GOOGLE_include_directive : require
#include "../includes/scene/scene_data.glsl"

layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 3) uniform Camera { mat4 viewProj; mat4 prevViewProj; } cam;

layout(location = 8) flat out uint fragInstance;

void main() {
    InstanceGpu inst = instances[gl_InstanceIndex];
    vec4 worldPos = inst.world * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(inst.world)));
    fragNormal = normalize(normalMatrix * inNormal);
    fragTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;
    fragCurrentPos = cam.viewProj * worldPos;
    fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(inPosition, 1.0));
    fragInstance = gl_InstanceIndex;
    gl_Position = fragCurrentPos;
}
```

(Keep the existing `in`/`out` declarations above `main`.)

`gbuffer.frag` — replace the push-constant block with the material fetch and keep the body's logic identical, reading from the pack instead of `pc`:

```glsl
#extension GL_GOOGLE_include_directive : require
#include "../includes/scene/scene_data.glsl"

layout(location = 8) flat in uint fragInstance;
layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 1, std430) readonly buffer Materials { vec4 matRows[]; };
layout(set = 1, binding = 2, std430) readonly buffer MaterialRefs { uint materialRefs[]; };
layout(push_constant) uniform PushConstants { uint materialSlot; } pc;

// at the top of main():
    InstanceGpu inst = instances[fragInstance];
    uint m = OHAO_MATERIAL_INDEX(inst, pc.materialSlot, materialRefs);
    vec4 row0 = matRows[m * 3u + 0u];   // baseColor, albedoTex
    vec4 row1 = matRows[m * 3u + 1u];   // roughness, metallic, normalTex, emissiveTex
    vec4 row2 = matRows[m * 3u + 2u];   // roughMetalTex, emissiveStrength
```

Then substitute in the existing body: `pc.materialParams.w` → `row0.w`; `pc.albedoColor.rgb` → `row0.rgb`; `pc.albedoColor.a` → `row1.z`; `pc.materialParams.x` (metallic) → `row1.y`; `pc.materialParams.y` (roughness) → `row1.x`; `pc.materialParams.z` → `row2.x`; `pc.emissiveParams.x` → `row1.w`; `pc.emissiveParams.y` → `row2.y`. Every other line is unchanged.

`shadow_csm.vert`:

```glsl
#extension GL_GOOGLE_include_directive : require
#include "../includes/scene/scene_data.glsl"
layout(set = 0, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(push_constant) uniform ShadowPushConstants { mat4 viewProj; } pc;   // 64 bytes
void main() { gl_Position = pc.viewProj * instances[gl_InstanceIndex].world * vec4(inPosition, 1.0); }
```

- [ ] **Step 5: Passes**
  - `GBufferPass`: descriptor set layout 1 = {0: storage (instances), 1: storage (materials), 2: storage (material refs), 3: uniform (camera)}, all `VERTEX | FRAGMENT`; one set per frame slot, written in `execute(cmd, frameIndex)` (buffers can change after arena/table growth); camera UBO: one host-visible `GpuBuffer` per frame slot, written in `execute`. Pipeline layout = {bindless set 0, scene set 1}, push constant 16 B fragment stage. Draw loop: bind the scene's vertex/index arenas once; for each `RenderScene::Draw`: CPU AABB cull with `AABB(draw.aabbMin, draw.aabbMax).transformed(*draw.world)` against the frustum (same test as today, without the vertex scan); push `materialSlot`; `vkCmdDrawIndexed(draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, draw.slot)`. Delete `buildMaterialDrawRanges`, `findMaterialTextureIndex`, `GBufferUBO`, `setGeometryBuffers`, `m_meshBufferMap`.
  - `CSMPass`: set 0 = {0: storage instances, `VERTEX`}, one per frame slot; push `viewProj` per cascade; draws from `RenderScene::draws()` (no culling, as today).
  - `DeferredRenderer`/`VulkanRenderer`: pass `&renderScene()` instead of the scene + mesh map; `updateLightBuffer` copies `renderScene().rasterLights()` into the light UBO (keep the existing default-sun fallback and sky override code paths around it).
  - `updateSceneBuffers`: call the bridge for raster; leave the RT build (`buildAccelerationStructures`) untouched in this task.

- [ ] **Step 6: Verify**

```bash
rm -f build/shaders/*.spv; rm -rf build/Release/bin/shaders
/c/Users/djmax/scoop/shims/cmake --build build --config Release
/c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release
build/Release/engine_tests.exe 2>&1 | grep -E "FAIL|Results"
python tests/golden/render_golden.py tests/golden/manifest.json
python tests/validation/check_validation.py
OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200; echo "exit=$?"
```
Expected: deferred goldens `[PASS]` within tolerance; RT goldens unchanged `[PASS]`; validation clean; exit 0. A deferred golden failure is a regression to find, not a golden to update.

- [ ] **Step 7: Commit**

```bash
git add -A ohao shaders
git commit -m "feat(deferred): G-buffer and CSM draw from RenderScene; instance/material storage buffers replace per-draw push constants"
```

---

### Task 8: Ray tracing reads RenderScene; one texture table

**Files:**
- Modify: `shaders/rt/pt_closesthit.rchit`, `shaders/rt/pt_anyhit.rahit`, `shaders/rt/rt_gi.rchit`
- Modify: `ohao/render/rt/path_tracer*.{hpp,cpp}` (descriptor layout: bindings 4,5,8,9,10 repurposed, new 36 instances, 37 material refs; TLAS from `RenderScene`), `rt_gi_technique.*` (instances + materials replace `materialFullData`), `rt_shadow_technique.*` (TLAS from `RenderScene`)
- Modify: `ohao/gpu/vulkan/rt_build.cpp`, `light_upload.cpp`, `renderer.cpp` (`prepareRTSceneForFrame`), `scene_bridge.cpp`
- Delete from `rt_build.cpp`/`renderer.hpp`: `createRTVertexIndexBuffers`, `createRTNormalUVBuffers`, `uploadRTMaterialBuffers`, `updateRTMaterialParams`, `uploadRTTextureArray`, `buildBLASTLAS` and their members; `uploadDeferredTextures` and `cacheEmissiveLights` (now `model_upload` + `LightTable`); `uploadLightBuffer`'s scene walk (RT lights come from `RenderScene::rtLightBuffer()`).
- Test: RT goldens; engine_tests (`updateRTMaterialParams` tests are deleted with the function — port their intent: material edits reach the material table — as `registerMaterial` + a re-register test if needed).

**Interfaces:**
- Consumes: `RenderScene::{vertexArena, indexArena, triangleSlotBuffer, meshTable, materialBuffer, materialRefBuffer, instanceBuffer, rtLightBuffer, rtLightCount, tlas}`; `BindlessTextureManager` views.
- Produces: path tracer/GI/shadow bound to `RenderScene`; `PathTracer::setSceneBuffers(const render::RenderScene&)` replacing `setNormalBuffer`, `setUVBuffer`, `setMaterialBuffers`, `setMaterialData`, `setTextureArray`; `setBindlessTextures` fed with the texture manager's views.

- [ ] **Step 1: Hit shader lookup** (`pt_closesthit.rchit`; same pattern in `pt_anyhit.rahit`)

Replace bindings 4, 5, 8, 9, 10 and add 36, 37:

```glsl
#extension GL_GOOGLE_include_directive : require
#include "../includes/scene/scene_data.glsl"
layout(set = 0, binding = 4, std430)  readonly buffer VertexArena { float vtx[]; };        // Vertex = 25 floats
layout(set = 0, binding = 5, std430)  readonly buffer IndexArena { uint idx[]; };          // mesh-local
layout(set = 0, binding = 8, std430)  readonly buffer MeshTable { MeshGpu meshes[]; };
layout(set = 0, binding = 9, std430)  readonly buffer TriangleSlots { uint triSlot[]; };
layout(set = 0, binding = 10, std430) readonly buffer MatColorBuffer { vec4 matColors[]; } matColorBuf;
layout(set = 0, binding = 36, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 0, binding = 37, std430) readonly buffer MaterialRefs { uint materialRefs[]; };

vec3 vtxNormal(uint v) { uint b = v * 25u + 6u; return vec3(vtx[b], vtx[b + 1u], vtx[b + 2u]); }
vec2 vtxUV(uint v)     { uint b = v * 25u + 9u; return vec2(vtx[b], vtx[b + 1u]); }
```

and in `main()` replace the global-triangle block:

```glsl
    InstanceGpu inst = instances[gl_InstanceCustomIndexEXT];
    MeshGpu mesh = meshes[inst.mesh];
    uint baseIdx = mesh.indexOffset + uint(gl_PrimitiveID) * 3u;
    uint i0 = mesh.vertexOffset + idx[baseIdx + 0u];
    uint i1 = mesh.vertexOffset + idx[baseIdx + 1u];
    uint i2 = mesh.vertexOffset + idx[baseIdx + 2u];
    vec2 texUV = w * vtxUV(i0) + u * vtxUV(i1) + v * vtxUV(i2);
    vec3 n0 = vtxNormal(i0), n1 = vtxNormal(i1), n2 = vtxNormal(i2);
    uint slot = triSlot[mesh.triangleSlotOffset + uint(gl_PrimitiveID)];
    uint matID = OHAO_MATERIAL_INDEX(inst, slot, materialRefs);
```

The rest (`matColorBuf.matColors[matID*3 + k]`, normal matrix from `gl_ObjectToWorldEXT`, texture sampling through `textures[]`) is unchanged. `payload.hitInstance` stays `gl_InstanceCustomIndexEXT` (now the slot).

`rt_gi.rchit`: replace the per-instance `MaterialBuffer` (binding 6) with the instance buffer and read the legacy record — semantics unchanged:

```glsl
#extension GL_GOOGLE_include_directive : require
#include "../includes/scene/scene_data.glsl"
layout(set = 0, binding = 6, std430) readonly buffer Instances { InstanceGpu instances[]; };

void main() {
    vec4 g = instances[gl_InstanceCustomIndexEXT].giAlbedoPacked;
    vec3 albedo = g.rgb;
    float isStatic = g.a;
    if (isStatic < 0.5) { giPayload = vec4(0.0, 0.0, 0.0, -1.0); return; }
    giPayload = vec4(albedo, gl_HitTEXT);
}
```

`RTGITechnique` binds `RenderScene::instanceBuffer()` at binding 6 (replacing `setMaterialData`). The bridge (Task 7/8) fills `InstanceDesc::giAlbedoPacked` with the same formula from the actor's `MaterialComponent`.

- [ ] **Step 2: Host side**
  - `PathTracer::setSceneBuffers(rs)` writes bindings 4, 5, 8, 9, 10, 36, 37 from `RenderScene`, and binding 0 from `rs.tlas()`; the light buffer binding from `rs.rtLightBuffer()`/`rtLightCount()`.
  - Textures: `setBindlessTextures` gets every live view of `BindlessTextureManager` in slot order (index = bindless index), with the manager's default sampler — the same indices the material table stores. Delete the array-image path.
  - `RTShadowTechnique`/`RTGITechnique`: TLAS from `rs.tlas()`; skip when `rs.tlas() == VK_NULL_HANDLE`.
  - `prepareRTSceneForFrame`: no rebuilds; RT data is current because `recordUpdates` ran.
  - `scene_bridge.cpp`: also feed the RT light list (it already builds `LightDesc`s) and the env-map texture index.

- [ ] **Step 3: Verify cornell**

```bash
rm -f build/shaders/*.spv; rm -rf build/Release/bin/shaders
/c/Users/djmax/scoop/shims/cmake --build build --config Release
/c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release
python tests/golden/render_golden.py tests/golden/manifest.json
```
Expected: `cornell_box` `[PASS]` bit-identical (or, per the Task 1 light-order record, a ledgered sample-sequence change verified by eye); both deferred goldens `[PASS]`; `damaged_helmet` (RT) may FAIL — Step 4.

- [ ] **Step 4: Verify the helmet change is only texture resolution**

Build master once in a separate worktree and render both at 1024 spp:

```bash
git worktree add ../ohao_master master
/c/Users/djmax/scoop/shims/cmake -B ../ohao_master/build -S ../ohao_master
/c/Users/djmax/scoop/shims/cmake --build ../ohao_master/build --config Release --target model_viewer
/c/Users/djmax/scoop/shims/cmake --build ../ohao_master/build --target shaders --config Release
(cd ../ohao_master && ./build/Release/model_viewer.exe assets/test_models/DamagedHelmet.glb /tmp/helmet_old.png 1024 --denoise=none)
./build/Release/model_viewer.exe assets/test_models/DamagedHelmet.glb /tmp/helmet_new.png 1024 --denoise=none
```

Compare with a short Python/PIL script: compute `|old - new|` per pixel; report `max_abs` and the fraction of pixels above 4, separately inside and outside the helmet's screen-space bounding box (project the 8 corners of the helmet entity's world AABB with the camera `model_viewer` uses; print them from `model_viewer` behind a `--print-bounds` flag added for this step). Expected: outside the box `max_abs <= 4`; inside, differences only on textured detail (inspect the two images side by side). Ledger the numbers, then update the golden and re-check:

```bash
python tests/golden/render_golden.py tests/golden/manifest.json --update --only damaged_helmet
python tests/golden/render_golden.py tests/golden/manifest.json
git worktree remove ../ohao_master
```
Expected: all four `[PASS]`.

- [ ] **Step 5: Gates**

```bash
build/Release/engine_tests.exe 2>&1 | grep -E "FAIL|Results"
python tests/validation/check_validation.py
OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200; echo "exit=$?"
```
Expected: pass, clean, exit 0.

- [ ] **Step 6: Commit**

```bash
git add -A ohao shaders tests/golden tests/engine
git commit -m "feat(rt): path tracer, RT shadows and RT GI read RenderScene; one bindless texture table"
```

---

### Task 9: Examples on the ECS; old scene code deleted

**Files:**
- Modify: `examples/cornell_box.cpp`, `model_viewer.cpp`, `turntable.cpp`, `env_demo.cpp`, `ohao_game.cpp`, `ohao/render/camera/scene_framer.hpp/.cpp`
- Modify: `ohao/runtime/application.hpp/.cpp` (owns an `ecs::World`; runs `world.update(dt)` after the game's `update` hook and before render; installs render extract)
- Modify: `ohao/gpu/vulkan/renderer.hpp/.cpp` (`setScene` removed; `renderScene()` is the input), `render_dispatch.cpp`, `deferred_renderer.cpp` (brightest-light pick from `RenderScene` light list)
- Delete: `ohao/gpu/vulkan/scene_bridge.cpp`, `scene_upload.cpp`, `ohao/scene/scene.*`, `ohao/scene/actor/`, `ohao/scene/scene_object.*`, `ohao/scene/transform.*`, `ohao/scene/component/`, `ohao/scene/default_scene_factory.*`, `ohao/scene/scene_module.hpp`
- Modify: `tests/engine/engine_tests.cpp` (scene CRUD tests → `ecs_tests` equivalents already exist; delete the old ones), `tests/renderer/renderer_test.cpp`
- Test: everything

**Interfaces:**
- Consumes: `ecs::World`, `spawnModel`, `spawnLight`, `lookRotation`, `installRenderExtract` (Task 6).
- Produces: `Application::world()`; `SceneFramer::applyLights(ecs::World&, const FrameResult&)`; examples build scenes as entities; ohao_game has a spinning object.

- [ ] **Step 1: Port examples**, one file at a time, keeping the scene content and creation order identical (creation order = light order = draw order):
  - `cornell_box`: `addWall` → `spawnModel(world, rs, tm, quadModel, name, Transform{}, &material)`; the two spheres → `PrimitiveMeshGenerator` sphere model + `spawnModel` with the same position/scale; the 12 lights → `spawnLight(world, name, Light{Point, color, intensity, range, radius}, Transform{pos})`. Before rendering: `world.update(0)` once (headless examples have no `Application`; call `installRenderExtract(world, renderer.renderScene())` and `world.update(0)` before each `renderer.render()`).
  - `model_viewer`: the room quads/boxes as above; the model as ONE `spawnModel` (submeshes replace the old per-material split, `model_viewer.cpp:189-270` deleted); `SceneFramer::applyLights(world, frame)`; `flip` rotates the model entity through `world.setTransform` (now reaches RT too).
  - `turntable`, `env_demo`: same mapping; `turntable`'s per-frame rotation uses `world.setTransform`.
  - `ohao_game`: scene as entities; add a spinning cube entity rotated in `hooks.update` via `world.setTransform`.
- [ ] **Step 2: Delete** the old scene code, the bridge, `scene_upload.cpp`, `VulkanRenderer::setScene/getScene/updateSceneBuffers`, `m_scene`, `m_meshBufferMap`. `grep -rn "Actor\|getAllActors\|MeshComponent\|LightComponent\|MaterialComponent\|scene_module" ohao examples tests` must return nothing (except the word in comments/docs, which you also fix).
- [ ] **Step 3: Verify**

```bash
rm -f build/shaders/*.spv; rm -rf build/Release/bin/shaders
/c/Users/djmax/scoop/shims/cmake -B build -S . && /c/Users/djmax/scoop/shims/cmake --build build --config Release
/c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release
for t in engine_tests ecs_tests renderer_test runtime_tests physics_backend_tests audio_system_tests force_generator_tests env_cdf_test denoise_parse_test sobol_test; do printf "%-24s" $t; build/Release/$t.exe > /tmp/$t.txt 2>&1; echo "exit=$?"; done
python tests/golden/render_golden.py tests/golden/manifest.json
python tests/validation/check_validation.py
OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200; echo "exit=$?"
./build/Release/ohao_game.exe --frames 600
grep -rn "entt/" ohao/gpu ohao/render
```
Expected: every suite exit 0; four goldens `[PASS]`; validation clean; game exit 0 with `validation including teardown: 0`; fps ≥ 60; the grep prints nothing. Ask the user to run `ohao_game` and confirm the spinning cube's RT shadow moves with it.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: examples and the game host build ECS worlds; Scene/Actor/Component deleted"
```

---

### Task 10: Documentation

**Files:**
- Modify: monograph units under `site/content/units/` that describe the scene system, scene upload, G-buffer draws, CSM, RT buffers/material lookup, lights (find with `grep -rln "Actor\|getAllActors\|MeshComponent\|m_meshBufferMap\|scene_upload\|rt_build\|GBufferUBO" site/content/units`); add units for `ecs` (world, transform system, extract) and `render/scene` (RenderScene) with `page()` entries and `author_rich.py` blocks; remove units whose subject is gone (3 edits each).
- Modify: `docs/ROADMAP.md` (2a → done), `README.md` if it shows scene API.
- Create: `docs/changes/2026-10-03-ecs-gpu-scene.md` (Haiku agent, reviewed before commit).

- [ ] **Step 1: Monograph** — dispatch per-module agents as in sub-project 1 (rules: cite exact substrings, describe the present, no history, `python site/tools/check_unit.py <module>/<id>` must PASS). Then:

```bash
cd site && python -c "from tools.monograph.build import build; import sys; sys.exit(build())" && cd ..
for t in site/tests/test_monograph_structure.py site/tests/test_blocks.py site/tests/test_cite.py; do python $t 2>&1 | tail -1; done
```
Expected: three `OK`.

- [ ] **Step 2: Change document** — dispatch a Haiku agent with the commit range, the spec and the ledger; review every factual claim against the code before committing.
- [ ] **Step 3: Commit**

```bash
git add -A docs site README.md
git commit -m "docs: ECS world + persistent GPU scene -- monograph, roadmap, change record"
```
