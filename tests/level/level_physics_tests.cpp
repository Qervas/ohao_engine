// Level collision (sub-project 5, step 4; docs/specs/2026-10-26-level-pipeline-design.md,
// "Collision generation", tests 6 and 7). CPU only: levels expand through the level system,
// their hints become Collider/RigidBody, and the physics system builds Jolt bodies that the
// tests ray-cast and (test 7's crates) step.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "level/level_system.hpp"
#include "physics/backend/physics_backend.hpp"
#include "reflect/registry.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

using namespace ohao::ecs;
using ohao::InputEvent;

namespace {

const ohao::reflect::TypeRegistry& engineTypes() {
    static ohao::reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

constexpr const char* kPhysicsLevel = "tests/level/fixtures/level_physics.gltf";
constexpr const char* kBlockout = "assets/levels/blockout_room.glb";

// A world with the level and physics systems over the source tree, one level loaded.
struct Fixture {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    PhysicsScene* physics{nullptr};
    entt::entity level{entt::null};
    explicit Fixture(const char* path) {
        world.seedIds(7);
        ohao::level::installLevels(world, files, engineTypes());
        physics = &installPhysics(world, files);
        level = world.createWithId(0x1e7e1, "Level");
        world.registry().emplace<Level>(level, Level{AssetRef{path, {}}});
        world.sync();
    }
    [[nodiscard]] entt::entity named(std::string_view name) const {
        for (auto [e, n, ln] : world.registry().view<const Name, const LevelNode>().each())
            if (n.value == name) return e;
        return entt::null;
    }
    // The hit's height (or the axis asked for), or NaN for a miss.
    [[nodiscard]] std::optional<RayHit> ray(glm::vec3 from, glm::vec3 dir, float max = 100.0f) const {
        return physics->raycast(from, dir, max);
    }
};

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

MeshData brick() {   // the fixture's Brick: (0,0,0)..(2,1,0.5)
    MeshData m;
    for (float x : {0.0f, 2.0f})
        for (float y : {0.0f, 1.0f})
            for (float z : {0.0f, 0.5f}) m.positions.push_back({x, y, z});
    m.indices = {0, 1, 3, 0, 3, 2};   // fitting reads positions; one face is enough to be a mesh
    return m;
}

} // namespace

// ---- test 6: shape fitting ---------------------------------------------------------------------

static void t06_fit_box_sphere_capsule_from_mesh() {
    const MeshData m = brick();
    const FittedCollider box = fitCollider(m, ColliderShape::Box, glm::vec3(1.0f));
    CHECK(box.shape == ColliderShape::Box && nearVec(box.halfExtents, {1.0f, 0.5f, 0.25f}, 1e-6f) &&
              nearVec(box.offset, {1.0f, 0.5f, 0.25f}, 1e-6f),
          "box: the mesh's AABB, its centre as the offset");
    const FittedCollider sphere = fitCollider(m, ColliderShape::Sphere, glm::vec3(1.0f));
    CHECK(sphere.shape == ColliderShape::Sphere && near(sphere.radius, std::sqrt(1.0f + 0.25f + 0.0625f), 1e-5f) &&
              nearVec(sphere.offset, {1.0f, 0.5f, 0.25f}, 1e-6f),
          "sphere: the AABB centre and the farthest vertex from it");
    const FittedCollider capsule = fitCollider(m, ColliderShape::Capsule, glm::vec3(1.0f));
    const glm::vec3 axis = capsule.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(capsule.shape == ColliderShape::Capsule && near(capsule.radius, 0.5f, 1e-6f) &&
              near(capsule.halfHeight, 0.5f, 1e-6f) && nearVec(glm::abs(axis), {1.0f, 0.0f, 0.0f}, 1e-5f),
          "capsule: along the longest AABB axis, radius the larger other half extent");

    Fixture f(kPhysicsLevel);
    const auto top = [&](float x) {
        const auto h = f.ray({x, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
        return h ? h->point.y : -999.0f;
    };
    CHECK(near(top(1.0f), 1.0f) && near(top(11.0f), 0.5f + sphere.radius) && near(top(21.0f), 1.0f),
          "the fitted box, sphere and capsule stand where the mesh is");
    CHECK(f.ray({1.0f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f})->entity == f.named("FitBox"), "a hit names its entity");
}

static void t06_convex_hull_of_mesh() {
    Fixture f(kPhysicsLevel);
    const auto h = f.ray({30.2f, 5.0f, 0.2f}, {0.0f, -1.0f, 0.0f});
    CHECK(h && h->entity == f.named("Hull") && near(h->point.y, 0.6f, 2e-3f),
          "a ray onto the tetrahedron's slanted face hits x + y + z = 1");
    CHECK(nearVec(h->normal, glm::normalize(glm::vec3(1.0f)), 2e-3f), "with that face's normal");
    CHECK(f.physics->info().shapes.convex == 2, "two convex shapes: Hull and the demoted DynMesh");
}

static void t06_static_mesh_shape() {
    Fixture f(kPhysicsLevel);
    const auto h = f.ray({0.0f, 0.0f, 3.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(h && h->entity == f.named("Ground") && near(h->point.y, -5.0f) && nearVec(h->normal, {0.0f, 1.0f, 0.0f}),
          "a mesh node without hints is a static triangle mesh");
    const PhysicsInfo info = f.physics->info();
    CHECK(info.shapes.mesh == 3 && info.bodies.statics == 8, "three mesh shapes (Ground, StretchedMesh, Mirrored), 8 static bodies");
    CHECK(info.shapes.box == 2 && info.shapes.sphere == 1 && info.shapes.capsule == 1, "and 2 boxes, a sphere, a capsule");
    CHECK(!f.world.registry().all_of<Collider>(f.named("Marker")), "an empty without hints has no collider");
}

static void t06_ucx_hulls_are_one_compound_body() {
    Fixture f(kPhysicsLevel);
    const auto& reg = f.world.registry();
    const entt::entity prop = f.named("Prop");
    const auto low = f.ray({40.0f, 5.0f, 0.3f}, {0.0f, 0.0f, -1.0f});   // misses: the ray runs above the hulls
    const auto upper = f.ray({40.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    const auto lower = f.ray({40.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f});
    CHECK(!low && upper && lower, "both hulls collide");
    CHECK(upper->entity == prop && lower->entity == prop && near(upper->point.y, 2.5f) && near(lower->point.z, 0.5f),
          "as one body, the Prop's: the upper hull's top at 2.5, the lower's face at z 0.5");
    CHECK(f.physics->bodyOf(prop) != ohao::physics::backend::INVALID_BODY &&
              f.physics->bodyOf(f.named("UCX_Prop_00")) == ohao::physics::backend::INVALID_BODY &&
              !reg.any_of<MeshSource>(f.named("UCX_Prop_00")),
          "the hull nodes have no body of their own and are not drawn");
    const PhysicsInfo info = f.physics->info();
    CHECK(info.shapes.compound == 1 && info.bodies.dynamics == 2 && info.steps == 0,
          "one compound shape; Prop and DynMesh are the dynamic bodies (not stepped: nothing simulated)");
    const auto* rb = reg.try_get<RigidBody>(prop);
    CHECK(rb && rb->motion == Motion::Dynamic && rb->mass == 3.0f, "the target's body hints are its RigidBody");
}

static void t06_dynamic_mesh_becomes_convex_with_warning() {
    Fixture f(kPhysicsLevel);
    const entt::entity dyn = f.named("DynMesh");
    const auto* c = f.world.registry().try_get<Collider>(dyn);
    CHECK(c && c->shape == ColliderShape::Mesh, "the component keeps what the file asked for");
    const auto& w = f.physics->warnings();
    const bool warned = std::any_of(w.begin(), w.end(), [&](const PhysicsWarning& x) {
        return x.code == "W_LEVEL_DYNAMIC_MESH" && x.entity == dyn && x.message.find("convex") != std::string::npos;
    });
    CHECK(warned, "a dynamic mesh collider is built convex, with W_LEVEL_DYNAMIC_MESH");
    const auto h = f.ray({51.0f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    CHECK(h && h->entity == dyn && near(h->point.y, 1.0f, 2e-3f), "and collides as the brick's hull");
}

static void t06_non_uniform_scale_baked() {
    Fixture f(kPhysicsLevel);
    for (const auto& [name, x] : {std::pair{"Stretched", 60.0f}, std::pair{"StretchedMesh", 70.0f}}) {
        const auto fromX = f.ray({x + 5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
        const auto fromZ = f.ray({x, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f});
        const auto fromY = f.ray({x, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
        CHECK(fromX && fromZ && fromY && fromX->entity == f.named(name), "the stretched cube is hit from three sides");
        CHECK(near(fromX->point.x, x + 1.0f) && near(fromY->point.y, 0.5f) && near(fromZ->point.z, 1.5f),
              "its scale (2, 1, 3) is baked into the shape: faces at 1, 0.5, 1.5");
    }
}

static void t06_mirrored_node_collides_outward() {
    Fixture f(kPhysicsLevel);
    const auto right = f.ray({85.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    const auto left = f.ray({75.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    const auto top = f.ray({80.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(right && left && top, "a mirrored (scale -1) mesh is hit from outside");
    CHECK(near(right->point.x, 80.5f) && nearVec(right->normal, {1.0f, 0.0f, 0.0f}) && near(left->point.x, 79.5f) &&
              nearVec(left->normal, {-1.0f, 0.0f, 0.0f}) && nearVec(top->normal, {0.0f, 1.0f, 0.0f}),
          "with outward normals: the winding is flipped with the mirror");
}

// ---- test 7: the blockout collides -----------------------------------------------------------

static void t07_ray_down_at_each_spawn_hits_authored_height() {
    Fixture f(kBlockout);
    std::size_t spawns = 0;
    for (auto [e, sp, wt] : f.world.registry().view<const SpawnPoint, const WorldTransform>().each()) {
        const glm::vec3 p(wt.world[3]);
        const auto h = f.ray(p + glm::vec3(0.0f, 0.5f, 0.0f), {0.0f, -1.0f, 0.0f}, 2.0f);
        std::printf("\n      %s at (%.3f, %.3f, %.3f): hit %s", f.world.registry().get<Name>(e).value.c_str(), p.x, p.y,
                    p.z, h ? std::to_string(h->point.y).c_str() : "none");
        CHECK(h && std::fabs(h->point.y - p.y) <= 1e-3f, "a ray down from 0.5 m above a spawn hits at its height (1 mm)");
        ++spawns;
    }
    CHECK(spawns == 3, "all three spawns checked");
}

static void t07_ray_through_doorway_passes() {
    Fixture f(kBlockout);
    const auto h = f.ray({1.5f, 1.0f, 2.5f}, {0.0f, 0.0f, 1.0f}, 3.0f);
    CHECK(!h, "a ray through the doorway at 1 m meets nothing");
}

static void t07_ray_beside_doorway_hits_wall_south_a() {
    Fixture f(kBlockout);
    const auto h = f.ray({0.6f, 1.0f, 2.5f}, {0.0f, 0.0f, 1.0f}, 3.0f);
    CHECK(h && h->entity == f.named("WallSouthA") && near(h->point.z, 3.0f) && nearVec(h->normal, {0.0f, 0.0f, -1.0f}),
          "0.3 m beside the doorway the ray hits WallSouthA's face at z 3.0");
}

static void t07_ray_on_ramp_returns_slope_normal() {
    Fixture f(kBlockout);
    const auto h = f.ray({2.7f, 2.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, 5.0f);
    const glm::vec3 slope = glm::normalize(glm::vec3(0.0f, 1.0f, 0.2f));   // 0.6 m up over 3 m toward -z
    CHECK(h && h->entity == f.named("Ramp") && near(h->point.y, 0.3f) && nearVec(h->normal, slope),
          "the ramp (a mesh collider) is hit at its height with the slope's normal");
}

static void t07_crates_come_to_rest_when_stepped() {
    Fixture f(kBlockout);
    auto& backend = f.physics->backend();
    std::vector<std::pair<ohao::physics::backend::BodyHandle, glm::vec3>> statics;
    for (const char* n : {"Floor", "WallNorth", "Step", "Platform", "Ramp", "Pillar"}) {
        const auto b = f.physics->bodyOf(f.named(n));
        CHECK(b != ohao::physics::backend::INVALID_BODY, "every static node has a body");
        statics.emplace_back(b, backend.getPosition(b));
    }
    const auto a = f.physics->bodyOf(f.named("CrateA"));
    const auto b = f.physics->bodyOf(f.named("CrateB"));
    CHECK(a != ohao::physics::backend::INVALID_BODY && b != ohao::physics::backend::INVALID_BODY, "the crates are bodies");
    for (int i = 0; i < 120; ++i) backend.step(1.0f / 60.0f);
    const glm::vec3 pa = backend.getPosition(a), pb = backend.getPosition(b);
    std::printf("\n      CrateA y %.4f, CrateB y %.4f after 120 steps", pa.y, pb.y);
    CHECK(near(pa.y, 0.3f, 0.02f) && near(pb.y, 0.8f, 0.02f), "the stacked crates rest at their authored heights (2 cm)");
    CHECK(near(pa.x, 1.3f, 0.02f) && near(pa.z, -1.7f, 0.02f) && near(pb.x, 1.3f, 0.02f) && near(pb.z, -1.7f, 0.02f),
          "without sliding off");
    for (const auto& [h, p] : statics) CHECK(backend.getPosition(h) == p, "no static body moved");
    const PhysicsInfo info = f.physics->info();
    CHECK(info.bodies.statics == 11 && info.bodies.dynamics == 2 && info.shapes.mesh == 1,
          "the blockout: 11 static bodies (one a mesh, the ramp) and the 2 crates");
}

static void t07_render_only_level_builds_no_bodies() {
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    ohao::level::installLevels(w, files, engineTypes());
    PhysicsScene& physics = installPhysics(w, files);
    const auto lv = w.createWithId(0x1e, "Level");
    w.registry().emplace<Level>(lv, Level{AssetRef{kBlockout, {}}, 1.0f, false});
    w.sync();
    CHECK(w.registry().view<Collider>().empty() && physics.info().bodies.statics == 0, "Level.collision false: no colliders");
    w.registry().patch<Level>(lv, [](Level& l) { l.collision = true; });
    w.sync();
    CHECK(physics.info().bodies.statics == 11 && physics.info().bodies.dynamics == 2, "turned on: the bodies are built");
    w.destroy(lv);
    w.sync();
    const PhysicsInfo after = physics.info();
    CHECK(after.bodies.statics == 0 && after.bodies.dynamics == 0, "destroying the level removes them");
}

static void t07_still_world_does_no_work() {
    Fixture f(kBlockout);
    const double built = f.physics->info().buildMs;
    const std::size_t rebuilds = f.physics->bodyBuilds();
    for (int i = 0; i < 10; ++i) f.world.sync();
    CHECK(f.physics->bodyBuilds() == rebuilds && f.physics->info().buildMs == built, "syncs of a still world build nothing");
    f.world.setTransform(f.named("Pillar"), Transform{{1.0f, 1.5f, -0.5f}});
    f.world.sync();
    const auto h = f.ray({1.0f, 5.0f, -0.5f}, {0.0f, -1.0f, 0.0f});
    CHECK(h && h->entity == f.named("Pillar") && f.physics->bodyBuilds() == rebuilds, "a moved node's body follows it, no rebuild");
}

// ---- test 10: physics in the world's fixed step (sub-project 6 step 1) ---------------------------
// docs/specs/2026-10-26-player-physics-design.md, "Test plan". The world steps as every host
// steps it: World::update(dt) is simulate(dt) (the physics simulate system) and sync().

namespace {

constexpr float kDt = 1.0f / 60.0f;
using Kind = PhysicsEvent::Kind;

// CrateA up 1 m and CrateB up 2 m from where they rest (stacked), so they start apart.
void liftCrates(Fixture& f) {
    for (const auto& [name, lift] : {std::pair{"CrateA", 1.0f}, std::pair{"CrateB", 2.0f}}) {
        const entt::entity e = f.named(name);
        Transform t = f.world.registry().get<Transform>(e);
        t.position.y += lift;
        f.world.setTransform(e, t);
    }
}

// steps world updates; every step's events, in order.
std::vector<PhysicsEvent> runSteps(World& w, int steps) {
    std::vector<PhysicsEvent> all;
    for (int i = 0; i < steps; ++i) {
        w.update(kDt);
        const auto ev = physicsEvents(w);
        all.insert(all.end(), ev.begin(), ev.end());
    }
    return all;
}

uint64_t idOf(const Fixture& f, const char* name) { return f.world.idOf(f.named(name)); }

// The first event of this kind between the two named entities (either order), or null.
const PhysicsEvent* findEvent(const std::vector<PhysicsEvent>& ev, Kind kind, uint64_t x, uint64_t y) {
    for (const PhysicsEvent& e : ev)
        if (e.kind == kind && ((e.aId == x && e.bId == y) || (e.aId == y && e.bId == x))) return &e;
    return nullptr;
}

glm::vec3 worldPos(const World& w, entt::entity e) { return glm::vec3(w.registry().get<WorldTransform>(e).world[3]); }

float tiltDeg(const World& w, entt::entity e) {
    const glm::vec3 up = glm::normalize(glm::vec3(w.registry().get<WorldTransform>(e).world[1]));
    return glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f)));
}

} // namespace

static void t10_crates_fall_and_rest() {
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 180);
    const entt::entity a = f.named("CrateA"), b = f.named("CrateB");
    const glm::vec3 pa = worldPos(f.world, a), pb = worldPos(f.world, b);
    std::printf("\n      CrateA (%.4f, %.4f, %.4f) tilt %.2f, CrateB (%.4f, %.4f, %.4f) tilt %.2f after 180 steps", pa.x, pa.y,
                pa.z, tiltDeg(f.world, a), pb.x, pb.y, pb.z, tiltDeg(f.world, b));
    CHECK(nearVec(pa, {1.3f, 0.3f, -1.7f}, 0.01f) && nearVec(pb, {1.3f, 0.8f, -1.7f}, 0.015f),
          "the dropped crates rest stacked at their authored places (Jolt's penetration slop is 5 mm a contact: 1 cm for "
          "CrateA, 1.5 cm for CrateB on top of it), in their entities' world transforms");
    CHECK(tiltDeg(f.world, a) < 1.0f && tiltDeg(f.world, b) < 1.0f, "upright (1 degree)");
    const Transform& ta = f.world.registry().get<Transform>(a);
    CHECK(nearVec(ta.position, pa, 1e-4f), "the entity's own Transform says so (the level root is at the origin)");
    auto& be = f.physics->backend();
    CHECK(!be.isAwake(f.physics->bodyOf(a)) && !be.isAwake(f.physics->bodyOf(b)), "both asleep");
    CHECK(f.physics->steps() == 180 && f.physics->info().steps == 180, "180 physics steps ran");
}

static void t10_contact_begin_events() {
    Fixture f(kBlockout);
    liftCrates(f);
    const auto ev = runSteps(f.world, 180);
    const uint64_t a = idOf(f, "CrateA"), b = idOf(f, "CrateB"), floor = idOf(f, "Floor");
    const PhysicsEvent* af = findEvent(ev, Kind::ContactBegin, a, floor);
    const PhysicsEvent* ab = findEvent(ev, Kind::ContactBegin, a, b);
    if (af) std::printf("\n      CrateA-Floor begins at step %llu, %.2f m/s", (unsigned long long)af->step, af->speed);
    if (ab) std::printf("\n      CrateA-CrateB begins at step %llu, %.2f m/s", (unsigned long long)ab->step, ab->speed);
    CHECK(af && af->step >= 26 && af->step <= 29 && af->speed >= 4.0f && af->speed <= 4.9f,
          "CrateA lands on the floor at step 26-29 at 4.0-4.9 m/s (a 1 m fall: 0.45 s, 4.43 m/s)");
    CHECK(ab && ab->step >= 36 && ab->step <= 40, "CrateB lands on CrateA at step 36-40 (a 2 m fall: 0.64 s)");
    CHECK(af && af->a == (a < floor ? f.named("CrateA") : f.named("Floor")) && af->aId < af->bId,
          "a contact's a is the lower entity id");
    CHECK(af && af->normal.y != 0.0f && std::fabs(glm::length(af->normal) - 1.0f) < 1e-3f && af->point.y > -0.05f &&
              af->point.y < 0.05f,
          "the begin's point is on the floor and its normal is a unit vector");
    bool lateEnd = false;
    for (const PhysicsEvent& e : ev)
        if (e.kind == Kind::ContactEnd && e.step > 60) lateEnd = true;
    CHECK(!lateEnd, "no contact ends while they rest");
    bool sorted = true;
    for (std::size_t i = 1; i < ev.size(); ++i) sorted &= ev[i - 1].seq < ev[i].seq && ev[i - 1].step <= ev[i].step;
    CHECK(sorted && !ev.empty() && ev.front().seq == 1, "events are numbered from 1 in step order");
}

static void t10_rest_writes_nothing() {
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 180);
    std::size_t patches = 0;
    struct Counter {
        std::size_t* n;
        void on(entt::registry&, entt::entity) { ++*n; }
    } counter{&patches};
    f.world.registry().on_update<Transform>().connect<&Counter::on>(counter);
    const std::size_t builds = f.physics->bodyBuilds();
    const auto ev = runSteps(f.world, 60);
    f.world.registry().on_update<Transform>().disconnect(&counter);
    CHECK(patches == 0 && f.physics->bodyBuilds() == builds && ev.empty() && f.physics->info().awake == 0,
          "a settled world: 60 steps patch no Transform, build no body, raise no event");
}

static void t10_edit_teleports_dynamic() {
    Fixture f(kBlockout);
    (void)runSteps(f.world, 60);   // the crates settle where they were authored
    const entt::entity b = f.named("CrateB");
    const std::size_t builds = f.physics->bodyBuilds();
    f.world.setTransform(b, Transform{{-1.0f, 1.0f, 1.5f}});
    f.world.update(kDt);
    auto& be = f.physics->backend();
    const auto h = f.physics->bodyOf(b);
    const glm::vec3 p = be.getPosition(h);
    CHECK(nearVec(p, {-1.0f, 1.0f, 1.5f}, 0.01f) && glm::length(be.getLinearVelocity(h)) < 0.2f,
          "an edited dynamic body jumps to its entity, from rest (one step of gravity at most)");
    (void)runSteps(f.world, 120);
    const glm::vec3 r = worldPos(f.world, b);
    std::printf("\n      CrateB rests at (%.4f, %.4f, %.4f)", r.x, r.y, r.z);
    CHECK(nearVec(r, {-1.0f, 0.2f, 1.5f}, 0.01f), "and falls to the floor there (its half height 0.2 m, 1 cm)");
    CHECK(f.physics->bodyBuilds() == builds, "moved, not rebuilt");
}

static void t10_two_steps_one_frame_motion() {
    Fixture f(kBlockout);
    liftCrates(f);
    for (int i = 0; i < 10; ++i) f.world.update(kDt);   // falling
    const entt::entity a = f.named("CrateA");
    const glm::mat4 start = f.world.registry().get<WorldTransform>(a).world;
    f.world.simulate(kDt);
    f.world.simulate(kDt);
    f.world.sync();
    const auto& wt = f.world.registry().get<WorldTransform>(a);
    CHECK(wt.prevWorld == start, "two steps in one frame: prevWorld is the frame's start, not the last step's");
    CHECK(wt.world[3].y < start[3].y - 0.01f, "and world is where the second step left it");
    f.world.sync();
    CHECK(f.world.registry().get<WorldTransform>(a).prevWorld == f.world.registry().get<WorldTransform>(a).world,
          "a frame without steps: motion returns to zero");
}

static void t10_destroy_ends_contacts() {
    Fixture f(kBlockout);
    (void)runSteps(f.world, 60);
    const uint64_t a = idOf(f, "CrateA"), b = idOf(f, "CrateB");
    CHECK(f.physics->info().contacts > 0, "the resting crates are in contact");
    f.world.destroy(f.named("CrateB"));
    f.world.update(kDt);
    const auto ev = physicsEvents(f.world);
    const std::vector<PhysicsEvent> list(ev.begin(), ev.end());
    const PhysicsEvent* end = findEvent(list, Kind::ContactEnd, a, b);
    CHECK(end != nullptr, "destroying CrateB ends its contact with CrateA");
    CHECK(end && end->b == entt::null && end->bId == b, "the gone entity is null, its id kept");
}

static void t10_trigger_enter_exit() {
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    w.seedIds(11);
    PhysicsScene& physics = installPhysics(w, files);
    auto& reg = w.registry();
    const entt::entity floor = w.create("Floor", Transform{{0.0f, -0.5f, 0.0f}});
    reg.emplace<Collider>(floor, Collider{ColliderShape::Box, {10.0f, 0.5f, 10.0f}});
    const entt::entity zone = w.create("Zone", Transform{{0.0f, 1.0f, 0.0f}});
    Collider trigger{ColliderShape::Box, {1.0f, 1.0f, 1.0f}};
    trigger.trigger = true;
    reg.emplace<Collider>(zone, trigger);
    const entt::entity ball = w.create("Ball", Transform{{0.0f, 4.0f, 0.0f}});
    reg.emplace<Collider>(ball, Collider{ColliderShape::Sphere, {0.25f, 0.0f, 0.0f}});
    reg.emplace<RigidBody>(ball, RigidBody{Motion::Dynamic, 1.0f});
    std::vector<PhysicsEvent> ev;
    for (int i = 0; i < 120; ++i) {
        w.update(kDt);
        const auto s = physicsEvents(w);
        ev.insert(ev.end(), s.begin(), s.end());
    }
    const uint64_t z = w.idOf(zone), bl = w.idOf(ball);
    const PhysicsEvent* enter = findEvent(ev, Kind::TriggerEnter, z, bl);
    if (enter) std::printf("\n      Ball enters Zone at step %llu", (unsigned long long)enter->step);
    CHECK(enter && enter->a == zone && enter->b == ball && enter->step >= 30 && enter->step <= 38,
          "trigger_enter (the trigger, the body) as the ball reaches the zone's top (1.75 m of fall: step ~36)");
    CHECK(near(worldPos(w, ball).y, 0.25f, 0.02f), "the trigger blocks nothing: the ball rests on the floor inside it");
    CHECK(!findEvent(ev, Kind::ContactBegin, z, bl) && findEvent(ev, Kind::ContactBegin, bl, w.idOf(floor)),
          "a trigger's overlap is not a contact; the floor's is");
    CHECK(physics.info().triggers == 1, "one sensor body");
    w.setTransform(ball, Transform{{5.0f, 0.3f, 5.0f}});
    std::vector<PhysicsEvent> after;
    for (int i = 0; i < 3; ++i) {
        w.update(kDt);
        const auto s = physicsEvents(w);
        after.insert(after.end(), s.begin(), s.end());
    }
    const PhysicsEvent* exit = findEvent(after, Kind::TriggerExit, z, bl);
    CHECK(exit && exit->a == zone, "teleported out: trigger_exit");
}

static void t10_substeps() {
    Fixture coarse(kBlockout), fine(kBlockout);
    liftCrates(coarse);
    liftCrates(fine);
    for (int i = 0; i < 6; ++i) coarse.world.update(0.05f);
    CHECK(coarse.physics->lastSubsteps() == 3, "a step of 0.05 s runs 3 Jolt updates");
    for (int i = 0; i < 18; ++i) fine.world.update(kDt);
    const glm::vec3 c = worldPos(coarse.world, coarse.named("CrateA")), d = worldPos(fine.world, fine.named("CrateA"));
    std::printf("\n      CrateA after 0.3 s: (%.5f, %.5f, %.5f) in 6 steps, (%.5f, %.5f, %.5f) in 18", c.x, c.y, c.z, d.x, d.y, d.z);
    CHECK(nearVec(c, d, 1e-3f), "and lands where 1/60 s steps do (1 mm)");
    CHECK(coarse.physics->steps() == 6, "the substeps are one step");
}

static void t10_runs_are_identical() {
    Fixture one(kBlockout), two(kBlockout);
    liftCrates(one);
    liftCrates(two);
    const auto e1 = runSteps(one.world, 120), e2 = runSteps(two.world, 120);
    bool same = e1.size() == e2.size();
    for (std::size_t i = 0; same && i < e1.size(); ++i)
        same = e1[i].kind == e2[i].kind && e1[i].aId == e2[i].aId && e1[i].bId == e2[i].bId && e1[i].step == e2[i].step &&
               e1[i].seq == e2[i].seq && e1[i].point == e2[i].point && e1[i].normal == e2[i].normal &&
               e1[i].speed == e2[i].speed;
    CHECK(same && !e1.empty(), "two runs of the same drop give the same events, bit for bit");
    bool poses = true;
    for (const char* n : {"CrateA", "CrateB"})
        poses &= one.world.registry().get<Transform>(one.named(n)).position == two.world.registry().get<Transform>(two.named(n)).position &&
                 one.world.registry().get<Transform>(one.named(n)).rotation == two.world.registry().get<Transform>(two.named(n)).rotation;
    CHECK(poses, "and the same poses");
}

static void t10_event_log_cursor() {
    Fixture f(kBlockout);
    liftCrates(f);
    const auto ev = runSteps(f.world, 120);
    uint64_t dropped = 99;
    const auto all = f.physics->eventsSince(0, 1000, dropped);
    CHECK(all.size() == ev.size() && dropped == 0 && f.physics->nextEventSeq() == ev.size() + 1,
          "the log holds every event so far, none dropped");
    const auto tail = f.physics->eventsSince(1, 1, dropped);
    CHECK(tail.size() == 1 && tail[0].seq == 2, "since 1, max 1: event 2");
}

// ---- test 11: the player (sub-project 6 step 2) ----------------------------------------------------
// docs/specs/2026-10-26-player-physics-design.md, "The player". A PlayerController entity walks the
// blockout on injected input (the virtual device: axes forced, actions held), one World::update a
// step as every host steps it. The blockout in engine axes: north is -Z (WallNorth's face at
// z -3.0), the Step x -3..-1.5 z -3..-1 at 0.3 m, the Platform x -4..-3 at 0.6 m, the Ramp x 2..3.4
// rising from z 0.5 (0 m) to z -2.5 (0.6 m), CrateA x 1.0..1.6 z -2.0..-1.4, SpawnPlayer at
// (1.5, 0, 2.2) facing -Z. The lane x = -1.0 is clear from z 2.5 to the north wall.

namespace {

InputEvent axisSet(const char* name, float value) {
    InputEvent e;
    e.type = InputEvent::Type::AxisSet;
    e.name = name;
    e.value = value;
    return e;
}

InputEvent actionEvent(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}

glm::quat yawRotation(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }

// The blockout with a player, spawned (one step), then placed where a test wants it.
struct PlayerFixture : Fixture {
    entt::entity player{entt::null};
    entt::entity head{entt::null};   // with `withHead`: a child holding the Camera (step 3)
    // stepFirst false: spawned by a sync, as scene.load leaves a play scene, and not stepped.
    explicit PlayerFixture(bool withHead = false, bool stepFirst = true) : Fixture(kBlockout) {
        player = world.createWithId(0xb1a7e2, "Player");
        world.registry().emplace<PlayerController>(player);
        if (withHead) {
            head = world.createWithId(0xb1a7e3, "Head");
            world.registry().emplace<Camera>(head, Camera{75.0f, 0.05f, 100.0f, true});
            world.setParent(head, player);
            // The level expanded before the head existed, so its camera took the view; a play scene
            // loads the head first and the level's camera stays inactive (the level system's rule).
            for (auto [e, cam, ln] : world.registry().view<Camera, const LevelNode>().each()) cam.active = false;
        }
        if (stepFirst) step();
        else world.sync();
    }
    // One world step with these input events (then none: actions stay held, axes stay set).
    void step(std::vector<InputEvent> events = {}) {
        world.stepInput(events);
        world.update(kDt);
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
    void place(glm::vec3 pos, float yawDeg) {
        world.setTransform(player, Transform{pos, yawRotation(yawDeg)});
        steps(10);   // settle onto the floor
    }
    [[nodiscard]] glm::vec3 pos() const { return world.registry().get<Transform>(player).position; }
    [[nodiscard]] glm::vec3 facing() const { return world.registry().get<Transform>(player).rotation * glm::vec3(0, 0, -1); }
};

} // namespace

static void t11_spawns_at_the_player_spawn_facing_its_yaw() {
    PlayerFixture f;
    const glm::vec3 p = f.pos();
    std::printf("\n      spawned at (%.4f, %.4f, %.4f) facing (%.3f, %.3f, %.3f)", p.x, p.y, p.z, f.facing().x, f.facing().y,
                f.facing().z);
    CHECK(nearVec(p, {1.5f, 0.0f, 2.2f}, 0.01f), "at SpawnPlayer (1.5, 0, 2.2), feet on the floor (1 cm)");
    CHECK(nearVec(f.facing(), {0.0f, 0.0f, -1.0f}, 1e-3f), "facing the spawn's -Z");
    f.steps(5);
    const auto* st = f.world.registry().try_get<PlayerState>(f.player);
    CHECK(st && st->onGround && near(st->yawDeg, 0.0f, 0.01f), "standing, yaw 0");

    // A spawn turned 90 degrees (facing -X), in a world without a level.
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    w.seedIds(3);
    PhysicsScene& physics = installPhysics(w, files);
    const entt::entity floor = w.create("Floor", Transform{{0.0f, -0.5f, 0.0f}});
    w.registry().emplace<Collider>(floor, Collider{ColliderShape::Box, {10.0f, 0.5f, 10.0f}});
    const entt::entity spawn = w.create("Spawn", Transform{{2.0f, 0.0f, -1.0f}, yawRotation(90.0f)});
    w.registry().emplace<SpawnPoint>(spawn, SpawnPoint{"player"});
    const entt::entity pl = w.create("Player");
    w.registry().emplace<PlayerController>(pl);
    w.stepInput({});
    w.update(kDt);
    const Transform& t = w.registry().get<Transform>(pl);
    CHECK(nearVec(t.position, {2.0f, 0.0f, -1.0f}, 0.01f) && nearVec(t.rotation * glm::vec3(0, 0, -1), {-1.0f, 0.0f, 0.0f}, 1e-3f),
          "a spawn yawed 90 degrees: the player faces -X");
    CHECK(physics.warnings().empty(), "no warning with a spawn");

    World bare;
    PhysicsScene& bp = installPhysics(bare, files);
    const entt::entity lone = bare.create("Player", Transform{{0.0f, 5.0f, 0.0f}});
    bare.registry().emplace<PlayerController>(lone);
    bare.stepInput({});
    bare.update(kDt);
    bool warned = false;
    for (const PhysicsWarning& wn : bp.warnings()) warned |= wn.code == "W_PLAYER_NO_SPAWN" && wn.entity == lone;
    CHECK(warned && bare.registry().get<Transform>(lone).position.y < 5.0f, "no spawn: W_PLAYER_NO_SPAWN, and it falls from where it is");
}

static void t11_walks_four_metres_in_sixty_steps() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(14);   // up to speed (40 m/s^2: 0.1 s)
    const glm::vec3 a = f.pos();
    f.steps(60);
    const glm::vec3 b = f.pos();
    std::printf("\n      60 steps: (%.4f, %.4f, %.4f) -> (%.4f, %.4f, %.4f), %.4f m", a.x, a.y, a.z, b.x, b.y, b.z, a.z - b.z);
    CHECK(near(a.z - b.z, 4.0f, 0.05f), "forward at walk speed: 4.0 m in 60 steps (5 cm)");
    CHECK(near(b.x, -1.0f, 0.01f) && near(b.y, 0.0f, 0.02f), "straight, on the floor");
    const auto& st = f.world.registry().get<PlayerState>(f.player);
    CHECK(near(glm::length(glm::vec2(st.velocity.x, st.velocity.z)), 4.0f, 0.05f) && st.onGround, "at 4 m/s, on the ground");

    PlayerFixture g;   // the same input gives the same walk
    g.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    g.step({axisSet("move_forward", 1.0f)});
    g.steps(74);
    CHECK(g.pos() == b, "two runs walk to the same place, bit for bit");
}

static void t11_the_north_wall_stops_the_player() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(150);
    const glm::vec3 p = f.pos();
    std::printf("\n      stopped at (%.4f, %.4f, %.4f)", p.x, p.y, p.z);
    CHECK(p.z > -2.75f && p.z < -2.6f, "against WallNorth (its face z -3.0, the capsule's radius 0.3)");
    CHECK(near(p.x, -1.0f, 0.02f), "without sliding sideways");
}

static void t11_climbs_the_step_and_the_platform() {
    PlayerFixture f;
    f.place({-0.8f, 0.0f, -2.0f}, 90.0f);   // facing -X, toward the Step
    CHECK(nearVec(f.facing(), {-1.0f, 0.0f, 0.0f}, 1e-3f), "facing -X");
    f.step({axisSet("move_forward", 1.0f)});
    float onStep = -1.0f;
    for (int i = 0; i < 150; ++i) {
        f.step();
        const glm::vec3 p = f.pos();
        if (p.x < -2.0f && p.x > -2.6f) onStep = p.y;
    }
    const glm::vec3 p = f.pos();
    std::printf("\n      on the Step y %.4f; ends at (%.4f, %.4f, %.4f)", onStep, p.x, p.y, p.z);
    CHECK(near(onStep, 0.3f, 0.02f), "up the 0.3 m Step");
    CHECK(near(p.y, 0.6f, 0.02f) && p.x > -3.75f && p.x < -3.6f, "up the Platform (0.6 m), against WallWest");
}

static void t11_climbs_the_ramp() {
    PlayerFixture f;
    f.place({2.7f, 0.0f, 1.5f}, 0.0f);
    f.step({axisSet("move_forward", 1.0f)});
    float y = -1.0f, z = 0.0f;
    for (int i = 0; i < 90 && f.pos().z > -1.0f; ++i) {
        f.step();
        y = f.pos().y;
        z = f.pos().z;
    }
    const float want = 0.6f * (0.5f - z) / 3.0f;
    std::printf("\n      at z %.4f: y %.4f (the ramp %.4f)", z, y, want);
    CHECK(z <= -1.0f && near(y, want, 0.03f), "up the ramp: on its surface (3 cm)");
}

static void t11_pushes_a_crate() {
    PlayerFixture f;   // at the spawn, facing CrateA (x 1.0..1.6; the player's x 1.5 is off its centre)
    const glm::vec3 crate0 = worldPos(f.world, f.named("CrateA"));
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(79);   // it reaches the crate's face at about step 52
    const glm::vec3 crate1 = worldPos(f.world, f.named("CrateA")), p = f.pos();
    std::printf("\n      step 80: CrateA (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f); player at z %.3f", crate0.x, crate0.y,
                crate0.z, crate1.x, crate1.y, crate1.z, p.z);
    CHECK(crate1.z < crate0.z - 0.2f, "walking into CrateA pushes it north (20 cm or more)");
    CHECK(p.z > crate1.z + 0.55f, "with the player still behind it (the crate's half depth and the capsule's radius)");
    CHECK(p.z < 2.2f - 3.3f + 0.05f, "and the player came through the crate's old place: it is pushing, not stopped at the face");
}

static void t11_jumps_one_point_two_two_metres() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 1.0f}, 0.0f);
    const float ground = f.pos().y;
    f.step({actionEvent("jump", true)});
    f.step({actionEvent("jump", false)});
    float apex = f.pos().y;
    for (int i = 0; i < 80; ++i) {
        f.step();
        apex = std::max(apex, f.pos().y);
    }
    std::printf("\n      apex %.4f m above %.4f; lands at %.4f", apex - ground, ground, f.pos().y);
    CHECK(near(apex - ground, 1.22f, 0.05f), "a jump rises 1.22 m (5 cm)");
    CHECK(near(f.pos().y, ground, 0.01f) && f.world.registry().get<PlayerState>(f.player).onGround, "and lands");
}

static void t11_a_trigger_sees_the_player() {
    PlayerFixture f;
    const entt::entity zone = f.world.create("Zone", Transform{{-1.0f, 1.0f, 0.0f}});
    Collider c{ColliderShape::Box, {0.5f, 1.0f, 0.5f}};
    c.trigger = true;
    f.world.registry().emplace<Collider>(zone, c);
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    std::vector<PhysicsEvent> ev;
    f.step({axisSet("move_forward", 1.0f)});
    for (int i = 0; i < 90; ++i) {
        f.step();
        const auto s = physicsEvents(f.world);
        ev.insert(ev.end(), s.begin(), s.end());
    }
    const uint64_t z = f.world.idOf(zone), pl = f.world.idOf(f.player);
    const PhysicsEvent* enter = findEvent(ev, Kind::TriggerEnter, z, pl);
    const PhysicsEvent* exit = findEvent(ev, Kind::TriggerExit, z, pl);
    CHECK(enter && enter->a == zone && enter->b == f.player, "trigger_enter (the zone, the player) through its inner body");
    CHECK(exit && enter && exit->step > enter->step, "and trigger_exit once it walks out");
    const auto hit = f.ray({-1.0f, 3.0f, f.pos().z}, {0.0f, -1.0f, 0.0f});
    CHECK(hit && hit->entity == f.player, "a ray down onto the player hits it");
}

static void t11_an_edited_controller_keeps_the_player_where_it_is() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 90.0f);
    const glm::vec3 before = f.pos();
    f.world.registry().patch<PlayerController>(f.player, [](PlayerController& pc) { pc.walkSpeed = 2.0f; });
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(39);
    const glm::vec3 p = f.pos();
    std::printf("\n      from (%.3f, %.3f, %.3f) at 2 m/s for 40 steps: (%.3f, %.3f, %.3f)", before.x, before.y, before.z, p.x,
                p.y, p.z);
    CHECK(near(p.z, before.z, 0.01f) && p.x < before.x - 1.2f && p.x > before.x - 1.4f,
          "walkSpeed 2 from where it stood, still facing -X: not back at the spawn (1.33 m in 40 steps, less the start)");
}

static void t11_kinematic_bodies_move_by_velocity() {
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    w.seedIds(5);
    PhysicsScene& physics = installPhysics(w, files);
    auto& reg = w.registry();
    const entt::entity floor = w.create("Floor", Transform{{0.0f, -0.5f, 0.0f}});
    reg.emplace<Collider>(floor, Collider{ColliderShape::Box, {20.0f, 0.5f, 20.0f}});
    const entt::entity pusher = w.create("Pusher", Transform{{0.0f, 0.5f, 0.0f}});
    reg.emplace<Collider>(pusher, Collider{ColliderShape::Box, {0.5f, 0.5f, 0.5f}});
    reg.emplace<RigidBody>(pusher, RigidBody{Motion::Kinematic});
    const entt::entity box = w.create("Box", Transform{{1.0f, 0.4f, 0.0f}});
    reg.emplace<Collider>(box, Collider{ColliderShape::Box, {0.4f, 0.4f, 0.4f}});
    reg.emplace<RigidBody>(box, RigidBody{Motion::Dynamic, 1.0f, 0.0f});
    w.update(kDt);
    for (int i = 1; i <= 90; ++i) {
        w.setTransform(pusher, Transform{{i / 60.0f, 0.5f, 0.0f}});   // 1 m/s along +X
        w.update(kDt);
    }
    const glm::vec3 bp = worldPos(w, box);
    const glm::vec3 v = physics.backend().getLinearVelocity(physics.bodyOf(box));
    std::printf("\n      pusher at x 1.5; box at x %.4f moving %.3f m/s", bp.x, v.x);
    CHECK(bp.x > 2.3f && bp.x < 2.45f, "a kinematic body moved by its entity pushes a box ahead of it (no gap, no overlap)");
    CHECK(near(v.x, 1.0f, 0.15f), "at its own speed: it moves by velocity, not by teleports");
    CHECK(glm::length(physics.backend().getLinearVelocity(physics.bodyOf(pusher))) == 0.0f, "and stops when its entity stops");
}

// ---- test 12: the camera (sub-project 6 step 3) ----------------------------------------------------
// docs/specs/2026-10-26-player-physics-design.md, "The camera": the player's Head child holds the
// Camera at eyeHeight, pitched by look_y within 89 degrees; yaw turns the body. Render interpolation
// draws movers between their last two step poses (World::setRenderAlpha) without touching what is
// saved or hashed.

namespace {

glm::vec3 forwardOf(const glm::quat& q) { return q * glm::vec3(0.0f, 0.0f, -1.0f); }

uint64_t hashOf(const World& w) {
    Snapshot table;
    return hashWorld(w, engineTypes(), table);
}

std::string savedText(const World& w) {
    auto text = saveScene(w, engineTypes());
    return text ? text.value() : std::string("<save failed>");
}

} // namespace

static void t12_head_at_eye_height() {
    PlayerFixture f(true);
    const Transform& h = f.world.registry().get<Transform>(f.head);
    std::printf("\n      head local (%.4f, %.4f, %.4f)", h.position.x, h.position.y, h.position.z);
    CHECK(nearVec(h.position, {0.0f, 1.62f, 0.0f}, 1e-6f) && nearVec(forwardOf(h.rotation), {0.0f, 0.0f, -1.0f}, 1e-6f),
          "the head sits at eyeHeight above the feet, level");
    const auto view = resolveSceneView(f.world).camera;
    CHECK(view && nearVec(view->position, {1.5f, 1.62f, 2.2f}, 0.01f) && near(view->pitchDeg, 0.0f, 1e-3f) &&
              near(view->yawDeg, -90.0f, 1e-3f),
          "the view is the spawn's, 1.62 m up, facing -Z");
    CHECK(view && near(view->fovDeg, 75.0f) && near(view->nearPlane, 0.05f), "through the head's Camera (fov 75, near 0.05)");
    f.world.registry().patch<PlayerController>(f.player, [](PlayerController& pc) { pc.eyeHeight = 1.5f; });
    f.step();
    CHECK(near(f.world.registry().get<Transform>(f.head).position.y, 1.5f, 1e-6f), "a changed eyeHeight moves the head");
}

static void t12_pitch_clamps_at_89() {
    PlayerFixture f(true);
    f.step({axisSet("look_y", 200.0f)});   // 20 degrees down a step
    f.steps(9);
    const auto& reg = f.world.registry();
    const float down = reg.get<PlayerState>(f.player).pitchDeg;
    const glm::vec3 headFwd = forwardOf(reg.get<Transform>(f.head).rotation);
    std::printf("\n      down: pitch %.3f, head forward (%.4f, %.4f, %.4f)", down, headFwd.x, headFwd.y, headFwd.z);
    CHECK(near(down, -89.0f, 1e-4f), "looking down stops at -89 degrees");
    CHECK(nearVec(headFwd, {0.0f, -std::sin(glm::radians(89.0f)), -std::cos(glm::radians(89.0f))}, 1e-5f),
          "the head is pitched by it");
    CHECK(near(forwardOf(reg.get<Transform>(f.player).rotation).y, 0.0f, 1e-6f), "the body is not");
    const auto view = resolveSceneView(f.world).camera;
    CHECK(view && near(view->pitchDeg, -89.0f, 1e-3f), "the view looks 89 degrees down");
    f.step({axisSet("look_y", -400.0f)});
    f.steps(9);
    CHECK(near(reg.get<PlayerState>(f.player).pitchDeg, 89.0f, 1e-4f), "and up stops at +89");
    f.step({axisSet("look_y", 0.0f)});
    f.world.setTransform(f.head, Transform{{0.0f, 1.62f, 0.0f}, glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0))});
    f.step();
    CHECK(near(reg.get<PlayerState>(f.player).pitchDeg, 30.0f, 1e-3f), "an edited head rotation sets the pitch");
}

static void t12_yaw_turns_the_body() {
    PlayerFixture f(true);
    f.step({axisSet("look_x", 100.0f)});   // 10 degrees right a step
    f.steps(8);
    f.step({axisSet("look_x", 0.0f)});
    const auto& reg = f.world.registry();
    const glm::vec3 body = forwardOf(reg.get<Transform>(f.player).rotation);
    const glm::vec3 headLocal = forwardOf(reg.get<Transform>(f.head).rotation);
    std::printf("\n      body forward (%.4f, %.4f, %.4f), yaw %.3f", body.x, body.y, body.z, reg.get<PlayerState>(f.player).yawDeg);
    CHECK(nearVec(body, {1.0f, 0.0f, 0.0f}, 1e-5f), "90 degrees right: the body faces +X");
    CHECK(nearVec(headLocal, {0.0f, 0.0f, -1.0f}, 1e-6f), "the head turns with it, not by itself");
    const auto view = resolveSceneView(f.world).camera;
    CHECK(view && near(view->yawDeg, 0.0f, 1e-3f) && near(view->pitchDeg, 0.0f, 1e-3f), "the view faces +X");
    const glm::vec3 a = f.pos();
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(20);
    CHECK(f.pos().x > a.x + 1.0f && near(f.pos().z, a.z, 0.01f), "and forward is +X now");
}

static void t12_interpolation_leaves_hash_and_save() {
    PlayerFixture f(true);
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    auto& reg = f.world.registry();
    // CrateA lifted 1.5 m: it falls while the player walks.
    const entt::entity crate = f.named("CrateA");
    const Transform c0 = reg.get<Transform>(crate);
    f.world.setTransform(crate, Transform{c0.position + glm::vec3(0.0f, 1.5f, 0.0f), c0.rotation, c0.scale});
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(10);
    const glm::vec3 eyePrev = glm::vec3(reg.get<WorldTransform>(f.head).world[3]);
    const glm::vec3 cratePrev = glm::vec3(reg.get<WorldTransform>(crate).world[3]);
    f.step();
    const glm::vec3 eyeCur = glm::vec3(reg.get<WorldTransform>(f.head).world[3]);
    const glm::vec3 crateCur = glm::vec3(reg.get<WorldTransform>(crate).world[3]);
    const uint64_t hash = hashOf(f.world);
    const std::string saved = savedText(f.world);
    const glm::mat4 playerWorld = reg.get<WorldTransform>(f.player).world;

    f.world.setRenderAlpha(0.5f);
    f.world.sync();
    const auto* headPose = reg.try_get<RenderPose>(f.head);
    const auto* cratePose = reg.try_get<RenderPose>(crate);
    const glm::vec3 eyeMid = glm::mix(eyePrev, eyeCur, 0.5f), crateMid = glm::mix(cratePrev, crateCur, 0.5f);
    std::printf("\n      eye (%.4f, %.4f, %.4f) -> (%.4f, %.4f, %.4f); drawn at (%.4f, %.4f, %.4f)", eyePrev.x, eyePrev.y,
                eyePrev.z, eyeCur.x, eyeCur.y, eyeCur.z, headPose ? headPose->world[3].x : 0.0f,
                headPose ? headPose->world[3].y : 0.0f, headPose ? headPose->world[3].z : 0.0f);
    CHECK(eyeCur.z < eyePrev.z - 0.05f && crateCur.y < cratePrev.y - 0.01f, "the player walks and the crate falls");
    CHECK(headPose && nearVec(glm::vec3(headPose->world[3]), eyeMid, 1e-4f), "alpha 0.5 draws the head halfway between its steps");
    CHECK(cratePose && nearVec(glm::vec3(cratePose->world[3]), crateMid, 1e-4f), "and the falling crate halfway");
    const auto view = resolveSceneView(f.world).camera;
    CHECK(view && nearVec(view->position, eyeMid, 1e-4f), "the camera views from the drawn head");
    CHECK(reg.get<WorldTransform>(f.player).world == playerWorld, "WorldTransform keeps the step's pose");
    CHECK(hashOf(f.world) == hash, "world.hash is the step's");
    CHECK(savedText(f.world) == saved, "the saved file is the step's, byte for byte");

    f.world.setRenderAlpha(1.0f);
    f.world.sync();
    CHECK(reg.view<RenderPose>().empty(), "alpha 1 draws the step's poses: no render pose is left");
    const auto exact = resolveSceneView(f.world).camera;
    CHECK(exact && nearVec(exact->position, eyeCur, 1e-5f), "and the camera is at the step's eye");
}

static void t12_play_scene_loads_with_head_camera() {
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    ohao::level::installLevels(w, files, engineTypes());
    installPhysics(w, files);
    const auto loaded = loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json",
                                      engineTypes());
    CHECK(loaded, "blockout_play.scene.json loads");
    if (!loaded) return;
    w.sync();
    auto& reg = w.registry();
    entt::entity player = entt::null, camera = entt::null;
    std::size_t active = 0;
    std::size_t players = 0;   // moved by input; the scene's enemies (SP7) are characters moved by their AI
    for (auto [e, pc] : reg.view<const PlayerController>().each())
        if (pc.input) player = e, ++players;
    for (auto [e, cam] : reg.view<const Camera>().each())
        if (cam.active) camera = e, ++active;
    const auto* parent = camera != entt::null ? reg.try_get<Parent>(camera) : nullptr;
    CHECK(player != entt::null && players == 1, "one player");
    CHECK(active == 1 && parent && parent->entity == player, "one active Camera, the player's child (the level's stays off)");
    const auto view = resolveSceneView(w).camera;
    std::printf("\n      view at (%.4f, %.4f, %.4f) pitch %.3f yaw %.3f", view ? view->position.x : 0.0f,
                view ? view->position.y : 0.0f, view ? view->position.z : 0.0f, view ? view->pitchDeg : 0.0f,
                view ? view->yawDeg : 0.0f);
    CHECK(view && nearVec(view->position, {1.5f, 1.62f, 2.2f}, 0.01f) && near(view->yawDeg, -90.0f, 1e-3f) &&
              near(view->pitchDeg, 0.0f, 1e-3f),
          "before any step, the view is the player spawn's at eye height, facing -Z");
    CHECK(view && near(view->fovDeg, 75.0f) && near(view->nearPlane, 0.05f), "fov 75, near 0.05");
    for (int i = 0; i < 30; ++i) {
        w.stepInput({});
        w.update(kDt);
    }
    const auto later = resolveSceneView(w).camera;
    CHECK(later && nearVec(later->position, {1.5f, 1.62f, 2.2f}, 0.01f), "standing still, it stays there");
    CHECK(reg.get<PlayerState>(player).onGround, "on the floor");
}

// ---- test 13: footsteps (sub-project 6 step 4) -----------------------------------------------------
// docs/specs/2026-10-26-player-physics-design.md, "Footsteps": a footstep every stride (0.75 m
// walking, 0.95 m sprinting) while on the ground and moving, a land after a fall of more than
// 0.3 m, each with the surface of the ground body (Collider.surface; the blockout's hints: the
// Floor concrete, the Step, Platform and Ramp metal, the crates wood), in the physics event log.

namespace {

using Kind = PhysicsEvent::Kind;

// n steps; the first with `first`; the player's footstep and land events, in order.
std::vector<PhysicsEvent> playerEvents(PlayerFixture& f, int n, std::vector<InputEvent> first = {}) {
    std::vector<PhysicsEvent> out;
    for (int i = 0; i < n; ++i) {
        f.step(i == 0 ? std::move(first) : std::vector<InputEvent>{});
        for (const PhysicsEvent& e : physicsEvents(f.world))
            if (e.kind == Kind::Footstep || e.kind == Kind::Land) out.push_back(e);
    }
    return out;
}

std::vector<PhysicsEvent> only(const std::vector<PhysicsEvent>& ev, Kind k) {
    std::vector<PhysicsEvent> out;
    for (const PhysicsEvent& e : ev)
        if (e.kind == k) out.push_back(e);
    return out;
}

// Distances between consecutive events' points, in the ground plane.
std::vector<float> spacing(const std::vector<PhysicsEvent>& ev) {
    std::vector<float> out;
    for (std::size_t i = 1; i < ev.size(); ++i)
        out.push_back(glm::length(glm::vec2(ev[i].point.x - ev[i - 1].point.x, ev[i].point.z - ev[i - 1].point.z)));
    return out;
}

} // namespace

static void t13_walk_strides_0_75_m() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    const float z0 = f.pos().z;
    const auto ev = playerEvents(f, 75, {axisSet("move_forward", 1.0f)});
    const auto steps = only(ev, Kind::Footstep);
    const float walked = z0 - f.pos().z;
    const auto gaps = spacing(steps);
    std::printf("\n      walked %.3f m: %zu footsteps", walked, steps.size());
    for (const float g : gaps) std::printf(" %.3f", g);
    CHECK(std::fabs(float(steps.size()) - walked / 0.75f) < 1.0f, "a footstep every 0.75 m walked");
    bool even = !gaps.empty();
    for (const float g : gaps) even &= std::fabs(g - 0.75f) <= 4.0f / 60.0f + 1e-3f;
    CHECK(even, "0.75 m apart (within the step's 6.7 cm)");
    bool floor = !steps.empty();
    for (const PhysicsEvent& e : steps)
        floor &= e.surface == "concrete" && e.a == f.player && e.b == f.named("Floor") && e.point.y < 0.01f;
    CHECK(floor, "each on the Floor's concrete, at the player's feet");
    CHECK(!steps.empty() && near(steps.back().speed, 4.0f, 0.05f), "at walking speed");
    CHECK(only(ev, Kind::Land).empty(), "no land on flat ground");
}

static void t13_sprint_strides_0_95_m() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    const float z0 = f.pos().z;
    const auto steps = only(playerEvents(f, 45, {axisSet("move_forward", 1.0f), actionEvent("sprint", true)}), Kind::Footstep);
    const float ran = z0 - f.pos().z;
    const auto gaps = spacing(steps);
    std::printf("\n      ran %.3f m: %zu footsteps", ran, steps.size());
    for (const float g : gaps) std::printf(" %.3f", g);
    CHECK(std::fabs(float(steps.size()) - ran / 0.95f) < 1.0f, "a footstep every 0.95 m sprinted");
    bool even = !gaps.empty();
    for (const float g : gaps) even &= std::fabs(g - 0.95f) <= 6.5f / 60.0f + 1e-3f;
    CHECK(even, "0.95 m apart (within the step's 10.8 cm)");
    CHECK(!steps.empty() && near(steps.back().speed, 6.5f, 0.05f), "at sprinting speed");
}

static void t13_surface_from_ground_body() {
    PlayerFixture f;
    f.place({0.5f, 0.0f, -2.0f}, 90.0f);   // facing -X: 2 m of Floor, then the Step and the Platform
    const auto steps = only(playerEvents(f, 90, {axisSet("move_forward", 1.0f)}), Kind::Footstep);
    std::string seen;
    for (const PhysicsEvent& e : steps) seen += f.world.registry().get<Name>(e.b).value + ":" + e.surface + " ";
    std::printf("\n      %s", seen.c_str());
    bool floorFirst = !steps.empty() && steps.front().surface == "concrete", metal = false;
    for (const PhysicsEvent& e : steps) {
        const std::string& name = f.world.registry().get<Name>(e.b).value;
        if ((name == "Step" || name == "Platform") && e.surface == "metal") metal = true;
        if (name == "Floor" && e.surface != "concrete") floorFirst = false;
    }
    CHECK(floorFirst && metal, "concrete on the Floor, metal on the Step and the Platform (their surf hints)");

    // A Collider's surface, edited: the next footsteps there report it.
    f.world.registry().patch<Collider>(f.named("Floor"), [](Collider& c) { c.surface = "gravel"; });
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    const auto after = only(playerEvents(f, 40, {axisSet("move_forward", 1.0f)}), Kind::Footstep);
    CHECK(!after.empty() && after.back().surface == "gravel", "an edited Collider.surface is what the footsteps report");

    // A collider without a surface is "default".
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    installPhysics(w, files);
    const entt::entity ground = w.create("Ground", Transform{{0.0f, -0.5f, 0.0f}});
    w.registry().emplace<Collider>(ground, Collider{ColliderShape::Box, {20.0f, 0.5f, 20.0f}});
    const entt::entity pl = w.create("Player");
    w.registry().emplace<PlayerController>(pl);
    std::vector<PhysicsEvent> ev;
    for (int i = 0; i < 40; ++i) {
        w.stepInput(i == 0 ? std::vector<InputEvent>{axisSet("move_forward", 1.0f)} : std::vector<InputEvent>{});
        w.update(kDt);
        for (const PhysicsEvent& e : physicsEvents(w))
            if (e.kind == Kind::Footstep) ev.push_back(e);
    }
    CHECK(!ev.empty() && ev.front().surface == "default", "a collider without a surface: \"default\"");
}

static void t13_land_after_a_drop() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 1.0f}, 0.0f);
    f.world.setTransform(f.player, Transform{{-1.0f, 0.5f, 1.0f}, yawRotation(0.0f)});   // 0.5 m up
    const auto ev = playerEvents(f, 40);
    const auto lands = only(ev, Kind::Land);
    const float v = std::sqrt(2.0f * 9.81f * 0.5f);
    std::printf("\n      0.5 m: %zu land(s), speed %.3f (free fall %.3f)", lands.size(), lands.empty() ? 0.0f : lands[0].speed, v);
    CHECK(lands.size() == 1 && near(lands[0].speed, v, 0.25f) && lands[0].surface == "concrete" && lands[0].a == f.player,
          "a 0.5 m drop: one land, at the free-fall speed, on concrete");
    CHECK(only(ev, Kind::Footstep).empty(), "and no footstep standing");

    f.world.setTransform(f.player, Transform{{-1.0f, 0.2f, 1.0f}, yawRotation(0.0f)});   // 0.2 m: under 0.3
    CHECK(only(playerEvents(f, 30), Kind::Land).empty(), "a 0.2 m drop: no land");

    const auto jump = playerEvents(f, 80, {actionEvent("jump", true)});
    const auto jl = only(jump, Kind::Land);
    CHECK(jl.size() == 1 && near(jl[0].speed, 4.9f, 0.3f), "a jump (1.22 m) lands once, at its take-off speed");
}

static void t13_none_while_still_or_airborne() {
    PlayerFixture f;
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    CHECK(playerEvents(f, 60).empty(), "standing still: no footstep, no land");
    // Walking, then a jump mid-stride: nothing while in the air, a land on touching down.
    auto ev = playerEvents(f, 20, {axisSet("move_forward", 1.0f)});
    const std::size_t before = ev.size();
    const uint64_t takeoff = f.physics->steps() + 1;
    const auto air = playerEvents(f, 60, {actionEvent("jump", true)});
    uint64_t landed = 0;
    for (const PhysicsEvent& e : air)
        if (e.kind == Kind::Land && !landed) landed = e.step;
    bool quiet = landed != 0;
    for (const PhysicsEvent& e : air)
        if (e.kind == Kind::Footstep && e.step >= takeoff && e.step < landed) quiet = false;
    std::printf("\n      %zu events walking; jump at step %llu, landed at %llu", before, static_cast<unsigned long long>(takeoff),
                static_cast<unsigned long long>(landed));
    CHECK(before > 0 && quiet, "no footstep between take-off and landing");
}

// ---- test 14: replay through snapshots (sub-project 6 step 5) ------------------------------------
// docs/specs/2026-10-26-player-physics-design.md, "Snapshots and replay". BodyMotion holds what the
// saved components do not: a body's velocities and sleep, a player's velocity. A snapshot taken
// mid-motion, restored and stepped again, lands where the first run did, bit for bit.

namespace {

bool samePose(const Transform& a, const Transform& b) { return a.position == b.position && a.rotation == b.rotation; }

} // namespace

static void t14_body_motion_follows_awake_bodies() {
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 10);
    auto& reg = f.world.registry();
    auto& be = f.physics->backend();
    const entt::entity a = f.named("CrateA");
    const auto* m = reg.try_get<BodyMotion>(a);
    if (m) std::printf("\n      CrateA after 10 steps: linear (%.4f, %.4f, %.4f)", m->linear.x, m->linear.y, m->linear.z);
    CHECK(m && !m->asleep && near(m->linear.y, -9.81f * 10.0f * kDt, 0.01f), "CrateA falling: its BodyMotion is -g t, awake");
    CHECK(m && m->linear == be.getLinearVelocity(f.physics->bodyOf(a)) && m->angular == be.getAngularVelocity(f.physics->bodyOf(a)),
          "exactly its body's velocities");
    CHECK(!reg.all_of<BodyMotion>(f.named("Floor")), "a static body has none");
    (void)runSteps(f.world, 170);
    m = reg.try_get<BodyMotion>(a);
    CHECK(m && m->asleep && m->linear == glm::vec3(0.0f) && m->angular == glm::vec3(0.0f), "settled: asleep, at rest");
    std::size_t writes = 0;
    struct Counter {
        std::size_t* n;
        void on(entt::registry&, entt::entity) { ++*n; }
    } counter{&writes};
    reg.on_update<BodyMotion>().connect<&Counter::on>(counter);
    (void)runSteps(f.world, 60);
    reg.on_update<BodyMotion>().disconnect(&counter);
    CHECK(writes == 0, "a settled world writes no BodyMotion");
}

