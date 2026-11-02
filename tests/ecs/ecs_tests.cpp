// ECS tests. The CPU tests need no GPU; the render-extract tests make a headless
// VulkanRenderer and print "skipped" when there is no device.
#include <cmath>
#include <cstdio>

#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "level/gltf_structure.hpp"
#include "level/level_hints.hpp"
#include "level/level_system.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/model.hpp"
#include "asset/primitive_mesh_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <memory>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

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

// update() runs verifyTransforms in debug builds and reports the first mismatch
// once per World; Release builds skip the check (the count stays 0).
static void update_reports_an_unpatched_write_in_debug() {
    World w;
    auto e = w.create("e");
    w.update(0.0f);
    CHECK(w.transformMismatches() == 0, "nothing to report after a clean update");
    w.registry().get<Transform>(e).position.x = 7;   // the forbidden write
    w.update(0.0f);
#ifndef NDEBUG
    CHECK(w.transformMismatches() == 1, "a debug update reports the untracked write");
#else
    CHECK(w.transformMismatches() == 0, "Release builds do not run the check");
#endif
}

// registry().destroy() bypasses World::destroy (which takes the subtree with it).
// The registry path detaches: children become roots, nothing keeps a dangling Parent.
static void registry_destroy_of_a_parent_detaches_its_children() {
    World w;
    auto p = w.create("p", Transform{{10, 0, 0}});
    auto c = w.create("c", Transform{{0, 1, 0}});
    auto g = w.create("g", Transform{{0, 0, 1}});
    w.setParent(c, p);
    w.setParent(g, c);
    w.update(0.0f);
    w.registry().destroy(p);
    CHECK(w.registry().valid(c) && w.registry().valid(g), "the registry path keeps the children");
    CHECK(!w.registry().all_of<Parent>(c), "the orphan has no dangling Parent");
    CHECK(w.registry().get<Parent>(g).entity == c, "grandchildren keep their own parent");
    w.update(0.0f);
    const glm::vec3 pos = glm::vec3(w.registry().get<WorldTransform>(g).world[3]);
    CHECK(std::fabs(pos.x) < 1e-5f && std::fabs(pos.y - 1) < 1e-5f && std::fabs(pos.z - 1) < 1e-5f,
          "the detached subtree is re-rooted: world = its own locals");
    CHECK(verifyTransforms(w.registry()) == 0, "and consistent");
}

static void registry_destroy_of_a_child_leaves_the_parent_consistent() {
    World w;
    auto p = w.create("p");
    auto c = w.create("c");
    w.setParent(c, p);
    w.registry().destroy(c);
    CHECK(w.registry().get<Children>(p).entities.empty(), "parent forgets a child destroyed through the registry");
}

static void registry_clear_with_a_hierarchy_is_safe() {
    World w;
    auto p = w.create("p");
    for (int k = 0; k < 8; ++k) w.setParent(w.create("c"), p);
    w.update(0.0f);
    w.registry().clear();
    CHECK(w.registry().storage<entt::entity>().free_list() == 0, "no entity survives clear()");
    CHECK(w.registry().storage<TransformDirty>().empty() && w.registry().storage<Parent>().empty(),
          "no component is left on a dead entity");
    auto e = w.create("after");
    w.update(0.0f);
    CHECK(w.registry().valid(e), "the world is usable after clear()");
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
    w.addSync("third", [&](World&) { order += "3"; });
    w.addSimulate("first", [&](World&, float) { order += "1"; });
    w.addSimulate("second", [&](World&, float) { order += "2"; });
    w.addSyncBefore("third", "before", [&](World&) { order += "b"; });
    w.update(0.0f);
    CHECK(order == "12b3", "simulate systems, then sync systems, each in the order they were added");
    order.clear();
    w.simulate(0.0f);
    CHECK(order == "12", "simulate runs only the simulate phase");
    order.clear();
    w.sync();
    CHECK(order == "b3", "sync runs only the sync phase");
}

static void cyclic_parenting_is_rejected() {
    World w;
    auto a = w.create("a", Transform{{1, 0, 0}});
    auto b = w.create("b", Transform{{0, 1, 0}});
    auto c = w.create("c", Transform{{0, 0, 1}});
    w.setParent(b, a);
    w.setParent(c, b);
    CHECK(!w.setParent(a, a), "self-parenting is rejected");
    CHECK(!w.setParent(a, c), "parenting to a descendant is rejected");
    CHECK(!w.registry().all_of<Parent>(a), "the hierarchy is unchanged");
    w.update(0.0f);   // must return
    glm::vec3 pos = glm::vec3(w.registry().get<WorldTransform>(c).world[3]);
    CHECK(std::fabs(pos.x - 1) < 1e-5f && std::fabs(pos.y - 1) < 1e-5f && std::fabs(pos.z - 1) < 1e-5f,
          "world matrices stay consistent");
    CHECK(verifyTransforms(w.registry()) == 0, "transforms verify clean");
    w.destroy(a);
    CHECK(!w.registry().valid(c), "destroy still terminates and removes the subtree");
}

