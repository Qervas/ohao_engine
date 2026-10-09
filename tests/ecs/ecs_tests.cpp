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
    RUN(cyclic_parenting_is_rejected);
    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