static void t14_body_motion_is_neither_saved_nor_hashed() {
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 10);
    const uint64_t hash = hashOf(f.world);
    const std::string saved = savedText(f.world);
    CHECK(saved.find("BodyMotion") == std::string::npos, "runtime state: never saved");
    f.world.registry().patch<BodyMotion>(f.named("CrateA"), [](BodyMotion& m) { m.linear.y = 3.0f; });
    CHECK(hashOf(f.world) == hash && savedText(f.world) == saved, "and not in world.hash: the hash is of the saved state");
    const Snapshot snap = takeSnapshot(f.world, engineTypes());
    bool captured = false;
    for (const SnapshotColumn& c : snap.columns) captured |= c.type == engineTypes().find<BodyMotion>();
    CHECK(captured, "but a snapshot holds it");
}

static void t14_restore_mid_fall_is_exact() {
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 15);   // CrateA lands at step 26-29: both crates are in the air
    const Snapshot mid = takeSnapshot(f.world, engineTypes());
    const uint64_t midHash = hashOf(f.world);
    (void)runSteps(f.world, 60);
    auto& reg = f.world.registry();
    const entt::entity a = f.named("CrateA"), b = f.named("CrateB");
    const Transform a1 = reg.get<Transform>(a), b1 = reg.get<Transform>(b);
    const uint64_t hash1 = hashOf(f.world);
    restoreSnapshot(f.world, mid);
    CHECK(hashOf(f.world) == midHash, "restored to the snapshot's hash");
    (void)runSteps(f.world, 60);
    const Transform a2 = reg.get<Transform>(a), b2 = reg.get<Transform>(b);
    std::printf("\n      CrateA y %.6f / %.6f, CrateB y %.6f / %.6f", a1.position.y, a2.position.y, b1.position.y, b2.position.y);
    CHECK(samePose(a1, a2) && samePose(b1, b2), "the same 60 steps from the restored snapshot: the same poses, bit for bit");
    CHECK(hashOf(f.world) == hash1, "and the same world.hash");
}

