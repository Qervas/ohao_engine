// Engine-provided system tests (sub-project 3, step 10; the add-a-test-suite recipe in
// docs/ARCHITECTURE.md followed once). CPU only.
//
//   systems_tests     run every test; last line is a JSON summary
//
// One test group per engine-provided system (ecs::World runs them before the transform
// system). Today: spin (ecs/spin_system.hpp).
#include "api/session.hpp"
#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "reflect/json_writer.hpp"

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

#include <glm/gtc/quaternion.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ohao;
using namespace ohao::ecs;


static const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

static bool bitEqual(const glm::quat& a, const glm::quat& b) {
    return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) &&
           std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
           std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z) &&
           std::bit_cast<uint32_t>(a.w) == std::bit_cast<uint32_t>(b.w);
}

static bool near(const glm::quat& a, const glm::quat& b, float eps = 1e-5f) {
    return std::fabs(glm::dot(a, b)) > 1.0f - eps;   // q and -q are the same rotation
}

// ---- spin ---------------------------------------------------------------------------

static void spin_runs_before_the_transform_system() {
    World w;
    const auto names = w.systemNames();
    CHECK(names.size() == 2 && names[0] == "spin" && names[1] == "transform",
          "every world runs spin, then transform");
}

static void spin_turns_about_the_local_axis_by_rate_times_dt() {
    World w;
    Transform t;
    t.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0));
    const auto e = w.create("top", t);
    w.registry().emplace<Spin>(e, Spin{{0.0f, 2.0f, 0.0f}, 90.0f});   // axis length is irrelevant
    w.update(0.5f);
    const glm::quat want =
        glm::normalize(t.rotation * glm::angleAxis(glm::radians(90.0f * 0.5f), glm::vec3(0, 1, 0)));
    CHECK(bitEqual(w.registry().get<Transform>(e).rotation, want),
          "rotation = normalize(rotation * angleAxis(rate * dt, axis)), bit for bit");
    const glm::mat4 world = w.registry().get<WorldTransform>(e).world;
    CHECK(world == w.registry().get<Transform>(e).matrix(), "the world matrix follows in the same update");
}

static void countWrite(std::size_t& n, entt::registry&, entt::entity) { ++n; }

static void spin_leaves_still_entities_untouched() {
    World w;
    const Spin cases[] = {{{0, 1, 0}, 0.0f}, {{0, 0, 0}, 90.0f}, {{0, NAN, 0}, 90.0f}, {{0, 1, 0}, INFINITY}};
    std::vector<entt::entity> es;
    for (const Spin& s : cases) {
        const auto e = w.create("still");
        w.registry().emplace<Spin>(e, s);
        es.push_back(e);
    }
    const auto moving = w.create("moving");
    w.registry().emplace<Spin>(moving, Spin{});
    w.update(0.0f);   // settles creation
    for (const auto e : es) CHECK(!w.registry().all_of<TransformDirty>(e), "(settled)");
    // Watch for writes: any patch<Transform> tags TransformDirty until the transform system runs.
    std::size_t patched = 0;
    w.registry().on_update<Transform>().connect<&countWrite>(patched);
    w.update(0.0f);
    CHECK(patched == 0, "dt = 0 writes nothing (headless renders call update(0))");
    w.update(1.0f / 60.0f);
    CHECK(patched == 1, "a zero rate, a zero or NaN axis or an infinite rate write nothing; the default Spin turns");
    for (const auto e : es) CHECK(w.registry().get<Transform>(e).rotation == glm::quat(1, 0, 0, 0), "unchanged");
}