static void look_rotation_points_minus_z_along_direction() {
    for (glm::vec3 d : {glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0, 1, 0), glm::vec3(0.3f, -0.8f, 0.5f)}) {
        const glm::vec3 f = glm::mat3_cast(lookRotation(d)) * glm::vec3(0, 0, -1);
        CHECK(glm::length(f - glm::normalize(d)) < 1e-5f, "lookRotation maps -Z onto the direction");
    }
}

static void creation_order_is_monotonic() {
    World w;
    auto a = w.create("a");
    auto b = w.create("b");
    w.destroy(a);
    auto c = w.create("c");   // may recycle a's id; the sequence still grows
    CHECK(w.registry().get<CreationOrder>(b).seq < w.registry().get<CreationOrder>(c).seq,
          "creation order follows creation, not entity ids");
}

// ---- GPU: render extraction ------------------------------------------------

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
    CHECK(r.renderScene().draws().size() == 2, "precondition: parent and child both draw");
    w.destroy(p);
    w.update(0.0f); r.render();
    CHECK(r.renderScene().draws().empty(), "parent and child slots both released");
}

static void ten_thousand_instances_one_percent_moving_under_half_a_millisecond() {
#ifndef NDEBUG
    // A timing budget for optimized code; debug builds also run verifyTransforms
    // over every entity each update.
    std::printf("(debug build -- not timed) ");
    return;
#endif
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model quad; quad.vertices.assign(3, ohao::Vertex{}); quad.vertices[1].position = {1,0,0}; quad.vertices[2].position = {0,0,1}; quad.indices = {0,1,2};
    SpawnedModel shared = ohao::ecs::uploadModel(r.renderScene(), r.getTextureManager(), quad, "q");
    std::vector<entt::entity> es;
    for (int k = 0; k < 10000; ++k) {
        auto e = w.create("i", Transform{{float(k), 0, 0}});
        w.registry().emplace<MeshRenderer>(e, MeshRenderer{shared.mesh, shared.materials, true});
        es.push_back(e);
    }
    w.update(0.0f); r.render(); r.render();
    std::vector<double> samples;
    for (int f = 0; f < 20; ++f) {
        for (int k = 0; k < 100; ++k)
            w.registry().patch<Transform>(es[(f * 100 + k) % es.size()], [&](Transform& t) { t.position.y = float(f); });
        auto t0 = std::chrono::steady_clock::now();
        w.update(0.0f);   // TransformSystem + extract
        auto t1 = std::chrono::steady_clock::now();
        r.render();       // recordUpdates writes staging; measured inside RenderScene
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() +
                          r.renderScene().stats().recordCpuMs;
        samples.push_back(ms);
    }
    // Median, not worst: render() between samples records the 10k draws (G-buffer +
    // 4 CSM cascades), and a single OS hiccup in 20 samples must not fail the budget.
    std::sort(samples.begin(), samples.end());
    const double median = (samples[9] + samples[10]) * 0.5;
    std::printf("(median %.3f ms, worst %.3f ms) ", median, samples.back());
    CHECK(median < 0.5, "TransformSystem + extract + staging for 10k instances, 1% moving, median < 0.5 ms");
}

// render() itself never rewrites the light list: the lights the ECS extract set
// survive any number of frames.
static void rendering_keeps_ecs_lights() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    Light point; point.type = LightType::Point; point.intensity = 5.0f;
    spawnLight(w, "point", point, Transform{{1, 4, 2}});
    w.update(0.0f);
    r.render();
    r.render();
    auto lights = r.renderScene().rasterLights();
    CHECK(lights.size() == 1, "the ECS light survives render()");
    CHECK(lights.size() == 1 && glm::length(glm::vec3(lights[0].position) - glm::vec3(1, 4, 2)) < 1e-5f &&
              lights[0].position.w == 1.0f,
          "at its extracted position, as a point light");
}

