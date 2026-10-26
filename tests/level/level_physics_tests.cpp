// Level collision (sub-project 5, step 4; docs/specs/2026-10-26-level-pipeline-design.md,
// "Collision generation", tests 6 and 7). CPU only: levels expand through the level system,
// their hints become Collider/RigidBody, and the physics system builds Jolt bodies that the
// tests ray-cast and (test 7's crates) step.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
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
    return ohao::test::finish("level_physics_tests");
}