static void t14_restore_puts_bodies_back_to_sleep() {
    Fixture f(kBlockout);
    (void)runSteps(f.world, 120);   // the crates settle and sleep
    auto& be = f.physics->backend();
    const entt::entity b = f.named("CrateB");
    const Snapshot rest = takeSnapshot(f.world, engineTypes());
    f.world.setTransform(b, Transform{{-1.0f, 1.5f, 1.5f}});
    (void)runSteps(f.world, 5);   // CrateB falls again, awake
    CHECK(be.isAwake(f.physics->bodyOf(b)), "an edited crate wakes");
    restoreSnapshot(f.world, rest);
    f.world.update(kDt);
    CHECK(!be.isAwake(f.physics->bodyOf(b)) && glm::length(be.getLinearVelocity(f.physics->bodyOf(b))) == 0.0f,
          "restored: asleep at rest, as it was");
    CHECK(f.physics->info().awake == 0, "nothing awake");
}

static void t14_restore_mid_jump_is_exact() {
    PlayerFixture f(true);
    f.place({-1.0f, 0.0f, 1.5f}, 0.0f);
    auto& reg = f.world.registry();
    f.step({axisSet("move_forward", 1.0f), actionEvent("jump", true)});
    f.step({actionEvent("jump", false)});
    f.steps(8);   // up and moving: horizontal speed and a vertical velocity the saved state does not hold
    CHECK(!reg.get<PlayerState>(f.player).onGround, "in the air");
    const Snapshot mid = takeSnapshot(f.world, engineTypes());
    f.steps(50);
    const Transform p1 = reg.get<Transform>(f.player), h1 = reg.get<Transform>(f.head);
    restoreSnapshot(f.world, mid);
    f.steps(50);
    const Transform p2 = reg.get<Transform>(f.player);
    std::printf("\n      after 50 steps: (%.5f, %.5f, %.5f) / (%.5f, %.5f, %.5f)", p1.position.x, p1.position.y, p1.position.z,
                p2.position.x, p2.position.y, p2.position.z);
    CHECK(samePose(p1, p2) && samePose(h1, reg.get<Transform>(f.head)), "the jump lands in the same place, bit for bit");
}