static void lights_follow_transforms_in_creation_order() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    World w;
    installRenderExtract(w, r.renderScene());
    Light spot; spot.type = LightType::Spot; spot.intensity = 3.0f;
    Light point; point.type = LightType::Point; point.intensity = 5.0f;
    auto s = spawnLight(w, "spot", spot, Transform{{0, 4, 0}, lookRotation({0, -1, 0})});
    spawnLight(w, "point", point, Transform{{1, 2, 3}});
    w.update(0.0f);
    auto lights = r.renderScene().rasterLights();
    CHECK(lights.size() == 2, "two raster lights");
    CHECK(lights[0].color.w == 3.0f && lights[1].color.w == 5.0f, "lights in creation order");
    CHECK(glm::length(glm::vec3(lights[0].direction) - glm::vec3(0, -1, 0)) < 1e-5f, "direction is the entity's -Z");
    CHECK(glm::length(glm::vec3(lights[1].position) - glm::vec3(1, 2, 3)) < 1e-5f, "position from the world matrix");
    w.setTransform(s, Transform{{0, 7, 0}, lookRotation({0, -1, 0})});
    w.update(0.0f);
    CHECK(std::fabs(r.renderScene().rasterLights()[0].position.y - 7.0f) < 1e-5f, "a moved light is re-extracted");
    w.destroy(s);
    w.update(0.0f);
    CHECK(r.renderScene().rasterLights().size() == 1, "a destroyed light leaves the list");
}

static void model_materials_become_submeshes_and_materials() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model m;   // three triangles: materials 1, 1, 0
    m.vertices.assign(4, ohao::Vertex{});
    m.vertices[1].position = {1, 0, 0}; m.vertices[2].position = {1, 0, 1}; m.vertices[3].position = {0, 0, 1};
    m.indices = {0, 1, 2, 0, 2, 3, 0, 1, 3};
    m.materialPerTriangle = {1, 1, 0};
    m.materialColors = {glm::vec4(1, 0, 0, 0.25f), glm::vec4(0, 1, 0, 0.75f)};
    m.materialMetallic = {0.0f, 1.0f};
    auto e = spawnModel(w, r.renderScene(), r.getTextureManager(), m, "m", Transform{});
    const auto& mr = w.registry().get<MeshRenderer>(e);
    CHECK(mr.materials.size() == 2, "one material per materialColors entry");
    const auto* info = r.renderScene().meshInfo(mr.mesh);
    CHECK(info && info->submeshes.size() == 2, "two contiguous material runs");
    CHECK(info->submeshes[0].materialSlot == 1 && info->submeshes[0].indexCount == 6, "run 1: material 1, 2 tris");
    CHECK(info->submeshes[1].materialSlot == 0 && info->submeshes[1].firstIndex == 6, "run 2: material 0");
    w.update(0.0f); r.render();
    CHECK(r.renderScene().draws().size() == 2, "one draw per submesh");
}

// An emissive model is light through its triangles (the path tracer's emissive lights, RT GI's
// bounce): it adds nothing to the raster light table (frontier F3 retired the proxy point light).
static void emissive_texture_adds_no_raster_light() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model m; m.vertices.assign(3, ohao::Vertex{});
    m.vertices[1].position = {2, 0, 0}; m.vertices[2].position = {0, 0, 2}; m.indices = {0, 1, 2};
    m.materialColors = {glm::vec4(1, 1, 1, 0.5f)};
    m.materialMetallic = {0.0f};
    ohao::Model::TextureData tex; tex.width = 4; tex.height = 4; tex.pixels.assign(4 * 4 * 4, 255);
    m.albedoTextures = {tex};
    m.emissiveTextures = {tex};
    m.materialTextureIndex = {0};
    m.materialEmissiveTexIndex = {0};
    SpawnedModel up = ohao::ecs::uploadModel(r.renderScene(), r.getTextureManager(), m, "em");
    auto e = w.create("em", Transform{{0, 5, 0}});
    w.registry().emplace<MeshRenderer>(e, MeshRenderer{up.mesh, up.materials, true});
    w.update(0.0f);
    CHECK(r.renderScene().rasterLights().empty(), "an emissive model adds no light to the raster table");
}

// The renderer's shutdown() keeps the RenderScene object alive (inert) until the
// renderer is destroyed, so a World whose extract holds it can be released after.
static void world_outlives_an_explicit_renderer_shutdown() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    auto w = std::make_unique<World>();
    installRenderExtract(*w, r.renderScene());
    ohao::Model tri; tri.vertices.assign(3, ohao::Vertex{}); tri.vertices[1].position = {1,0,0}; tri.vertices[2].position = {0,0,1}; tri.indices = {0,1,2};
    spawnModel(*w, r.renderScene(), r.getTextureManager(), tri, "t", Transform{});
    w->update(0.0f); r.render();
    r.shutdown();
    CHECK(r.renderScene().createInstance({}) == ohao::render::kInvalidIndex, "and inert after shutdown");
    w.reset();   // releases its slots into the shut-down scene: must not crash
}

