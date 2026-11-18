#include "physics/private/physics_scene_impl.hpp"
#include "visual/components.hpp"

#include "core/jobs/graph.hpp"

#include <algorithm>

namespace ohao::ecs {

PhysicsScene::Impl::Impl(World& w, AssetResolver& a) : world(w), reg(w.registry()), assets(a) {
    connect<Collider>();
    connect<RigidBody>();
    connect<ColliderPart>();
    connect<CollisionMesh>();
    connect<MeshSource>();
    world.onDestroy<PhysicsBody, &Impl::onBodyGone>(this, queued());
    world.onConstruct<PlayerController, &Impl::touchPlayer>(this, queued());
    world.onUpdate<PlayerController, &Impl::touchPlayer>(this, queued());
    world.onDestroy<PlayerController, &Impl::onPlayerGone>(this, {.writesResources = {"physics.scene", "physics.touched"}});
    world.onConstruct<BodyMotion, &Impl::touchMotion>(this, queued());
    world.onUpdate<BodyMotion, &Impl::touchMotion>(this, queued());
    world.onUpdate<Transform, &Impl::touchPose>(this, queued());
    world.onConstruct<Joint, &Impl::touchJoint>(this, queued());
    world.onUpdate<Joint, &Impl::touchJoint>(this, queued());
    world.onDestroy<Joint, &Impl::touchJoint>(this, queued());
    world.onConstruct<JointState, &Impl::touchJointState>(this, queued());
    world.onUpdate<JointState, &Impl::touchJointState>(this, queued());
    world.onDestroy<JointState, &Impl::touchJointState>(this, queued());
    for (const entt::entity e : reg.view<Joint>()) jointsTouched.push_back(e);
    for (const entt::entity e : reg.view<Collider>()) touched.push_back(e);
    for (const entt::entity e : reg.view<ColliderPart>()) touched.push_back(e);
    for (const entt::entity e : reg.view<PlayerController>()) playersTouched.push_back(e);
}

PhysicsScene::Impl::~Impl() {
    disconnect<Collider>();
    disconnect<RigidBody>();
    disconnect<ColliderPart>();
    disconnect<CollisionMesh>();
    disconnect<MeshSource>();
    world.disconnect<PhysicsBody>(this);
    world.disconnect<PlayerController>(this);
    world.disconnect<BodyMotion>(this);
    world.disconnect<Transform>(this);
    world.disconnect<Joint>(this);
    world.disconnect<JointState>(this);
    if (backend) {
        for (auto& [e, p] : players) backend->destroyCharacter(p.character);
        for (auto& [e, k] : parked) backend->destroyCharacter(k.record.character);
    }
}

void PhysicsScene::Impl::touchJointState(entt::registry&, entt::entity e) {
    if (!writingJoint) jointsBroken.push_back(e);
}

void PhysicsScene::Impl::touchMotion(entt::registry&, entt::entity e) {
    if (!writingMotion) motionTouched.push_back(e);
}

void PhysicsScene::Impl::touchPose(entt::registry&, entt::entity e) {
    if (!writingPose) posedExternally.insert(e);
}

// This scene's own BodyMotion write (not a restore's), only when it changes.
void PhysicsScene::Impl::writeMotion(entt::entity e, const BodyMotion& m) {
    if (const auto* cur = reg.try_get<BodyMotion>(e);
        cur && cur->linear == m.linear && cur->angular == m.angular && cur->asleep == m.asleep)
        return;
    writingMotion = true;
    reg.emplace_or_replace<BodyMotion>(e, m);
    writingMotion = false;
}

// A restored body's motion: its velocities, awake, or asleep at rest (after the restore's
// teleport, which left it at rest).
void PhysicsScene::Impl::applyMotion(entt::entity e) {
    const auto it = bodies.find(e);
    if (it == bodies.end() || it->second.motion != Motion::Dynamic || !backend || !reg.valid(e)) return;
    const auto* m = reg.try_get<BodyMotion>(e);
    if (!m) return;
    BodyRecord& r = it->second;
    if (m->asleep) {
        backend->setLinearVelocity(r.handle, glm::vec3(0.0f));
        backend->setAngularVelocity(r.handle, glm::vec3(0.0f));
        backend->setAwake(r.handle, false);
    } else {
        backend->setAwake(r.handle, true);
        backend->setLinearVelocity(r.handle, m->linear);
        backend->setAngularVelocity(r.handle, m->angular);
    }
    r.awake = !m->asleep;
}

void PhysicsScene::Impl::removePlayer(entt::entity e, bool park) {
    const auto it = players.find(e);
    if (it == players.end()) return;
    if (it->second.inner != pb::INVALID_BODY) {
        if (!pairs.empty()) endPairsOf(it->second.inner);
        entityOf.erase(it->second.inner);
    }
    if (backend) {
        if (park && reg.all_of<PlayerController>(e)) {
            backend->setCharacterInWorld(it->second.character, false);
            dropParked(e);
            parked[e] = Parked{it->second, reg.get<PlayerController>(e)};
        } else {
            backend->destroyCharacter(it->second.character);
        }
    }
    players.erase(it);
}

void PhysicsScene::Impl::dropParked(entt::entity e) {
    const auto it = parked.find(e);
    if (it == parked.end()) return;
    if (backend) backend->destroyCharacter(it->second.record.character);
    parked.erase(it);
}

// A parked character for a controller like the one it was made for (a restore's), back in the
// world; else the parked one goes and false.
auto PhysicsScene::Impl::unpark(entt::entity e, const PlayerController& pc, PlayerRecord& out) -> bool {
    const auto it = parked.find(e);
    if (it == parked.end()) return false;
    const PlayerController& m = it->second.made;
    const bool same = m.radius == pc.radius && m.height == pc.height && m.maxSlopeDeg == pc.maxSlopeDeg &&
                      m.mass == pc.mass && m.pushForce == pc.pushForce;
    if (!same || !backend) {
        dropParked(e);
        return false;
    }
    out = it->second.record;
    parked.erase(it);
    backend->setCharacterInWorld(out.character, true);
    return true;
}

// A body entity destroyed (or its PhysicsBody removed by hand); not the scene's own removal.
void PhysicsScene::Impl::onBodyGone(entt::registry&, entt::entity e) {
    if (!removing) touched.push_back(e);
}

auto PhysicsScene::Impl::ensureBackend() -> pb::IPhysicsBackend& {
    if (!backend) {
        pb::BackendConfig config;
        config.maxBodies = 65536;
        backend = pb::createPhysicsBackend("jolt");
        if (!backend->initialize(config)) {
            OHAO_LOG(Error, physics, "physics: the Jolt backend did not start; levels will not collide");
            backend = std::make_unique<pb::NullPhysicsBackend>();
            (void)backend->initialize(config);
        }
    }
    return *backend;
}

void PhysicsScene::Impl::warn(entt::entity e, std::string code, std::string message) {
    if (!warned.emplace(e, code).second) return;
    const auto* name = reg.try_get<Name>(e);
    OHAO_LOG(Warn, physics, "physics: '{}': {}: {}", name ? name->value : std::string("?"), code, message);
    warnings.push_back({std::move(code), e, std::move(message)});
}

auto PhysicsScene::Impl::seqOf(entt::entity e) const -> uint64_t {
    const auto* co = reg.valid(e) ? reg.try_get<CreationOrder>(e) : nullptr;
    return co ? co->seq : UINT64_MAX;
}

auto PhysicsScene::Impl::ownCollider(entt::entity e) const -> bool {
    const auto* c = reg.try_get<Collider>(e);
    return c && c->shape != ColliderShape::None && !reg.all_of<ColliderPart>(e);
}

PhysicsScene::PhysicsScene(World& world, AssetResolver& assets) : m(std::make_unique<Impl>(world, assets)) {}

std::shared_ptr<const void> PhysicsScene::saveSimulation() const { return m->saveSimulation(); }
void PhysicsScene::restoreSimulation(const std::shared_ptr<const void>& state) { m->restoreSimulation(state); }
void PhysicsScene::hashSimulation(StateHash& h) const { m->hashSimulation(h); }
PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::sync(entt::registry&) { m->sync(); }

PhysicsScene& installPhysics(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<PhysicsInstalled>()) return *installed->scene;
    // The system owns the scene: it goes with the World's systems, before the registry.
    auto scene = std::make_shared<PhysicsScene>(world, assets);
    reg.ctx().emplace<PhysicsInstalled>(PhysicsInstalled{scene.get()});
    // Snapshots carry the simulation, so a restore replays exactly (spec "Snapshots and replay").
    PhysicsScene* raw = scene.get();
    raw->setEvents(world.events());
    addSnapshotHook(reg, SnapshotHook{"physics", [raw] { return raw->saveSimulation(); },
                                      [raw](const std::shared_ptr<const void>& s) { raw->restoreSimulation(s); },
                                      [raw](StateHash& h) { raw->hashSimulation(h); }});
    // A body follows its entity: the transform system tags what it moves for the sync below.
    tagMoves<PhysicsMoved, PhysicsBody, ColliderPart>(world);
    // Players spawn, from the entities a level expanded, before the transform system: a play scene's first frame
    // is the spawn's.
    // All four are exclusive: the scene reads and writes the registry itself.
    world.addSync("player_spawn", [scene](SystemContext&) { scene->spawnPlayers(); },
                  {.after = {"level"}, .before = {"transform"}, .reads = types<PlayerController, Transform>(),
                   .writes = types<PlayerState, Transform>(), .writesResources = {"physics.scene", "physics.touched"},
                   .exclusive = true});
    // Bodies follow their entities' world transforms.
    world.addSync("physics", [scene](SystemContext& c) { scene->sync(c.registry()); },
                  {.after = {"transform"},
                   .reads = types<Transform, WorldTransform, Collider, RigidBody, ColliderPart, CollisionMesh, MeshSource,
                                  Joint, PlayerController>(),
                   .writes = types<PhysicsBody, PhysicsMoved, BodyMotion, JointState>(),
                   .writesResources = {"physics.scene", "physics.touched"}, .exclusive = true});
    // Render interpolation between the last two steps, from the bodies as the sync left them.
    world.addSync("interpolate",
                  [scene](SystemContext& c) { scene->interpolate(c.world().renderAlpha(), c.world().pendingLook()); },
                  {.after = {"physics"}, .reads = types<PlayerController, WorldTransform>(),
                   .writes = types<RenderPose, RenderDirty>(), .readsResources = {"physics.scene"}, .exclusive = true});
    // Once per fixed step; its events order it before their readers.
    world.addSimulate("physics", [scene](SystemContext& c, float dt) { scene->step(c.world(), dt); },
                      {.reads = types<PlayerController, MoveIntent, RigidBody, Collider, Joint>(),
                       .writes = types<Transform, BodyMotion, PlayerState, JointState>(),
                       .writesResources = {"physics.scene", "physics.touched"}, .exclusive = true});
    return *scene;
}

PhysicsScene* physicsScene(World& world) {
    const auto* installed = world.context().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

const PhysicsScene* physicsScene(const World& world) {
    const auto* installed = world.context().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

} // namespace ohao::ecs