namespace {

// A walk north, then east onto the Ramp, then a stop; the world's hash after it.
uint64_t walkAndHash(PlayerFixture& f) {
    f.step({axisSet("move_forward", 1.0f)});
    f.steps(19);
    f.step({axisSet("move_right", 1.0f)});
    f.steps(19);
    f.step({axisSet("move_forward", 0.0f), axisSet("move_right", 0.0f)});
    f.steps(9);
    return hashOf(f.world);
}

} // namespace

static void t14_restore_from_spawn_replays_a_walk() {
    // The agent's loop: snapshot the loaded scene (spawned, not stepped), walk, restore, walk again.
    PlayerFixture f(true, false);
    const Snapshot base = takeSnapshot(f.world, engineTypes());
    const uint64_t baseHash = hashOf(f.world);
    const uint64_t first = walkAndHash(f);
    const Transform p1 = f.world.registry().get<Transform>(f.player);
    restoreSnapshot(f.world, base);
    CHECK(hashOf(f.world) == baseHash, "back at the spawn's hash");
    const uint64_t second = walkAndHash(f);
    const Transform p2 = f.world.registry().get<Transform>(f.player);
    std::printf("\n      walked to (%.5f, %.5f, %.5f) / (%.5f, %.5f, %.5f)", p1.position.x, p1.position.y, p1.position.z,
                p2.position.x, p2.position.y, p2.position.z);
    CHECK(first == second && samePose(p1, p2), "the same walk from the restored spawn: the same hash and pose, bit for bit");
}