static void unknown_mesh_handles_are_skipped_not_fatal() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model empty; empty.vertices.assign(3, ohao::Vertex{});   // zero indices: registerMesh rejects it
    auto bad = spawnModel(w, r.renderScene(), r.getTextureManager(), empty, "empty", Transform{});
    auto def = w.create("default");
    w.registry().emplace<MeshRenderer>(def);   // MeshRenderer{}: invalid handle
    ohao::Model tri; tri.vertices.assign(3, ohao::Vertex{}); tri.vertices[1].position = {1,0,0}; tri.vertices[2].position = {0,0,1}; tri.indices = {0,1,2};
    auto good = spawnModel(w, r.renderScene(), r.getTextureManager(), tri, "good", Transform{});
    for (int f = 0; f < 4; ++f) {
        w.setTransform(bad, Transform{{float(f), 0, 0}});   // keep the bad ones dirty
        w.setTransform(def, Transform{{0, float(f), 0}});
        w.update(0.0f); r.render();
    }
    CHECK(!w.registry().all_of<RenderSlot>(bad), "a rejected mesh gets no slot");
    CHECK(!w.registry().all_of<RenderSlot>(def), "a default MeshRenderer gets no slot");
    CHECK(w.registry().all_of<RenderSlot>(good), "the valid entity beside them gets a slot");
    CHECK(r.renderScene().draws().size() == 1, "only the valid entity draws");
}

// ---- sub-project 5, step 3: levels in the world (spec tests 4 and 9) -------------------------
// A Level component expands into one entity per glTF node through the `level` sync system
// (ohao/level/level_system.hpp). These run on the CPU: the fixture level's meshes have no
// buffers, so nothing here loads geometry except through a resolver that hands out cubes.

namespace fs = std::filesystem;
using ohao::reflect::TypeRegistry;

static const char* g_self = "";   // this executable, for the child-process id test

static const TypeRegistry& engineTypes() {
    static TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

static constexpr const char* kSmallLevel = "tests/level/fixtures/level_small.gltf";
static constexpr const char* kBlockoutLevel = "assets/levels/blockout_room.glb";
static constexpr uint64_t kLevelId = 0x00000000001e7e10ull;

static ohao::level::GltfStructure readLevelStructure(const char* rel) {
    std::ifstream f(fs::path(OHAO_SOURCE_DIR) / rel, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = ohao::level::readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    return r ? r.value() : ohao::level::GltfStructure{};
}

// A world with the level system over the source tree.
struct LevelFixture {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    LevelFixture() {
        world.seedIds(5);
        ohao::level::installLevels(world, files, engineTypes());
    }
    entt::entity addLevel(const char* path, uint64_t id = kLevelId, float lightScale = 1.0f) {
        const entt::entity e = world.createWithId(id, "Level");
        world.registry().emplace<Level>(e, Level{AssetRef{path, {}}, lightScale, true});
        return e;
    }
    // The level's node entities, in creation order.
    [[nodiscard]] std::vector<entt::entity> nodes(entt::entity level) const {
        std::vector<std::pair<uint64_t, entt::entity>> found;
        for (auto [e, n, co] : world.registry().view<const LevelNode, const CreationOrder>().each())
            if (n.level == level) found.emplace_back(co.seq, e);
        std::sort(found.begin(), found.end());
        std::vector<entt::entity> out;
        for (const auto& [seq, e] : found) out.push_back(e);
        return out;
    }
    [[nodiscard]] entt::entity named(std::string_view name) const {
        for (auto [e, n, ln] : world.registry().view<const Name, const LevelNode>().each())
            if (n.value == name) return e;
        return entt::null;
    }
    [[nodiscard]] uint64_t hash() const {
        Snapshot table;
        return hashWorld(world, engineTypes(), table);
    }
};

static bool nearVec3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
    return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(eps)));
}
static glm::vec3 worldPosition(const World& w, entt::entity e) { return glm::vec3(w.registry().get<WorldTransform>(e).world[3]); }
static glm::vec3 facing(const World& w, entt::entity e) {
    return glm::normalize(-glm::vec3(w.registry().get<WorldTransform>(e).world[2]));
}