static void spin_negative_rates_turn_back_and_steps_add_up() {
    World w;
    const auto e = w.create("e");
    w.registry().emplace<Spin>(e, Spin{{1, 1, 0}, 120.0f});
    for (int i = 0; i < 12; ++i) w.update(0.25f);   // 12 * 30 deg = 360 deg
    CHECK(near(w.registry().get<Transform>(e).rotation, glm::quat(1, 0, 0, 0)), "a full turn comes back to the start");
    w.update(0.25f);
    w.registry().patch<Spin>(e, [](Spin& s) { s.degreesPerSecond = -120.0f; });
    w.update(0.25f);
    CHECK(near(w.registry().get<Transform>(e).rotation, glm::quat(1, 0, 0, 0)), "a negative rate undoes it");
}

static void spin_is_deterministic_and_children_follow() {
    const auto run = [](World& w) {
        const auto parent = w.create("parent");
        Transform local;
        local.position = {2, 0, 0};
        const auto child = w.create("child", local);
        w.setParent(child, parent);
        w.registry().emplace<Spin>(parent, Spin{{0.3f, 1, 0.2f}, 37.0f});
        for (int i = 0; i < 600; ++i) w.update(1.0f / 60.0f);
        return std::pair{parent, child};
    };
    World a, b;
    const auto [pa, ca] = run(a);
    const auto [pb, cb] = run(b);
    CHECK(bitEqual(a.registry().get<Transform>(pa).rotation, b.registry().get<Transform>(pb).rotation),
          "same dt sequence, same bits");
    const float len = glm::length(a.registry().get<Transform>(pa).rotation);
    CHECK(std::fabs(len - 1.0f) < 1e-6f, "600 steps stay a unit quaternion");
    const glm::mat4 want = a.registry().get<WorldTransform>(pa).world * a.registry().get<Transform>(ca).matrix();
    const glm::mat4 got = a.registry().get<WorldTransform>(ca).world;
    float worst = 0.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) worst = std::max(worst, std::fabs(want[c][r] - got[c][r]));
    CHECK(worst < 1e-5f, "the child orbits with its spinning parent");
}

// Through the agent path: a scene file with Spin, world.step, snapshot and restore.
static void spin_through_the_api_steps_saves_and_restores() {
    World w;
    api::Session s(w, engineTypes());
    const std::string scene = R"({"format":"ohao.scene","version":1,"entities":[)"
                              R"({"id":"00000000000000a1","Name":{"value":"Fan"},"Spin":{"degreesPerSecond":45}}]})";
    CHECK(s.call("scene.load", std::string("{\"scene\":") + scene + "}").rfind("{\"result\":", 0) == 0, "loads");
    const auto e = w.findById(0xa1);
    CHECK(e != entt::null && w.registry().get<Spin>(e).axis == glm::vec3(0, 1, 0), "the default axis is +Y");
    const auto before = saveScene(w, engineTypes());
    CHECK(before && before.value().find(R"("Spin": { "degreesPerSecond": 45 })") != std::string::npos,
          "only the non-default field is saved");
    CHECK(s.call("world.snapshot", R"({"name":"base"})").rfind("{\"result\":", 0) == 0, "snapshot");
    CHECK(s.call("world.step", R"({"frames":60,"dt":0.0333333})").rfind("{\"result\":", 0) == 0, "step");
    const glm::quat r = w.registry().get<Transform>(e).rotation;
    CHECK(near(r, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), 1e-4f),
          "60 steps of 1/30 s at 45 deg/s turn 90 degrees about +Y");
    const std::string restored = s.call("world.restore", R"({"name":"base"})");
    CHECK(restored.find("\"changed\":1") != std::string::npos, "the restore puts one Transform back");
    CHECK(saveScene(w, engineTypes()).value() == before.value(), "and the world saves as it did before stepping");
}

int main() {
    std::printf("systems_tests\n");
    RUN(spin_runs_before_the_transform_system);
    RUN(spin_turns_about_the_local_axis_by_rate_times_dt);
    RUN(spin_leaves_still_entities_untouched);
    RUN(spin_negative_rates_turn_back_and_steps_add_up);
    RUN(spin_is_deterministic_and_children_follow);
    RUN(spin_through_the_api_steps_saves_and_restores);
    return ohao::test::finish("systems_tests");
}