static void t14_restore_puts_a_body_back_however_close() {
    // A restore moves a body back even when it settled less than the 1e-4 the scene uses to know
    // its own write-back (CrateA sinks 4.2 micrometres into the floor): the Transform was
    // written by the restore, not by the step.
    PlayerFixture f(false, false);
    const Snapshot base = takeSnapshot(f.world, engineTypes());
    const entt::entity a = f.named("CrateA");
    const glm::vec3 authored = f.world.registry().get<Transform>(a).position;
    f.steps(90);   // settled and asleep, a few micrometres down
    auto& be = f.physics->backend();
    const glm::vec3 settled = be.getPosition(f.physics->bodyOf(a));
    restoreSnapshot(f.world, base);
    f.world.sync();
    const glm::vec3 back = be.getPosition(f.physics->bodyOf(a));
    std::printf("\n      CrateA y %.7f settled %.7f restored %.7f", authored.y, settled.y, back.y);
    CHECK(settled != authored && back == authored, "the body is back where the snapshot had it, exactly");
}

static void t14_resting_contacts_replay_exactly() {
    // Awake bodies resting on each other at the snapshot (the crates, one step after loading): the
    // solver warm-starts their contacts from the last step's impulses, which BodyMotion does not
    // hold; the snapshot's simulation (the backend's state) does, so they replay bit for bit.
    PlayerFixture f(true);
    const Snapshot base = takeSnapshot(f.world, engineTypes());
    (void)walkAndHash(f);
    auto& reg = f.world.registry();
    const Transform p1 = reg.get<Transform>(f.player), a1 = reg.get<Transform>(f.named("CrateA")),
                    b1 = reg.get<Transform>(f.named("CrateB"));
    restoreSnapshot(f.world, base);
    (void)walkAndHash(f);
    const float da = glm::length(reg.get<Transform>(f.named("CrateA")).position - a1.position);
    const float db = glm::length(reg.get<Transform>(f.named("CrateB")).position - b1.position);
    std::printf("\n      crates differ by %.2e m and %.2e m", da, db);
    CHECK(samePose(p1, reg.get<Transform>(f.player)), "the player: bit for bit");
    CHECK(samePose(a1, reg.get<Transform>(f.named("CrateA"))) && samePose(b1, reg.get<Transform>(f.named("CrateB"))),
          "the crates resting in contact: bit for bit");
}