static void sp5_t04_one_entity_per_node() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    const auto s = readLevelStructure(kSmallLevel);
    const auto nodes = f.nodes(lv);
    CHECK(s.nodes.size() == 12 && nodes.size() == s.nodes.size(), "one entity per node of the default scene");
    CHECK(f.world.idCount() == 13, "and nothing else: the level entity and its 12 nodes");
    const auto& reg = f.world.registry();
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        const LevelNode& ln = reg.get<LevelNode>(nodes[i]);
        CHECK(ln.level == lv && ln.index == i, "in node order, each knowing its level and index");
        CHECK(reg.get<Name>(nodes[i]).value == s.nodes[i].name, "named as in the file");
        CHECK(f.world.idOf(nodes[i]) == ohao::level::levelNodeId(kLevelId, i), "with the id hashed from (level id, index)");
    }
    CHECK(ohao::level::levelNodeId(kLevelId, 0) != 0 && ohao::level::levelNodeId(kLevelId, 0) != ohao::level::levelNodeId(kLevelId, 1) &&
              ohao::level::levelNodeId(kLevelId, 0) != ohao::level::levelNodeId(kLevelId + 1, 0),
          "ids differ per index and per level");

    LevelFixture b;
    const auto blv = b.addLevel(kBlockoutLevel);
    b.world.sync();
    const auto bn = b.nodes(blv);
    std::size_t meshes = 0;
    for (const auto e : bn) meshes += b.world.registry().all_of<MeshSource>(e) ? 1 : 0;
    CHECK(bn.size() == 18 && meshes == 14, "the blockout: 18 nodes, 14 of them drawn meshes");
    CHECK(!b.world.registry().all_of<InstantiateError>(blv), "the blockout loads without errors");
}

static void sp5_t04_hierarchy_matches_file() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    const auto s = readLevelStructure(kSmallLevel);
    const auto nodes = f.nodes(lv);
    CHECK(nodes.size() == s.nodes.size(), "expanded");
    const auto& reg = f.world.registry();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto* p = reg.try_get<Parent>(nodes[i]);
        const entt::entity want = s.nodes[i].parent < 0 ? lv : nodes[static_cast<std::size_t>(s.nodes[i].parent)];
        CHECK(p && p->entity == want, "each node's parent is its file parent's entity, roots under the level entity");
    }
    std::vector<std::string> kids;
    for (const auto c : reg.get<Children>(f.named("Room")).entities) kids.push_back(reg.get<Name>(c).value);
    const std::vector<std::string> want{"Crate", "Lamp", "UCX_Crate_01", "Ghost", "SpawnB", "Spot", "UCX_Shown", "SOCKET_Grip"};
    CHECK(kids == want, "children in file order");
    CHECK(nearVec3(worldPosition(f.world, f.named("Crate")), {1.0f, 0.5f, 0.0f}) &&
              nearVec3(worldPosition(f.world, f.named("SOCKET_Handle")), {1.0f, 1.0f, 0.0f}),
          "world transforms compose through the file's hierarchy");
}

// The child half of sp5_t04_ids_stable_across_processes: the fixture's node ids, one per line.
static int printLevelIds() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    for (const auto e : f.nodes(lv)) std::printf("%016llx\n", static_cast<unsigned long long>(f.world.idOf(e)));
    return 0;
}

static void sp5_t04_ids_stable_across_processes() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    std::string mine;
    char line[64];
    for (const auto e : f.nodes(lv)) {
        std::snprintf(line, sizeof line, "%016llx\n", static_cast<unsigned long long>(f.world.idOf(e)));
        mine += line;
    }
#ifdef _WIN32
    const std::string cmd = "\"\"" + std::string(g_self) + "\" --level-ids\"";
    FILE* p = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "\"" + std::string(g_self) + "\" --level-ids";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    CHECK(p != nullptr, "the child process starts");
    std::string theirs;
    while (std::fgets(line, sizeof line, p)) theirs += line;
#ifdef _WIN32
    const int status = _pclose(p);
#else
    const int status = pclose(p);
#endif
    CHECK(status == 0 && !mine.empty(), "the child process ran");
    CHECK(theirs == mine, "a second process gives the same node ids");
}

static void sp5_t04_save_writes_only_the_level() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    const auto sun = f.world.createWithId(0x5a, "Sun");
    f.world.registry().emplace<Light>(sun, Light{LightType::Directional});
    f.world.sync();
    CHECK(f.nodes(lv).size() == 12, "expanded");
    auto text = saveScene(f.world, engineTypes());
    CHECK(text, "the world saves");
    const std::string& t = text.value();
    std::size_t ids = 0;
    for (std::size_t at = t.find("\"id\""); at != std::string::npos; at = t.find("\"id\"", at + 1)) ++ids;
    CHECK(ids == 2 && t.find("\"Level\"") != std::string::npos, "only the level entity and the sun are written");
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(ohao::level::levelNodeId(kLevelId, 0)));
    CHECK(t.find(hex) == std::string::npos && t.find("\"Crate\"") == std::string::npos, "no node is written");

    World b;
    CHECK(loadScene(b, t, engineTypes()), "the saved scene loads");
    ohao::level::installLevels(b, f.files, engineTypes());
    b.sync();
    std::size_t nodes = 0;
    for (auto [e, n] : b.registry().view<const LevelNode>().each()) nodes += f.world.findById(b.idOf(e)) != entt::null ? 1 : 0;
    CHECK(b.idCount() == 14 && nodes == 12, "reloading it expands the level once, to the same ids (not twice)");
    auto again = saveScene(b, engineTypes());
    CHECK(again && again.value() == t, "and saves to the same bytes");
}

