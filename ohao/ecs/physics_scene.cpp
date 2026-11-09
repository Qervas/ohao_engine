#include "ecs/physics_scene_impl.hpp"

#include <algorithm>

namespace ohao::ecs {


FittedCollider fitCollider(const MeshData& mesh, ColliderShape shape, glm::vec3 scale) {
    FittedCollider f;
    f.shape = shape;
    // The mesh's own AABB: spheres and capsules are fitted to it, then scaled by the largest axis.
    glm::vec3 lo(0.0f), hi(0.0f);
    if (!mesh.positions.empty()) {
        lo = hi = mesh.positions.front();
        for (const glm::vec3& p : mesh.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    const glm::vec3 centre = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    const float largest = maxAbs(scale);
    f.vertices.reserve(mesh.positions.size());
    for (const glm::vec3& p : mesh.positions) f.vertices.push_back(p * scale);
    switch (shape) {
        case ColliderShape::Box:
            f.halfExtents = half * glm::abs(scale);
            f.offset = centre * scale;
            break;
        case ColliderShape::Sphere: {
            float r2 = 0.0f;
            for (const glm::vec3& p : mesh.positions) r2 = std::max(r2, glm::dot(p - centre, p - centre));
            f.radius = std::sqrt(r2) * largest;
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Capsule: {
            const int axis = half.x >= half.y && half.x >= half.z ? 0 : half.y >= half.z ? 1 : 2;
            const float a = half[(axis + 1) % 3], b = half[(axis + 2) % 3];
            f.radius = std::max(a, b) * largest;
            f.halfHeight = std::max(half[axis] * largest - f.radius, 0.0f);
            // The backend's capsule runs along +Y.
            if (axis == 0) f.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            if (axis == 2) f.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Mesh:
            f.indices = mesh.indices;
            // A mirror turns the triangles inside out; swapping two corners turns them back.
            if (scale.x * scale.y * scale.z < 0.0f)
                for (std::size_t i = 0; i + 2 < f.indices.size(); i += 3) std::swap(f.indices[i + 1], f.indices[i + 2]);
            break;
        case ColliderShape::Convex:
        case ColliderShape::None:
            break;
    }
    if (shape != ColliderShape::Convex && shape != ColliderShape::Mesh) f.vertices.clear();
    return f;
}

PhysicsScene::Impl::Impl(entt::registry& r, AssetResolver& a) : reg(r), assets(a) {
    connect<Collider>();
    connect<RigidBody>();
    connect<ColliderPart>();
    connect<CollisionMesh>();
    connect<MeshSource>();
    reg.on_destroy<PhysicsBody>().connect<&Impl::onBodyGone>(this);
    reg.on_construct<PlayerController>().connect<&Impl::touchPlayer>(this);
    reg.on_update<PlayerController>().connect<&Impl::touchPlayer>(this);
    reg.on_destroy<PlayerController>().connect<&Impl::onPlayerGone>(this);
    reg.on_construct<BodyMotion>().connect<&Impl::touchMotion>(this);
    reg.on_update<BodyMotion>().connect<&Impl::touchMotion>(this);
    reg.on_update<Transform>().connect<&Impl::touchPose>(this);
    reg.on_construct<Joint>().connect<&Impl::touchJoint>(this);
    reg.on_update<Joint>().connect<&Impl::touchJoint>(this);
    reg.on_destroy<Joint>().connect<&Impl::touchJoint>(this);
    reg.on_construct<JointState>().connect<&Impl::touchJointState>(this);
    reg.on_update<JointState>().connect<&Impl::touchJointState>(this);
    reg.on_destroy<JointState>().connect<&Impl::touchJointState>(this);
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
    reg.on_destroy<PhysicsBody>().disconnect(this);
    reg.on_construct<PlayerController>().disconnect(this);
    reg.on_update<PlayerController>().disconnect(this);
    reg.on_destroy<PlayerController>().disconnect(this);
    reg.on_construct<BodyMotion>().disconnect(this);
    reg.on_update<BodyMotion>().disconnect(this);
    reg.on_update<Transform>().disconnect(this);
    reg.on_construct<Joint>().disconnect(this);
    reg.on_update<Joint>().disconnect(this);
    reg.on_destroy<Joint>().disconnect(this);
    reg.on_construct<JointState>().disconnect(this);
    reg.on_update<JointState>().disconnect(this);
    reg.on_destroy<JointState>().disconnect(this);
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
        // Single-threaded: a level's bodies build and step the same way on every machine,
        // and no worker threads start for a world that only ray-casts.
        pb::BackendConfig config;
        config.enableMultithreading = false;
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

// The mesh a collider is fitted to: a CollisionMesh, else the entity's MeshSource.
auto PhysicsScene::Impl::meshOf(entt::entity e) -> std::shared_ptr<const Model> {
    const MeshSource* source = nullptr;
    MeshSource fromCollision;
    if (const auto* cm = reg.try_get<CollisionMesh>(e)) {
        fromCollision.source = cm->source;
        source = &fromCollision;
    } else {
        source = reg.try_get<MeshSource>(e);
    }
    if (!source) return nullptr;
    if (const auto* asset = std::get_if<ModelAsset>(&source->source)) {
        auto loaded = assets.loadModel(asset->asset, asset->mesh);
        if (!loaded) {
            warn(e, "W_COLLIDER_NO_MESH", "its mesh did not load (" + loaded.error().message + "); no collider");
            return nullptr;
        }
        return std::move(loaded.value());
    }
    return primitiveModel(*source);
}

// One collider as a backend shape in the body's frame (scale baked), or nullopt.
auto PhysicsScene::Impl::shapeOf(entt::entity e, const Collider& c, glm::vec3 scale, Motion motion, std::vector<std::shared_ptr<FittedCollider>>& keep, BodyRecord::Kind& kind) -> std::optional<pb::ShapeInfo> {
    ColliderShape shape = c.shape;
    if (shape == ColliderShape::Mesh && motion == Motion::Dynamic) {
        warn(e, "W_LEVEL_DYNAMIC_MESH", "a dynamic body cannot have a triangle-mesh collider; it is built as the mesh's convex hull");
        shape = ColliderShape::Convex;
    }
    auto fitted = std::make_shared<FittedCollider>();
    const bool explicitSize = c.size != glm::vec3(0.0f) &&
                              (shape == ColliderShape::Box || shape == ColliderShape::Sphere || shape == ColliderShape::Capsule);
    if (explicitSize) {
        fitted->shape = shape;
        fitted->offset = c.offset * scale;
        fitted->halfExtents = c.size * glm::abs(scale);
        fitted->radius = c.size.x * maxAbs(scale);
        fitted->halfHeight = c.size.y * maxAbs(scale);
        fitted->roundedScale = shape != ColliderShape::Box && nonUniform(scale);
    } else {
        const auto model = meshOf(e);
        if (!model || model->vertices.empty()) {
            warn(e, "W_COLLIDER_NO_MESH", "its collider has no size and the entity no mesh to fit it to; no collider");
            return std::nullopt;
        }
        MeshData data;
        data.positions.reserve(model->vertices.size());
        for (const auto& v : model->vertices) data.positions.push_back(v.position);
        if (shape == ColliderShape::Mesh) data.indices = model->indices;
        *fitted = fitCollider(data, shape, scale);
    }
    if (fitted->roundedScale)
        warn(e, "W_COLLIDER_SCALE", "a sphere or capsule cannot stretch unevenly; it takes the largest scale axis");

    pb::ShapeInfo s;
    bool placed = false;   // the shape sits away from the body's origin (or turned)
    switch (shape) {
        case ColliderShape::Box:
            s.type = pb::ShapeInfo::Type::BOX;
            s.halfExtents = glm::max(fitted->halfExtents, glm::vec3(0.001f));
            kind = BodyRecord::Kind::Box;
            placed = fitted->offset != glm::vec3(0.0f);
            break;
        case ColliderShape::Sphere:
            s.type = pb::ShapeInfo::Type::SPHERE;
            s.radius = std::max(fitted->radius, 0.001f);
            kind = BodyRecord::Kind::Sphere;
            placed = fitted->offset != glm::vec3(0.0f);
            break;
        case ColliderShape::Capsule:
            s.type = pb::ShapeInfo::Type::CAPSULE;
            s.radius = std::max(fitted->radius, 0.001f);
            s.height = 2.0f * (fitted->halfHeight + s.radius);   // the backend's height is end to end
            kind = BodyRecord::Kind::Capsule;
            placed = fitted->offset != glm::vec3(0.0f) || fitted->rotation != glm::quat(1, 0, 0, 0);
            break;
        case ColliderShape::Convex:
            s.type = pb::ShapeInfo::Type::CONVEX_HULL;
            s.meshVertices = fitted->vertices.data();
            s.meshVertexCount = static_cast<uint32_t>(fitted->vertices.size());
            kind = BodyRecord::Kind::Convex;
            break;
        case ColliderShape::Mesh:
            s.setMesh(fitted->vertices, fitted->indices);
            kind = BodyRecord::Kind::Mesh;
            break;
        case ColliderShape::None:
            return std::nullopt;
    }
    keep.push_back(fitted);
    if (!placed) return s;
    pb::ShapeInfo wrap;
    wrap.type = pb::ShapeInfo::Type::COMPOUND;
    wrap.children.push_back({std::move(s), fitted->offset, fitted->rotation});
    return wrap;
}

// A removed body's pairs end now (Jolt's later OnContactRemoved for it no longer maps).
void PhysicsScene::Impl::endPairsOf(pb::BodyHandle h) {
    for (auto it = pairs.begin(); it != pairs.end();) {
        if (pb::BodyHandle(it->first >> 32) != h && pb::BodyHandle(it->first & 0xffffffffu) != h) {
            ++it;
            continue;
        }
        if (it->second.dormant) --dormant;
        pending.push_back(endEvent(it->second));
        it = pairs.erase(it);
    }
}

auto PhysicsScene::Impl::endEvent(const Pair& p) const -> PhysicsEvent {
    PhysicsEvent ev;
    ev.kind = p.trigger ? PhysicsEvent::Kind::TriggerExit : PhysicsEvent::Kind::ContactEnd;
    ev.a = p.a != entt::null && reg.valid(p.a) ? p.a : entt::entity{entt::null};
    ev.b = p.b != entt::null && reg.valid(p.b) ? p.b : entt::entity{entt::null};
    ev.aId = p.aId;
    ev.bId = p.bId;
    ev.point = p.point;
    ev.normal = p.normal;
    return ev;
}

void PhysicsScene::Impl::destroyBody(entt::entity e) {
    const auto it = bodies.find(e);
    if (it == bodies.end()) return;
    destroyJointsOf(e);
    const pb::BodyHandle h = it->second.handle;
    bodies.erase(it);
    if (h != pb::INVALID_BODY && !pairs.empty()) endPairsOf(h);
    if (backend && h != pb::INVALID_BODY) {
        backend->destroyBody(h);
        entityOf.erase(h);
    }
    if (reg.valid(e) && reg.all_of<PhysicsBody>(e)) {
        removing = true;
        reg.remove<PhysicsBody>(e);
        removing = false;
    }
}

void PhysicsScene::Impl::build(entt::entity e, const std::vector<entt::entity>& parts) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto* wt = reg.try_get<WorldTransform>(e);
    const Placement body = decompose(wt ? wt->world : glm::mat4(1.0f));
    const RigidBody rb = reg.all_of<RigidBody>(e) ? reg.get<RigidBody>(e) : RigidBody{};

    std::vector<std::shared_ptr<FittedCollider>> keep;
    std::vector<pb::CompoundChild> children;
    BodyRecord record;
    record.motion = rb.motion;
    record.scale = body.scale;
    record.hasParts = !parts.empty();
    record.trigger = ownCollider(e) && reg.get<Collider>(e).trigger;
    record.wpos = body.position;
    record.wrot = body.rotation;
    if (ownCollider(e))
        if (auto s = shapeOf(e, reg.get<Collider>(e), body.scale, rb.motion, keep, record.kind))
            children.push_back({std::move(*s), glm::vec3(0.0f), glm::quat(1, 0, 0, 0)});
    const glm::quat inverse = glm::inverse(body.rotation);
    for (const entt::entity p : parts) {
        const auto* c = reg.try_get<Collider>(p);
        const auto* pw = reg.try_get<WorldTransform>(p);
        if (!c || c->shape == ColliderShape::None || !pw) continue;
        const Placement part = decompose(pw->world);
        BodyRecord::Kind ignored{};
        if (auto s = shapeOf(p, *c, part.scale, rb.motion, keep, ignored))
            children.push_back({std::move(*s), inverse * (part.position - body.position), inverse * part.rotation});
        partOf[p] = e;
    }
    if (children.empty()) return;

    pb::BodyCreationInfo info;
    info.position = body.position;
    info.rotation = body.rotation;
    info.motionType = toBackend(rb.motion);
    info.mass = rb.mass;
    info.friction = rb.friction;
    info.restitution = rb.restitution;
    info.isSensor = record.trigger;
    if (parts.empty() && children.size() == 1 && children[0].position == glm::vec3(0.0f) &&
        children[0].rotation == glm::quat(1, 0, 0, 0)) {
        info.shape = std::move(children[0].shape);
    } else {
        info.shape.type = pb::ShapeInfo::Type::COMPOUND;
        info.shape.children = std::move(children);
    }
    if (!parts.empty()) record.kind = BodyRecord::Kind::Compound;

    pb::IPhysicsBackend& be = ensureBackend();
    record.handle = be.createBody(info);
    ++builds;
    if (record.handle != pb::INVALID_BODY) {
        entityOf[record.handle] = e;
        bodies[e] = record;
        reg.emplace_or_replace<PhysicsBody>(e, PhysicsBody{record.handle});
    }
    buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void PhysicsScene::Impl::syncBodies() {
    // 1. Bodies to rebuild: the touched entities' own, and the bodies their parts were or
    // are in; moved parts and moved bodies whose shapes depend on where things are.
    std::vector<entt::entity> rebuild;
    const auto addBodyOf = [&](entt::entity e) {
        rebuild.push_back(e);
        if (const auto it = partOf.find(e); it != partOf.end()) rebuild.push_back(it->second);
        if (reg.valid(e))
            if (const auto* p = reg.try_get<ColliderPart>(e)) rebuild.push_back(p->body);
    };
    for (const entt::entity e : touched) addBodyOf(e);
    touched.clear();
    std::vector<entt::entity> moved;
    for (const entt::entity e : reg.view<PhysicsMoved>()) {
        if (reg.all_of<ColliderPart>(e)) {
            addBodyOf(e);
            continue;
        }
        const auto it = bodies.find(e);
        if (it == bodies.end()) continue;
        const auto& wt = reg.get<WorldTransform>(e);
        if (it->second.hasParts || differs(decompose(wt.world).scale, it->second.scale, 1e-6f)) rebuild.push_back(e);
        else moved.push_back(e);
    }
    reg.clear<PhysicsMoved>();
    if (rebuild.empty() && moved.empty() && !savedPending && jointsTouched.empty() && jointsBroken.empty()) return;

    // 2. Rebuild, in creation order (bodies build in the same order on every run).
    std::sort(rebuild.begin(), rebuild.end(), [&](entt::entity a, entt::entity b) {
        const uint64_t sa = seqOf(a), sb = seqOf(b);
        return sa != sb ? sa < sb : entt::to_integral(a) < entt::to_integral(b);
    });
    rebuild.erase(std::unique(rebuild.begin(), rebuild.end()), rebuild.end());
    ankerl::unordered_dense::map<entt::entity, std::vector<entt::entity>> partsOf;
    if (!rebuild.empty()) {
        std::vector<std::pair<uint64_t, entt::entity>> ordered;
        for (auto [p, part] : reg.view<const ColliderPart>().each())
            if (reg.valid(part.body)) ordered.emplace_back(seqOf(p), p);
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [seq, p] : ordered) partsOf[reg.get<ColliderPart>(p).body].push_back(p);
    }
    static const std::vector<entt::entity> kNone;
    for (const entt::entity e : rebuild) {
        if (e == entt::null) continue;
        destroyBody(e);
        for (auto it = partOf.begin(); it != partOf.end();)
            it = it->second == e ? partOf.erase(it) : std::next(it);
        if (!reg.valid(e) || reg.all_of<ColliderPart>(e)) continue;
        const auto parts = partsOf.find(e);
        const auto& list = parts == partsOf.end() ? kNone : parts->second;
        if (ownCollider(e) || !list.empty()) build(e, list);
    }

    syncJoints();

    // A snapshot's simulation, restored: every body as it was, so a body whose entity the
    // restore put back where its body now is stays (no teleport, which would drop the contact
    // cache); one the snapshot had already moved without a step goes there below.
    const bool restored = savedPending && restoreSaved();

    // 3. Moved bodies follow their entities. A dynamic body's entity moved by the write-back
    // (its world pose is the one written) stays; moved by anything else, the body jumps
    // there, at rest.
    for (const entt::entity e : moved) {
        const auto it = bodies.find(e);
        if (it == bodies.end() || !backend) continue;
        BodyRecord& r = it->second;
        const Placement p = decompose(reg.get<WorldTransform>(e).world);
        if (restored && !differs(p.position, r.wpos, 1e-4f) && sameRotation(p.rotation, r.wrot, 1e-4f)) {
            posedExternally.erase(e);
            continue;
        }
        if (r.motion == Motion::Dynamic) {
            if (!posedExternally.erase(e) && !differs(p.position, r.wpos, 1e-4f) && sameRotation(p.rotation, r.wrot, 1e-4f))
                continue;
            backend->setPose(r.handle, p.position, p.rotation);
            backend->setLinearVelocity(r.handle, glm::vec3(0.0f));
            backend->setAngularVelocity(r.handle, glm::vec3(0.0f));
            backend->setAwake(r.handle, true);
            r.wpos = p.position;
            r.wrot = p.rotation;
            r.awake = true;
            continue;
        }
        if (r.motion == Motion::Kinematic && stepDt > 0.0f) {
            // Inside a step: by velocity, so it pushes what it meets; stopped after the step.
            backend->moveKinematic(r.handle, p.position, p.rotation, stepDt);
            kinematicMoved.push_back(r.handle);
            continue;
        }
        backend->setPose(r.handle, p.position, p.rotation);
    }
}

// Every body (players' inner bodies too) and its entity's id, by handle.
auto PhysicsScene::Impl::bodyIdentity() const -> std::vector<std::pair<pb::BodyHandle, uint64_t>> {
    std::vector<std::pair<pb::BodyHandle, uint64_t>> out;
    out.reserve(entityOf.size());
    for (const auto& [h, e] : entityOf) out.emplace_back(h, idOf(e));
    std::sort(out.begin(), out.end());
    return out;
}

// Bodies up to date, then the motion a restore wrote: into the bodies (after their
// teleports); players take theirs in their step, after its own teleport.
void PhysicsScene::Impl::sync() {
    const std::size_t built = builds;
    syncBodies();
    if (builds != built && backend)
        for (auto& [e, p] : players) {
            if (!p.placed || !reg.valid(e)) continue;
            backend->setCharacterPosition(p.character, p.written);   // its contacts found again
            standWhereItIs(p);
            if (auto* st = reg.try_get<PlayerState>(e)) st->onGround = p.grounded;
        }
    if (motionTouched.empty()) return;
    std::vector<entt::entity> list;
    list.swap(motionTouched);
    for (const entt::entity e : list) {
        if (players.contains(e)) playerMotion.insert(e);
        else applyMotion(e);
    }
}


struct PhysicsScene::Impl::SavedSimulation {
    std::string backend;                                        // bodies, contact cache, characters
    std::vector<std::pair<pb::BodyHandle, uint64_t>> bodies;    // which body is whose (bodyIdentity)
    std::vector<std::pair<pb::ConstraintHandle, uint64_t>> joints;   // which constraint is whose (jointIdentity)
    std::vector<SavedPlayer> players;
    std::vector<std::pair<uint64_t, Pair>> pairs;               // the contact bookkeeping, entities by id
    std::size_t dormant{0};
    uint64_t steps{0};                                          // the step count (the log's seq is the log's: step 3c)
};

std::shared_ptr<const void> PhysicsScene::Impl::saveSimulation() const {
    if (!backend) return nullptr;
    auto s = std::make_shared<SavedSimulation>();
    s->backend = backend->saveState();
    s->bodies = bodyIdentity();
    s->joints = jointIdentity();
    for (const auto& [e, p] : players) s->players.push_back(SavedPlayer{idOf(e), idOf(p.head), idOf(p.ground), p});
    s->pairs.assign(pairs.begin(), pairs.end());   // in the map's order: a restore inserts them back in it
    s->dormant = dormant;
    s->steps = steps;
    return s;
}

void PhysicsScene::Impl::restoreSimulation(const std::shared_ptr<const void>& state) {
    savedPending = std::static_pointer_cast<const SavedSimulation>(state);
    restoredSinceSpawn = true;
    if (savedPending) steps = savedPending->steps;   // at once: the bodies wait for the next sync
}

void PhysicsScene::Impl::hashSimulation(StateHash& h) const {
    std::shared_ptr<const void> live;
    const SavedSimulation* s = savedPending.get();
    if (!s) {
        live = saveSimulation();
        s = static_cast<const SavedSimulation*>(live.get());
    }
    if (!s) {
        h.u64(0);
        return;
    }
    h.string(s->backend);
    h.u64(s->bodies.size());
    for (const auto& [body, id] : s->bodies) {
        h.u64(body);
        h.u64(id);
    }
    h.u64(s->joints.size());
    for (const auto& [joint, id] : s->joints) {
        h.u64(joint);
        h.u64(id);
    }
    h.u64(s->players.size());
    for (const SavedPlayer& sp : s->players) {
        const PlayerRecord& p = sp.record;
        h.u64(sp.id);
        h.u64(sp.headId);
        h.u64(sp.groundId);
        h.u64(p.character);
        h.u64(p.inner);
        h.vec3(p.written);
        h.f32(p.writtenRot.w); h.f32(p.writtenRot.x); h.f32(p.writtenRot.y); h.f32(p.writtenRot.z);
        h.vec3(p.horizontal);
        h.f32(p.vy);
        h.f32(p.yawDeg);
        h.f32(p.pitchDeg);
        h.vec3(p.headPos);
        h.f32(p.headRot.w); h.f32(p.headRot.x); h.f32(p.headRot.y); h.f32(p.headRot.z);
        h.f32(p.stride);
        h.f32(p.airTop);
        h.boolean(p.grounded);
        h.u64(p.footsteps);
        h.boolean(p.placed);
    }
    std::vector<std::pair<uint64_t, Pair>> pairsByKey = s->pairs;
    std::sort(pairsByKey.begin(), pairsByKey.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    h.u64(pairsByKey.size());
    for (const auto& [key, pair] : pairsByKey) {
        h.u64(key);
        h.u32(pair.count);
        h.boolean(pair.trigger);
        h.boolean(pair.dormant);
        h.u64(pair.aId);
        h.u64(pair.bId);
        h.vec3(pair.point);
        h.vec3(pair.normal);
        h.u64(pair.since);
    }
    h.u64(s->dormant);
    h.u64(s->steps);
}

// The saved simulation back in place, when its bodies and players are this world's (the same
// handles for the same entities); else nothing changes and false.
bool PhysicsScene::Impl::restoreSaved() {
    const std::shared_ptr<const SavedSimulation> s = std::move(savedPending);
    savedPending.reset();
    if (!backend || !s || s->backend.empty() || bodyIdentity() != s->bodies || jointIdentity() != s->joints ||
        s->players.size() != players.size())
        return false;
    ankerl::unordered_dense::map<uint64_t, entt::entity> byId;
    for (auto [e, id] : reg.view<const EntityId>().each()) byId.emplace(id.value, e);
    const auto entityOfId = [&](uint64_t id) {
        const auto it = byId.find(id);
        return id != 0 && it != byId.end() ? it->second : entt::entity{entt::null};
    };
    for (const SavedPlayer& sp : s->players) {
        const auto it = players.find(entityOfId(sp.id));
        if (it == players.end() || it->second.character != sp.record.character) return false;
    }
    if (!backend->restoreState(s->backend)) {
        OHAO_LOG(Warn, physics, "physics: a snapshot's simulation did not restore; bodies restart from their poses");
        return false;
    }
    for (auto& [e, r] : bodies) {
        r.wpos = backend->getPosition(r.handle);
        r.wrot = backend->getRotation(r.handle);
        r.awake = r.motion == Motion::Dynamic && backend->isAwake(r.handle);
    }
    for (const SavedPlayer& sp : s->players) {
        const entt::entity e = entityOfId(sp.id);
        PlayerRecord& p = players[e];
        p = sp.record;
        p.head = entityOfId(sp.headId);
        p.ground = entityOfId(sp.groundId);
        posedExternally.erase(e);
        if (p.head != entt::null) posedExternally.erase(p.head);
    }
    pairs.clear();
    for (const auto& [key, saved] : s->pairs) {
        Pair p = saved;
        p.a = entityOfId(p.aId);
        p.b = entityOfId(p.bId);
        pairs.emplace(key, p);
    }
    dormant = s->dormant;
    movers.clear();          // placed, not moved: nothing to draw between
    motionTouched.clear();   // the simulation carries every body's motion
    playerMotion.clear();
    return true;
}

PhysicsScene::PhysicsScene(entt::registry& reg, AssetResolver& assets) : m(std::make_unique<Impl>(reg, assets)) {}

std::shared_ptr<const void> PhysicsScene::saveSimulation() const { return m->saveSimulation(); }
void PhysicsScene::restoreSimulation(const std::shared_ptr<const void>& state) { m->restoreSimulation(state); }
void PhysicsScene::hashSimulation(StateHash& h) const { m->hashSimulation(h); }
PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::sync(entt::registry&) { m->sync(); }

std::optional<RayHit> PhysicsScene::raycast(glm::vec3 from, glm::vec3 dir, float max) const {
    if (!m->backend || glm::dot(dir, dir) == 0.0f || !(max > 0.0f)) return std::nullopt;
    const glm::vec3 d = glm::normalize(dir);
    pb::RaycastHit hit;
    if (!m->backend->castRay(from, d, max, hit)) return std::nullopt;
    const auto it = m->entityOf.find(hit.bodyHandle);
    RayHit out;
    out.entity = it == m->entityOf.end() ? entt::entity{entt::null} : it->second;
    out.point = hit.position;
    out.normal = hit.normal;
    out.dist = hit.fraction * max;
    return out;
}

std::optional<RayHit> PhysicsScene::raycastFrom(glm::vec3 from, glm::vec3 dir, float max, entt::entity ignore) const {
    if (!m->backend || glm::dot(dir, dir) == 0.0f || !(max > 0.0f)) return std::nullopt;
    const glm::vec3 d = glm::normalize(dir);
    for (const pb::RaycastHit& hit : m->backend->castRayAll(from, d, max)) {
        const auto it = m->entityOf.find(hit.bodyHandle);
        const entt::entity e = it == m->entityOf.end() ? entt::entity{entt::null} : it->second;
        if (e != entt::null) {
            if (e == ignore) continue;
            if (const auto r = m->bodies.find(e); r != m->bodies.end() && r->second.trigger) continue;
        }
        RayHit out;
        out.entity = e;
        out.point = hit.position;
        out.normal = hit.normal;
        out.dist = hit.fraction * max;
        return out;
    }
    return std::nullopt;
}

bool PhysicsScene::turnPlayer(entt::entity e, float yawDeg, float pitchDeg) { return m->turnPlayer(e, yawDeg, pitchDeg); }

PhysicsInfo PhysicsScene::info() const {
    PhysicsInfo out;
    using Kind = Impl::BodyRecord::Kind;
    for (const auto& [e, r] : m->bodies) {
        switch (r.motion) {
            case Motion::Static: ++out.bodies.statics; break;
            case Motion::Dynamic:
                ++out.bodies.dynamics;
                if (m->backend && m->backend->isAwake(r.handle)) ++out.awake;
                break;
            case Motion::Kinematic: ++out.bodies.kinematics; break;
        }
        if (r.trigger) ++out.triggers;
        switch (r.kind) {
            case Kind::Box: ++out.shapes.box; break;
            case Kind::Sphere: ++out.shapes.sphere; break;
            case Kind::Capsule: ++out.shapes.capsule; break;
            case Kind::Convex: ++out.shapes.convex; break;
            case Kind::Mesh: ++out.shapes.mesh; break;
            case Kind::Compound: ++out.shapes.compound; break;
        }
    }
    out.buildMs = m->buildMs;
    out.steps = m->steps;
    out.contacts = static_cast<uint32_t>(m->pairs.size());
    out.players = static_cast<uint32_t>(m->players.size());
    out.joints = static_cast<uint32_t>(m->joints.size());
    for (auto [e, st] : m->reg.view<const JointState>().each())
        if (st.broken && m->joints.contains(e)) ++out.brokenJoints;
    return out;
}

std::vector<PhysicsContact> PhysicsScene::contacts() const {
    std::vector<PhysicsContact> out;
    out.reserve(m->pairs.size());
    for (const auto& [key, p] : m->pairs) out.push_back(PhysicsContact{p.a, p.b, p.aId, p.bId, p.point, p.normal, p.trigger, p.since});
    std::sort(out.begin(), out.end(), [](const PhysicsContact& x, const PhysicsContact& y) {
        return x.aId != y.aId ? x.aId < y.aId : x.bId < y.bId;
    });
    return out;
}

std::vector<entt::entity> PhysicsScene::players() const {
    std::vector<entt::entity> out;
    for (const auto& [e, p] : m->players)
        if (m->reg.valid(e)) out.push_back(e);
    std::sort(out.begin(), out.end(), [&](entt::entity a, entt::entity b) { return m->seqOf(a) < m->seqOf(b); });
    return out;
}

std::optional<PlayerInfo> PhysicsScene::playerInfo(entt::entity e) const {
    const auto it = m->players.find(e);
    if (it == m->players.end()) return std::nullopt;
    const Impl::PlayerRecord& p = it->second;
    PlayerInfo out;
    out.pos = p.written;
    out.yawDeg = p.yawDeg;
    out.pitchDeg = p.pitchDeg;
    out.velocity = glm::vec3(p.horizontal.x, p.vy, p.horizontal.z);
    out.onGround = p.grounded;
    out.footsteps = p.footsteps;
    if (p.grounded && p.ground != entt::null && m->reg.valid(p.ground)) {
        out.ground = p.ground;
        const auto* c = m->reg.try_get<Collider>(p.ground);
        out.surface = c && !c->surface.empty() ? c->surface : std::string("default");
    } else if (p.grounded) {
        out.surface = "default";
    }
    return out;
}

bool PhysicsScene::teleportPlayer(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg) {
    return m->teleport(e, pos, yawDeg);
}

void PhysicsScene::step(World& world, float dt) { m->step(world.input(), dt); }
void PhysicsScene::spawnPlayers() { m->spawnPlayers(); }
void PhysicsScene::interpolate(float alpha, glm::vec2 pendingLook) { m->interpolate(alpha, pendingLook); }
bool PhysicsScene::leadsLook() const noexcept { return m->leadsLook; }
std::span<const PhysicsEvent> PhysicsScene::stepEvents() const noexcept { return m->stepEvents; }
std::vector<PhysicsEvent> PhysicsScene::eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const {
    return m->since(since, max, dropped);
}
uint64_t PhysicsScene::nextEventSeq() const noexcept { return m->nextSeq; }
uint64_t PhysicsScene::steps() const noexcept { return m->steps; }
int PhysicsScene::lastSubsteps() const noexcept { return m->lastSubsteps; }
PhysicsScene::StepTiming PhysicsScene::lastStepTiming() const noexcept { return m->timing; }

const char* physicsEventKindName(PhysicsEvent::Kind k) noexcept {
    switch (k) {
        case PhysicsEvent::Kind::ContactBegin: return "contact_begin";
        case PhysicsEvent::Kind::ContactEnd: return "contact_end";
        case PhysicsEvent::Kind::TriggerEnter: return "trigger_enter";
        case PhysicsEvent::Kind::TriggerExit: return "trigger_exit";
        case PhysicsEvent::Kind::JointBreak: return "joint_break";
        case PhysicsEvent::Kind::Footstep: return "footstep";
        case PhysicsEvent::Kind::Land: return "land";
    }
    return "?";
}

std::span<const PhysicsEvent> physicsEvents(const World& world) {
    const PhysicsScene* s = physicsScene(world);
    return s ? s->stepEvents() : std::span<const PhysicsEvent>{};
}

physics::backend::IPhysicsBackend& PhysicsScene::backend() { return m->ensureBackend(); }

physics::backend::BodyHandle PhysicsScene::bodyOf(entt::entity e) const {
    const auto it = m->bodies.find(e);
    return it == m->bodies.end() ? pb::INVALID_BODY : it->second.handle;
}

const std::vector<PhysicsWarning>& PhysicsScene::warnings() const noexcept { return m->warnings; }

std::size_t PhysicsScene::bodyBuilds() const noexcept { return m->builds; }

LookResult applyLook(float yawDeg, float pitchDeg, glm::vec2 delta, const PlayerController& pc) noexcept {
    yawDeg -= delta.x * pc.lookDegPerUnit;
    pitchDeg -= delta.y * pc.lookDegPerUnit;
    return {std::remainder(yawDeg, 360.0f), std::clamp(pitchDeg, -89.0f, 89.0f)};
}

PhysicsScene& installPhysics(World& world, AssetResolver& assets) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<PhysicsInstalled>()) return *installed->scene;
    // The system owns the scene: it goes with the World's systems, before the registry.
    auto scene = std::make_shared<PhysicsScene>(reg, assets);
    reg.ctx().emplace<PhysicsInstalled>(PhysicsInstalled{scene.get()});
    // Snapshots carry the simulation, so a restore replays exactly (spec "Snapshots and replay").
    PhysicsScene* raw = scene.get();
    addSnapshotHook(reg, SnapshotHook{"physics", [raw] { return raw->saveSimulation(); },
                                      [raw](const std::shared_ptr<const void>& s) { raw->restoreSimulation(s); },
                                      [raw](StateHash& h) { raw->hashSimulation(h); }});
    // Players spawn before the transform system, so a play scene's first frame is the spawn's.
    world.addSync("player_spawn", [scene](World&) { scene->spawnPlayers(); });
    world.addSync("physics", [scene](World& w) { scene->sync(w.registry()); });
    // Render interpolation, before the render extract reads the poses.
    world.addSync("interpolate", [scene](World& w) { scene->interpolate(w.renderAlpha(), w.pendingLook()); });
    // Once per fixed step, after spin and the fly controller.
    world.addSimulate("physics", [scene](World& w, float dt) { scene->step(w, dt); });
    return *scene;
}

PhysicsScene* physicsScene(World& world) {
    const auto* installed = world.registry().ctx().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

const PhysicsScene* physicsScene(const World& world) {
    const auto* installed = world.registry().ctx().find<PhysicsInstalled>();
    return installed ? installed->scene : nullptr;
}

} // namespace ohao::ecs