static void t14_restore_after_a_rebuild_falls_back_to_body_motion() {
    // A body rebuilt since the snapshot (its Collider edited) is not the snapshot's body: the saved
    // simulation is not used, and the components' path (the teleport, then BodyMotion) still
    // replays a free fall exactly.
    Fixture f(kBlockout);
    liftCrates(f);
    (void)runSteps(f.world, 15);
    const Snapshot mid = takeSnapshot(f.world, engineTypes());
    (void)runSteps(f.world, 10);
    const Transform a1 = f.world.registry().get<Transform>(f.named("CrateA"));
    const entt::entity a = f.named("CrateA");
    const std::size_t builds = f.physics->bodyBuilds();
    f.world.registry().patch<Collider>(a, [](Collider& c) { c.surface = "stone"; });   // rebuilt at the next sync
    (void)runSteps(f.world, 5);
    CHECK(f.physics->bodyBuilds() > builds, "the edit rebuilt CrateA's body");
    restoreSnapshot(f.world, mid);
    (void)runSteps(f.world, 10);
    const Transform a2 = f.world.registry().get<Transform>(a);
    std::printf("\n      CrateA y %.6f / %.6f", a1.position.y, a2.position.y);
    CHECK(samePose(a1, a2), "the fall replays exactly through BodyMotion");
}