static void sp5_t04_reload_keeps_world_hash() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    auto& reg = f.world.registry();
    const uint64_t h1 = f.hash();
    reg.replace<Level>(lv, reg.get<Level>(lv));   // a reload of the unchanged file
    f.world.sync();
    CHECK(f.hash() == h1, "re-expanding an unchanged level keeps world.hash");
    CHECK(f.nodes(lv).size() == 12, "and its nodes");

    const auto crate = f.named("Crate");
    f.world.setTransform(crate, Transform{{9.0f, 9.0f, 9.0f}});
    reg.emplace<Spin>(f.named("Lamp"));
    f.world.sync();
    CHECK(f.hash() != h1, "a node moved and a component added at run time change the hash");
    reg.patch<Level>(lv, [](Level&) {});
    f.world.sync();
    CHECK(f.hash() == h1, "a reload puts the file's state back: the same hash");
    CHECK(f.named("Crate") == crate && nearVec3(reg.get<Transform>(crate).position, {0.0f, 0.5f, 0.0f}) &&
              !reg.all_of<Spin>(f.named("Lamp")),
          "the same entities, moved back, the added component gone");

    f.world.destroy(f.named("Ghost"));
    reg.patch<Level>(lv, [](Level&) {});
    f.world.sync();
    CHECK(f.nodes(lv).size() == 12 && f.named("Ghost") != entt::null, "a destroyed node is recreated");
}

// Hands out a cube for every model and counts what it was asked for.
struct CubeResolver final : AssetResolver {
    FileAssetResolver files{OHAO_SOURCE_DIR};
    std::vector<std::string> meshes;
    ohao::Result<std::shared_ptr<const ohao::Model>, AssetError> loadModel(const AssetRef&, std::string_view mesh) override {
        meshes.emplace_back(mesh);
        return ohao::Result<std::shared_ptr<const ohao::Model>, AssetError>::ok(ohao::PrimitiveMeshGenerator::makeCube(1.0f));
    }
    bool exists(const AssetRef& r) const override { return files.exists(r); }
    fs::path absolutePath(const AssetRef& r) const override { return files.absolutePath(r); }
};

struct CountingBackend final : InstantiateBackend {
    int uploads = 0;
    ohao::render::MeshHandle uploadMesh(const ohao::Model&, std::string_view) override {
        return {static_cast<uint32_t>(uploads++), 0};
    }
    void releaseMesh(ohao::render::MeshHandle) override {}
    std::vector<ohao::render::MaterialHandle> modelMaterials(const ohao::Model&) override { return {}; }
    ohao::render::MaterialHandle material(const ohao::render::MaterialDesc&) override { return {1, 0}; }
};

static void sp5_t04_collision_only_nodes_have_no_renderer() {
    World w;
    CubeResolver files;
    auto backend = std::make_shared<CountingBackend>();
    installInstantiate(w, backend, files);
    ohao::level::installLevels(w, files, engineTypes());   // after instantiate: still placed before it
    const auto names = w.syncNames();
    const auto pos = [&](const char* n) { return std::find(names.begin(), names.end(), n) - names.begin(); };
    CHECK(pos("level") < pos("instantiate") && pos("instantiate") < pos("transform"), "level runs before instantiate");
    const auto lv = w.createWithId(kLevelId, "Level");
    w.registry().emplace<Level>(lv, Level{AssetRef{kSmallLevel, {}}});
    w.sync();
    const auto& reg = w.registry();
    const auto find = [&](std::string_view n) -> entt::entity {
        for (auto [e, name, ln] : reg.view<const Name, const LevelNode>().each())
            if (name.value == n) return e;
        return entt::null;
    };
    for (const char* hidden : {"Ghost", "UCX_Crate_01"})
        CHECK((!reg.any_of<MeshSource, MeshRenderer>(find(hidden))), "a vis 0 node (or a UCX_ hull) gets no mesh source or renderer");
    for (const char* drawn : {"Room", "Crate", "UCX_Shown"})
        CHECK((reg.all_of<MeshSource, MeshRenderer>(find(drawn))), "drawn nodes get both");
    std::vector<std::string> asked = files.meshes;
    std::sort(asked.begin(), asked.end());
    CHECK((asked == std::vector<std::string>{"#0", "#1", "#3"}) && backend->uploads == 3,
          "only the drawn meshes are loaded and uploaded (GhostMesh never is)");
}

