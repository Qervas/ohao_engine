// ECS tests. The CPU tests need no GPU; the render-extract tests make a headless
// VulkanRenderer and print "skipped" when there is no device.
#include <cmath>
#include <cstdio>

#include "ecs/components.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/spawn.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "render/scene/render_scene.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <memory>

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
    w.addSystem("first", [&](World&, float) { order += "1"; });
    w.addSystem("second", [&](World&, float) { order += "2"; });
    w.update(0.0f);
    CHECK(order == "12", "systems run in the order they were added");
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
    const glm::vec4 gi = w.registry().get<GiMaterialLegacy>(e).packed;
    // GI legacy: roughness/metallic come from material 0 (the old texture upload wrote
    // them over the override material before the GI pack read it).
    CHECK(std::fabs(gi.w - 0.25f) < 1e-6f && std::fabs(gi.r - 0.73f) < 1e-6f, "GI legacy pack");
    w.update(0.0f); r.render();
    CHECK(r.renderScene().draws().size() == 2, "one draw per submesh");
}

static void emissive_texture_yields_a_proxy_light() {
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
    CHECK(up.emissive.has_value(), "a bright emissive texture makes a proxy light");
    // 16 white pixels: lum 1 each -> totalPower 16 -> intensity 1.6 (cacheEmissiveLights).
    CHECK(std::fabs(up.emissive->intensity - 1.6f) < 1e-5f, "intensity = min(power * 0.1, 20)");
    CHECK(glm::length(up.emissive->localCenter - glm::vec3(1, 0, 1)) < 1e-6f, "center = model AABB center");
    CHECK(std::fabs(up.emissive->radius - glm::length(glm::vec3(2, 0, 2)) * 0.3f) < 1e-6f, "radius = 0.3 * diagonal");
    auto e = w.create("em", Transform{{0, 5, 0}});
    w.registry().emplace<MeshRenderer>(e, MeshRenderer{up.mesh, up.materials, true});
    w.registry().emplace<EmissiveProxyLight>(e, *up.emissive);
    w.update(0.0f);
    auto lights = r.renderScene().rasterLights();
    CHECK(lights.size() == 1 && std::fabs(lights[0].position.y - 5.0f) < 1e-5f &&
              std::fabs(lights[0].direction.w - up.emissive->radius * 10.0f) < 1e-5f,
          "the proxy is placed by the entity's world matrix, range = radius * 10");
}

// GiMaterialLegacy, like MeshRenderer, reaches the instance table when patched.
static void patched_gi_material_reuploads_the_instance() {
    ohao::VulkanRenderer r(64, 64);
    if (!r.initialize()) { std::printf("(no Vulkan -- skipped) "); return; }
    r.setRenderMode(ohao::RenderMode::Deferred);
    World w;
    installRenderExtract(w, r.renderScene());
    ohao::Model tri; tri.vertices.assign(3, ohao::Vertex{}); tri.vertices[1].position = {1,0,0}; tri.vertices[2].position = {0,0,1}; tri.indices = {0,1,2};
    auto e = spawnModel(w, r.renderScene(), r.getTextureManager(), tri, "t", Transform{});
    w.update(0.0f); r.render();
    w.update(0.0f); r.render();
    CHECK(r.renderScene().stats().instanceBytesUploaded == 0, "precondition: a still entity uploads nothing");
    w.registry().emplace_or_replace<GiMaterialLegacy>(e);
    w.update(0.0f); r.render();
    w.registry().patch<GiMaterialLegacy>(e, [](GiMaterialLegacy& g) { g.packed = {0.1f, 0.2f, 0.3f, 0.5f}; });
    w.update(0.0f); r.render();
    CHECK(r.renderScene().stats().instanceBytesUploaded == sizeof(ohao::render::InstanceGpu),
          "a patched GiMaterialLegacy re-uploads its instance");
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

int main() {
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
    std::printf("GPU (headless)\n");
    RUN(extract_creates_updates_and_releases_slots);
    RUN(destroyed_subtree_releases_slots);
    RUN(ten_thousand_instances_one_percent_moving_under_half_a_millisecond);
    RUN(lights_follow_transforms_in_creation_order);
    RUN(rendering_keeps_ecs_lights);
    RUN(model_materials_become_submeshes_and_materials);
    RUN(emissive_texture_yields_a_proxy_light);
    RUN(unknown_mesh_handles_are_skipped_not_fatal);
    RUN(patched_gi_material_reuploads_the_instance);
    RUN(world_outlives_an_explicit_renderer_shutdown);
    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