// ---- test 19: the look lead (sub-project 9 step 2) -----------------------------------------------
// docs/specs/2026-10-27-feel-design.md, "The look lead": the drawn camera leads the step by the look
// the steps have not consumed (World::setPendingLook), through the step's own arithmetic, so the step
// lands where the frame already was; the lead is presentation only, and absent in lockstep, for the
// dead and for characters not moved by the world's input.

namespace {

InputEvent mouseMove(float x, float y) {
    InputEvent e;
    e.type = InputEvent::Type::MouseDelta;
    e.x = x;
    e.y = y;
    return e;
}

// The upper 3x3 (rotation and scale) of two matrices, compared bit for bit.
bool sameBasis(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!(a[c][r] == b[c][r])) return false;
    return true;
}

// What a frame draws with `pending` look not yet stepped (alpha as given), then the step that
// consumes the same look: the drawn bases of the player and its head against the stepped ones.
struct LeadCheck {
    bool drawn{false};
    glm::mat4 playerDrawn{1.0f}, headDrawn{1.0f}, playerStepped{1.0f}, headStepped{1.0f};
};

LeadCheck leadThenStep(PlayerFixture& f, glm::vec2 pending, float alpha = 1.0f) {
    auto& reg = f.world.registry();
    LeadCheck out;
    f.world.setRenderAlpha(alpha);
    f.world.setPendingLook(pending);
    f.world.sync();
    const auto* pp = reg.try_get<RenderPose>(f.player);
    const auto* hp = reg.try_get<RenderPose>(f.head);
    out.drawn = pp && hp;
    if (pp) out.playerDrawn = pp->world;
    if (hp) out.headDrawn = hp->world;
    f.world.setRenderAlpha(1.0f);
    f.world.setPendingLook(glm::vec2(0.0f));
    f.step({mouseMove(pending.x, pending.y)});
    out.playerStepped = reg.get<WorldTransform>(f.player).world;
    out.headStepped = reg.get<WorldTransform>(f.head).world;
    return out;
}

} // namespace

static void sp9_t19_lead_matches_next_step() {
    PlayerFixture f(true);
    f.steps(5);
    const LeadCheck a = leadThenStep(f, {37.5f, -12.25f});
    CHECK(a.drawn, "a pending look gives the player and its head a drawn pose");
    CHECK(sameBasis(a.playerDrawn, a.playerStepped) && sameBasis(a.headDrawn, a.headStepped),
          "the drawn turn is the step's turn, bit for bit (body yaw and head pitch)");
    const LeadCheck b = leadThenStep(f, {0.0f, -2000.0f});   // 200 degrees up: the clamp
    const float pitch = f.world.registry().get<PlayerState>(f.player).pitchDeg;
    std::printf("\n      after the clamped look: pitch %.4f", pitch);
    CHECK(near(pitch, 89.0f, 1e-4f) && sameBasis(b.headDrawn, b.headStepped),
          "a lead past 89 degrees stops where the step stops it");
}

static void sp9_t19_lead_split_across_steps() {
    PlayerFixture f(true);
    f.steps(5);
    f.step({mouseMove(20.0f, 4.0f)});   // A, consumed
    const LeadCheck b = leadThenStep(f, {-55.0f, 9.5f});   // B, pending, then consumed
    CHECK(b.drawn && sameBasis(b.playerDrawn, b.playerStepped) && sameBasis(b.headDrawn, b.headStepped),
          "after a step consumed A, the lead is B alone: the drawn view equals the view after B's step");
}

static void sp9_t19_lead_never_in_world() {
    PlayerFixture f(true);
    f.steps(5);
    auto& reg = f.world.registry();
    const uint64_t hash = hashOf(f.world);
    const std::string saved = savedText(f.world);
    const Transform player = reg.get<Transform>(f.player), head = reg.get<Transform>(f.head);
    const glm::mat4 playerWorld = reg.get<WorldTransform>(f.player).world;
    f.world.setRenderAlpha(0.4f);
    f.world.setPendingLook({120.0f, -30.0f});
    f.world.sync();
    CHECK(reg.try_get<RenderPose>(f.head) != nullptr, "the lead is drawn");
    CHECK(hashOf(f.world) == hash && savedText(f.world) == saved, "world.hash and the saved file are the step's");
    CHECK(samePose(reg.get<Transform>(f.player), player) && samePose(reg.get<Transform>(f.head), head) &&
              reg.get<WorldTransform>(f.player).world == playerWorld,
          "Transform and WorldTransform keep the step's pose");
    const Snapshot snap = takeSnapshot(f.world, engineTypes());
    bool drawnInSnapshot = false;
    for (const SnapshotColumn& c : snap.columns) drawnInSnapshot |= c.type == engineTypes().find<RenderPose>();
    CHECK(!drawnInSnapshot, "and a snapshot holds no drawn pose");
    f.world.setRenderAlpha(1.0f);
    f.world.setPendingLook(glm::vec2(0.0f));
    f.world.sync();
    CHECK(reg.view<RenderPose>().empty(), "with nothing pending at alpha 1 nothing is drawn but the step's poses");
}

static void sp9_t19_no_lead_lockstep_dead_or_enemy() {
    {
        PlayerFixture f(true);
        f.steps(5);
        f.world.setRenderAlpha(1.0f);
        f.world.setPendingLook(glm::vec2(0.0f));
        f.world.sync();
        CHECK(f.world.registry().view<RenderPose>().empty(), "lockstep (alpha 1, nothing pending): no lead");
    }
    {
        PlayerFixture f(true);
        f.steps(5);
        f.world.registry().emplace_or_replace<Health>(f.player, Health{100.0f, 0.0f});
        f.steps(1);
        f.world.setPendingLook({80.0f, 0.0f});
        f.world.sync();
        CHECK(f.world.registry().try_get<RenderPose>(f.head) == nullptr, "a dead player: no lead (it does not turn)");
    }
    {
        PlayerFixture f(true);
        f.steps(5);
        f.world.registry().patch<PlayerController>(f.player, [](PlayerController& pc) { pc.input = false; });
        f.steps(1);
        f.world.setPendingLook({80.0f, 0.0f});
        f.world.sync();
        CHECK(f.world.registry().try_get<RenderPose>(f.head) == nullptr, "a character moved by its intent (an enemy): no lead");
    }
}

static void sp9_t19_rotation_drawn_from_newest_step() {
    PlayerFixture f(true);
    f.place({-1.0f, 0.0f, 2.5f}, 0.0f);
    f.step({axisSet("move_forward", 1.0f), axisSet("look_x", 50.0f)});   // walking and turning 5 degrees a step
    f.steps(3);
    auto& reg = f.world.registry();
    const glm::mat4 head = reg.get<WorldTransform>(f.head).world;
    f.world.setRenderAlpha(0.5f);
    f.world.setPendingLook(glm::vec2(0.0f));
    f.world.sync();
    const auto* hp = reg.try_get<RenderPose>(f.head);
    CHECK(hp && sameBasis(hp->world, head), "between steps the turn is drawn at the newest step's, not halfway");
    CHECK(hp && !nearVec(glm::vec3(hp->world[3]), glm::vec3(head[3]), 1e-4f),
          "while the position is still drawn between the steps");
}

static void sp9_t19_lead_across_placement() {
    PlayerFixture f(true);
    f.steps(5);
    f.world.setTransform(f.player, Transform{{-1.0f, 0.0f, 1.5f}, yawRotation(30.0f)});
    f.step();   // the placement: nothing to interpolate across
    auto& reg = f.world.registry();
    const glm::vec3 eye = glm::vec3(reg.get<WorldTransform>(f.head).world[3]);
    const LeadCheck c = leadThenStep(f, {25.0f, 0.0f}, 0.3f);
    CHECK(c.drawn && nearVec(glm::vec3(c.headDrawn[3]), eye, 1e-6f),
          "after a placement the eye is drawn where the step put it (not between the old place and the new)");
    CHECK(sameBasis(c.headDrawn, c.headStepped), "with the lead on top of the placed yaw");
}

// ---- --look DIR: pictures of the blockout's collision, from rays --------------------------------
// Every pixel is one ray against the physics scene (nothing is drawn from the meshes): a top-down
// view (rays straight down, 2 cm apart) and a view from inside the east wall looking west. A hit
// is tinted by its body and shaded by its normal; a miss is dark. crates.txt has the crates'
// centres before and after 120 steps.

static bool writePpm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    return std::fclose(f) == 0 && ok;
}

static glm::vec3 shade(const std::optional<RayHit>& h, glm::vec3 light) {
    if (!h) return glm::vec3(0.08f);
    const uint32_t k = static_cast<uint32_t>(entt::to_integral(h->entity)) * 2654435761u;
    const glm::vec3 tint(0.45f + 0.5f * float(k & 255) / 255.0f, 0.45f + 0.5f * float((k >> 8) & 255) / 255.0f,
                         0.45f + 0.5f * float((k >> 16) & 255) / 255.0f);
    return tint * (0.25f + 0.75f * std::max(0.0f, glm::dot(h->normal, light)));
}