static void sp5_t04_passthrough_equals_scene_json() {
    LevelFixture f;
    f.addLevel(kSmallLevel);
    f.world.sync();
    const auto* spin = f.world.registry().try_get<Spin>(f.named("Spot"));
    CHECK(spin, "the Spot node's extras {\"Spin\": {...}} became a Spin");
    World b;
    CHECK(loadScene(b, R"({"format":"ohao.scene","version":1,"entities":[{"id":"0000000000000001","Spin":{"degreesPerSecond":30}}]})",
                    engineTypes()),
          "the same JSON as a scene file loads");
    const auto* fromFile = b.registry().try_get<Spin>(b.findById(1));
    CHECK(fromFile && ohao::reflect::equalStruct(spin, fromFile, *engineTypes().find<Spin>()),
          "and gives the same component, field for field");
}

static void sp5_t04_ucx_and_socket_conventions() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel);
    f.world.sync();
    const auto& reg = f.world.registry();
    const auto s = readLevelStructure(kSmallLevel);
    std::vector<ohao::reflect::Diagnostic> diags;
    const auto ucx = ohao::level::parseHints(s, 4, engineTypes(), diags);   // UCX_Crate_01, node 4 in depth-first order
    CHECK(s.nodes[4].name == "UCX_Crate_01" && ucx.col == ColliderShape::Convex && !ucx.visible,
          "UCX_<target> reads as {\"col\":\"convex\",\"vis\":0}");
    CHECK(!reg.any_of<MeshSource>(f.named("UCX_Crate_01")), "so the hull is not drawn");
    const auto* shown = reg.try_get<MeshSource>(f.named("UCX_Shown"));
    CHECK(shown && std::get<ModelAsset>(shown->source).mesh == "#3", "explicit extras win over the name rule");
    const auto* handle = reg.try_get<Socket>(f.named("SOCKET_Handle"));
    const auto* grip = reg.try_get<Socket>(f.named("SOCKET_Grip"));
    CHECK(handle && handle->name == "Handle" && grip && grip->name == "grip",
          "SOCKET_<name> becomes Socket{name}; an explicit socket key wins");
    CHECK(diags.empty() && !reg.all_of<InstantiateError>(lv), "the fixture has no hint errors");
}

static void sp5_t04_khr_light_becomes_light() {
    LevelFixture f;
    const auto lv = f.addLevel(kSmallLevel, kLevelId, 2.0f);
    f.world.sync();
    auto& reg = f.world.registry();
    const auto* lamp = reg.try_get<Light>(f.named("Lamp"));
    CHECK(lamp && lamp->type == LightType::Point && nearVec3(lamp->color, {1.0f, 0.5f, 0.25f}) &&
              lamp->intensity == 80.0f && lamp->range == 10.0f,
          "a point light: the file's colour, intensity x lightScale, OHAO's 10 m range where the file has none");
    const auto* spot = reg.try_get<Light>(f.named("Spot"));
    CHECK(spot && spot->type == LightType::Spot && spot->intensity == 200.0f && spot->range == 5.0f &&
              std::fabs(spot->innerConeDeg - glm::degrees(0.2f)) < 1e-4f &&
              std::fabs(spot->outerConeDeg - glm::degrees(0.6f)) < 1e-4f,
          "a spot light: the file's range and cone half-angles in degrees");
    reg.patch<Level>(lv, [](Level& l) { l.lightScale = 1.0f; });
    f.world.sync();
    CHECK(reg.get<Light>(f.named("Lamp")).intensity == 40.0f, "a changed lightScale re-expands the level");
}

static void sp5_t04_level_camera_active_only_without_another() {
    LevelFixture f;
    f.addLevel(kSmallLevel);
    f.world.sync();
    const auto* cam = f.world.registry().try_get<ohao::ecs::Camera>(f.named("Cam"));
    CHECK(cam && cam->active && std::fabs(cam->fovDeg - glm::degrees(0.8f)) < 1e-4f && cam->nearPlane == 0.05f &&
              cam->farPlane == 200.0f,
          "alone, the level's camera is active: yfov in degrees, the file's near, 200 m for an infinite far");

    LevelFixture g;
    const auto mine = g.world.create("SceneCamera");
    g.world.registry().emplace<ohao::ecs::Camera>(mine);
    g.addLevel(kSmallLevel);
    g.world.sync();
    const auto* levelCam = g.world.registry().try_get<ohao::ecs::Camera>(g.named("Cam"));
    CHECK(levelCam && !levelCam->active && g.world.registry().get<ohao::ecs::Camera>(mine).active,
          "beside the scene's camera, the level's is inactive and the scene's wins");
}