static bool look(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    bool ok = true;
    Fixture f(kBlockout);
    const glm::vec3 light = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
    const auto put = [](std::vector<uint8_t>& img, std::size_t i, glm::vec3 c) {
        for (int k = 0; k < 3; ++k) img[3 * i + k] = static_cast<uint8_t>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    {   // top-down: x -4.2..4.2 across, z -3.2..3.2 down
        const int w = 420, h = 320;
        std::vector<uint8_t> img(std::size_t(w) * h * 3);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) {
                const auto hit = f.ray({-4.2f + 0.02f * (c + 0.5f), 10.0f, -3.2f + 0.02f * (r + 0.5f)}, {0, -1, 0}, 20.0f);
                glm::vec3 col = shade(hit, light);
                if (hit) col *= 0.6f + 0.4f * std::clamp(hit->point.y / 3.0f, 0.0f, 1.0f);   // higher is brighter
                put(img, std::size_t(r) * w + c, col);
            }
        ok &= writePpm(dir + "/collision_top.ppm", w, h, img);
    }
    {   // from x 3.9 looking toward -x: z -3..3 across, y 3..0 down
        const int w = 300, h = 150;
        std::vector<uint8_t> img(std::size_t(w) * h * 3);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) {
                const auto hit = f.ray({3.9f, 3.0f - 0.02f * (r + 0.5f), 3.0f - 0.02f * (c + 0.5f)}, {-1, 0, 0}, 8.0f);
                put(img, std::size_t(r) * w + c, shade(hit, glm::normalize(glm::vec3(1.0f, 0.6f, 0.2f))));
            }
        ok &= writePpm(dir + "/collision_side.ppm", w, h, img);
    }
    auto& backend = f.physics->backend();
    const auto a = f.physics->bodyOf(f.named("CrateA")), b = f.physics->bodyOf(f.named("CrateB"));
    const glm::vec3 a0 = backend.getPosition(a), b0 = backend.getPosition(b);
    for (int i = 0; i < 120; ++i) backend.step(1.0f / 60.0f);
    const glm::vec3 a1 = backend.getPosition(a), b1 = backend.getPosition(b);
    const PhysicsInfo info = f.physics->info();
    FILE* t = std::fopen((dir + "/crates.txt").c_str(), "w");
    if (!t) return false;
    std::fprintf(t, "blockout: %u static bodies, %u dynamic; shapes box %u, mesh %u; build %.3f ms\n", info.bodies.statics,
                 info.bodies.dynamics, info.shapes.box, info.shapes.mesh, info.buildMs);
    std::fprintf(t, "CrateA (%.4f, %.4f, %.4f) -> (%.4f, %.4f, %.4f) after 120 steps of 1/60 s\n", a0.x, a0.y, a0.z, a1.x, a1.y,
                 a1.z);
    std::fprintf(t, "CrateB (%.4f, %.4f, %.4f) -> (%.4f, %.4f, %.4f)\n", b0.x, b0.y, b0.z, b1.x, b1.y, b1.z);
    ok &= std::fclose(t) == 0;
    std::printf("look: %s %s/collision_top.ppm, collision_side.ppm, crates.txt\n", ok ? "wrote" : "FAILED to write", dir.c_str());
    return ok;
}

// ---- --drop DIR: the crates dropped from above, for before/after look shots ---------------------
// Lifts and tilts the two crates, steps 120 frames of 1/60 s, and writes crates.txt (poses before,
// at frame 40 and after, plus every contact that began) and drop_{before,mid,after}.jsonl: ohao run
// scripts that put the crates at those poses and render the room, deferred. Nothing in the live
// world steps physics yet (SP6), so the scripts set the crates' Transform from the bodies.

namespace {
struct ContactLog final : ohao::physics::backend::IContactListener {
    std::vector<ohao::physics::backend::ContactEvent> begins;
    int frame{0};
    std::vector<int> frames;
    void onContactBegin(const ohao::physics::backend::ContactEvent& e) override { begins.push_back(e); frames.push_back(frame); }
    void onContactPersist(const ohao::physics::backend::ContactEvent&) override {}
    void onContactEnd(const ohao::physics::backend::ContactEvent&) override {}
};
} // namespace

static bool drop(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Fixture f(kBlockout);
    auto& backend = f.physics->backend();
    using ohao::physics::backend::BodyHandle;
    std::vector<std::pair<BodyHandle, std::string>> names;
    for (auto [e, n, ln] : f.world.registry().view<const Name, const LevelNode>().each()) {
        const BodyHandle h = f.physics->bodyOf(e);
        if (h != ohao::physics::backend::INVALID_BODY) names.emplace_back(h, n.value);
    }
    const auto nameOf = [&](BodyHandle h) -> std::string {
        for (const auto& [b, n] : names) if (b == h) return n;
        return "?";
    };
    struct Crate { const char* name; BodyHandle body; std::string id; glm::vec3 pos[4]; glm::quat rot[4]; };
    Crate crates[2] = {{"CrateA", 0, {}, {}, {}}, {"CrateB", 0, {}, {}, {}}};
    for (auto& c : crates) {
        const entt::entity e = f.named(c.name);
        c.body = f.physics->bodyOf(e);
        char id[17];
        std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(f.world.idOf(e)));
        c.id = id;
        c.pos[0] = backend.getPosition(c.body);
        c.rot[0] = backend.getRotation(c.body);
    }
    // The drop: CrateA 1.3 m up, tilted; CrateB higher still, off-centre and tilted the other way.
    const glm::vec3 start[2] = {{1.3f, 1.6f, -1.7f}, {1.42f, 2.7f, -1.6f}};
    const glm::quat tilt[2] = {glm::quat(glm::radians(glm::vec3(18.0f, 30.0f, 0.0f))),
                               glm::quat(glm::radians(glm::vec3(0.0f, -20.0f, 35.0f)))};
    for (int i = 0; i < 2; ++i) {
        backend.setPosition(crates[i].body, start[i]);
        backend.setRotation(crates[i].body, tilt[i]);
        backend.setLinearVelocity(crates[i].body, glm::vec3(0.0f));
        backend.setAngularVelocity(crates[i].body, glm::vec3(0.0f));
        backend.setAwake(crates[i].body, true);
        crates[i].pos[1] = backend.getPosition(crates[i].body);
        crates[i].rot[1] = backend.getRotation(crates[i].body);
    }
    ContactLog log;
    backend.setContactListener(&log);
    for (int i = 1; i <= 120; ++i) {
        log.frame = i;
        backend.step(1.0f / 60.0f);
        if (i == 40)
            for (auto& c : crates) { c.pos[2] = backend.getPosition(c.body); c.rot[2] = backend.getRotation(c.body); }
    }
    backend.setContactListener(nullptr);
    for (auto& c : crates) { c.pos[3] = backend.getPosition(c.body); c.rot[3] = backend.getRotation(c.body); }

    bool ok = true;
    FILE* t = std::fopen((dir + "/crates.txt").c_str(), "w");
    if (!t) return false;
    std::fprintf(t, "blockout_room: CrateA (20 kg box), CrateB (8 kg box); Jolt, 120 steps of 1/60 s\n\n");
    std::fprintf(t, "authored (at rest already):\n");
    for (const auto& c : crates) std::fprintf(t, "  %s %s (%.4f, %.4f, %.4f)\n", c.name, c.id.c_str(), c.pos[0].x, c.pos[0].y, c.pos[0].z);
    const char* label[4] = {"authored", "before (dropped, frame 0)", "frame 40 (just after impact)", "after (frame 120)"};
    for (int k = 1; k < 4; ++k) {
        std::fprintf(t, "\n%s:\n", label[k]);
        for (const auto& c : crates) {
            const glm::vec3 up = c.rot[k] * glm::vec3(0, 1, 0);
            std::fprintf(t, "  %s pos (%.4f, %.4f, %.4f) quat xyzw (%.4f, %.4f, %.4f, %.4f) tilt %.1f deg awake %d\n", c.name,
                         c.pos[k].x, c.pos[k].y, c.pos[k].z, c.rot[k].x, c.rot[k].y, c.rot[k].z, c.rot[k].w,
                         glm::degrees(std::acos(std::clamp(std::fabs(up.y), 0.0f, 1.0f))), k == 3 ? int(backend.isAwake(c.body)) : 1);
        }
    }
    std::fprintf(t, "\ncontact begin events: %zu\n", log.begins.size());
    for (std::size_t i = 0; i < log.begins.size(); ++i) {
        const auto& e = log.begins[i];
        std::fprintf(t, "  frame %3d  %s <-> %s  at (%.3f, %.3f, %.3f)  rel speed %.2f m/s\n", log.frames[i], nameOf(e.body1).c_str(),
                     nameOf(e.body2).c_str(), e.contactPoint.x, e.contactPoint.y, e.contactPoint.z, glm::length(e.relativeVelocity));
    }
    ok &= std::fclose(t) == 0;
    const char* shot[3] = {"before", "mid", "after"};
    for (int k = 1; k < 4; ++k) {
        FILE* s = std::fopen((dir + "/drop_" + shot[k - 1] + ".jsonl").c_str(), "w");
        if (!s) return false;
        std::fprintf(s, "{\"method\":\"world.apply\",\"params\":{\"commands\":[");
        for (int i = 0; i < 2; ++i) {
            const auto& c = crates[i];
            std::fprintf(s, "%s{\"op\":\"field.set\",\"id\":\"%s\",\"path\":\"/Transform/position\",\"value\":[%.5f,%.5f,%.5f]},"
                            "{\"op\":\"field.set\",\"id\":\"%s\",\"path\":\"/Transform/rotation\",\"value\":[%.6f,%.6f,%.6f,%.6f]}",
                         i ? "," : "", c.id.c_str(), c.pos[k].x, c.pos[k].y, c.pos[k].z, c.id.c_str(), c.rot[k].x, c.rot[k].y,
                         c.rot[k].z, c.rot[k].w);
        }
        std::fprintf(s, "]}}\n");
        ok &= std::fclose(s) == 0;
    }
    std::printf("drop: %s %s/crates.txt, drop_{before,mid,after}.jsonl\n", ok ? "wrote" : "FAILED to write", dir.c_str());
    return ok;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--drop") return drop(argv[2]) ? 0 : 1;
    if (argc == 3 && std::string(argv[1]) == "--look") return look(argv[2]) ? 0 : 1;
    std::printf("level_physics_tests\n");
    RUN(t06_fit_box_sphere_capsule_from_mesh);
    RUN(t06_convex_hull_of_mesh);
    RUN(t06_static_mesh_shape);
    RUN(t06_ucx_hulls_are_one_compound_body);
    RUN(t06_dynamic_mesh_becomes_convex_with_warning);
    RUN(t06_non_uniform_scale_baked);
    RUN(t06_mirrored_node_collides_outward);
    RUN(t07_ray_down_at_each_spawn_hits_authored_height);
    RUN(t07_ray_through_doorway_passes);
    RUN(t07_ray_beside_doorway_hits_wall_south_a);
    RUN(t07_ray_on_ramp_returns_slope_normal);
    RUN(t07_crates_come_to_rest_when_stepped);
    RUN(t07_render_only_level_builds_no_bodies);
    RUN(t07_still_world_does_no_work);
    RUN(t10_crates_fall_and_rest);
    RUN(t10_contact_begin_events);
    RUN(t10_rest_writes_nothing);
    RUN(t10_edit_teleports_dynamic);
    RUN(t10_two_steps_one_frame_motion);
    RUN(t10_destroy_ends_contacts);
    RUN(t10_trigger_enter_exit);
    RUN(t10_substeps);
    RUN(t10_runs_are_identical);
    RUN(t10_event_log_cursor);
    RUN(t11_spawns_at_the_player_spawn_facing_its_yaw);
    RUN(t11_walks_four_metres_in_sixty_steps);
    RUN(t11_the_north_wall_stops_the_player);
    RUN(t11_climbs_the_step_and_the_platform);
    RUN(t11_climbs_the_ramp);
    RUN(t11_pushes_a_crate);
    RUN(t11_jumps_one_point_two_two_metres);
    RUN(t11_a_trigger_sees_the_player);
    RUN(t11_an_edited_controller_keeps_the_player_where_it_is);
    RUN(t11_kinematic_bodies_move_by_velocity);
    RUN(t12_head_at_eye_height);
    RUN(t12_pitch_clamps_at_89);
    RUN(t12_yaw_turns_the_body);
    RUN(t12_interpolation_leaves_hash_and_save);
    RUN(t12_play_scene_loads_with_head_camera);
    RUN(t13_walk_strides_0_75_m);
    RUN(t13_sprint_strides_0_95_m);
    RUN(t13_surface_from_ground_body);
    RUN(t13_land_after_a_drop);
    RUN(t13_none_while_still_or_airborne);
    RUN(t14_body_motion_follows_awake_bodies);
    RUN(t14_body_motion_is_neither_saved_nor_hashed);
    RUN(t14_restore_mid_fall_is_exact);
    RUN(t14_restore_puts_bodies_back_to_sleep);
    RUN(t14_restore_mid_jump_is_exact);
    RUN(t14_restore_from_spawn_replays_a_walk);
    RUN(t14_restore_puts_a_body_back_however_close);
    RUN(t14_resting_contacts_replay_exactly);
    RUN(t14_restore_after_a_rebuild_falls_back_to_body_motion);
    RUN(sp9_t19_lead_matches_next_step);
    RUN(sp9_t19_lead_split_across_steps);
    RUN(sp9_t19_lead_never_in_world);
    RUN(sp9_t19_no_lead_lockstep_dead_or_enemy);
    RUN(sp9_t19_rotation_drawn_from_newest_step);
    RUN(sp9_t19_lead_across_placement);
    return ohao::test::finish("level_physics_tests");
}