static void sp5_t09_spawns_in_node_order_with_facing() {
    const auto spawns = [](const LevelFixture& f) {
        std::vector<std::pair<uint64_t, entt::entity>> found;
        for (auto [e, sp, co] : f.world.registry().view<const SpawnPoint, const CreationOrder>().each())
            found.emplace_back(co.seq, e);
        std::sort(found.begin(), found.end());
        std::vector<entt::entity> out;
        for (const auto& [seq, e] : found) out.push_back(e);
        return out;
    };
    LevelFixture f;
    f.addLevel(kSmallLevel);
    f.world.sync();
    const auto& reg = f.world.registry();
    const auto small = spawns(f);
    CHECK(small.size() == 2 && reg.get<Name>(small[0]).value == "SpawnB" && reg.get<SpawnPoint>(small[0]).tag == "enemy" &&
              reg.get<Name>(small[1]).value == "SpawnA" && reg.get<SpawnPoint>(small[1]).tag == "player",
          "spawns in node order (depth first), with their tags");
    CHECK(nearVec3(facing(f.world, small[0]), {-1.0f, 0.0f, 0.0f}, 1e-4f) && nearVec3(facing(f.world, small[1]), {0.0f, 0.0f, -1.0f}),
          "each faces its local -Z");

    LevelFixture b;
    b.addLevel(kBlockoutLevel);
    b.world.sync();
    const auto& breg = b.world.registry();
    const auto bs = spawns(b);
    CHECK(bs.size() == 3, "the blockout has three spawns");
    const char* names[] = {"SpawnEnemyA", "SpawnEnemyB", "SpawnPlayer"};
    const char* tags[] = {"enemy", "enemy", "player"};
    const glm::vec3 pos[] = {{-2.5f, 0.3f, -2.0f}, {3.0f, 0.5f, -2.0f}, {1.5f, 0.0f, 2.2f}};
    const glm::vec3 dir[] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
    for (int i = 0; i < 3; ++i)
        CHECK(breg.get<Name>(bs[i]).value == names[i] && breg.get<SpawnPoint>(bs[i]).tag == tags[i] &&
                  nearVec3(worldPosition(b.world, bs[i]), pos[i], 1e-5f) && nearVec3(facing(b.world, bs[i]), dir[i], 1e-5f),
              "the blockout's spawns: step 1's contract, in node order, facing as authored");
}

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc > 1 && std::string_view(argv[1]) == "--level-ids") return printLevelIds();
    std::printf("ECS tests\n");
    RUN(world_matrix_of_a_root);
    RUN(child_composes_parent);
    RUN(moving_parent_moves_child_and_tracks_prev);
    RUN(patch_marks_dirty);
    RUN(unpatched_write_is_detected);
    RUN(destroy_parent_destroys_subtree);
    RUN(destroy_child_detaches_from_parent);
    RUN(update_reports_an_unpatched_write_in_debug);
    RUN(registry_destroy_of_a_parent_detaches_its_children);
    RUN(registry_destroy_of_a_child_leaves_the_parent_consistent);
    RUN(registry_clear_with_a_hierarchy_is_safe);
    RUN(reparent_keeps_local);
    RUN(systems_run_in_registration_order);
    RUN(cyclic_parenting_is_rejected);
    RUN(look_rotation_points_minus_z_along_direction);
    RUN(creation_order_is_monotonic);
    std::printf("Levels (sub-project 5, step 3)\n");
    RUN(sp5_t04_one_entity_per_node);
    RUN(sp5_t04_hierarchy_matches_file);
    RUN(sp5_t04_ids_stable_across_processes);
    RUN(sp5_t04_save_writes_only_the_level);
    RUN(sp5_t04_reload_keeps_world_hash);
    RUN(sp5_t04_collision_only_nodes_have_no_renderer);
    RUN(sp5_t04_passthrough_equals_scene_json);
    RUN(sp5_t04_ucx_and_socket_conventions);
    RUN(sp5_t04_khr_light_becomes_light);
    RUN(sp5_t04_level_camera_active_only_without_another);
    RUN(sp5_t09_spawns_in_node_order_with_facing);
    std::printf("GPU (headless)\n");
    RUN(extract_creates_updates_and_releases_slots);
    RUN(destroyed_subtree_releases_slots);
    RUN(ten_thousand_instances_one_percent_moving_under_half_a_millisecond);
    RUN(lights_follow_transforms_in_creation_order);
    RUN(rendering_keeps_ecs_lights);
    RUN(model_materials_become_submeshes_and_materials);
    RUN(emissive_texture_adds_no_raster_light);
    RUN(unknown_mesh_handles_are_skipped_not_fatal);
    RUN(world_outlives_an_explicit_renderer_shutdown);
    return ohao::test::finish("ecs_tests");
}
