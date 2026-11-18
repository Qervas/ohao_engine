// The physics scene's bodies (physics/physics_scene.hpp): built from their entities' colliders and rigid bodies, kept in
// step with their components (syncBodies, sync), and destroyed with their pairs.
#include "physics/private/physics_scene_impl.hpp"
#include "visual/components.hpp"

#include "core/jobs/graph.hpp"

#include <algorithm>

namespace ohao::ecs {

// A removed body's pairs end now (Jolt's later OnContactRemoved for it no longer maps).
void PhysicsScene::Impl::endPairsOf(pb::BodyHandle h) {
    for (auto it = pairs.begin(); it != pairs.end();) {
        if (pb::BodyHandle(it->first >> 32) != h && pb::BodyHandle(it->first & 0xffffffffu) != h) {
            ++it;
            continue;
        }
        if (it->second.dormant) --dormant;
        emit(endEvent(it->second));
        it = pairs.erase(it);
    }
}

auto PhysicsScene::Impl::endEvent(const Pair& p) const -> PhysicsEvent {
    PhysicsEvent ev;
    ev.kind = p.trigger ? PhysicsEvent::Kind::TriggerExit : PhysicsEvent::Kind::ContactEnd;
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
        if (auto s = shapeOf(e, reg.get<Collider>(e), body.scale, rb.motion, keep, record.kind, record.shared))
            children.push_back({std::move(*s), glm::vec3(0.0f), glm::quat(1, 0, 0, 0)});
    const glm::quat inverse = glm::inverse(body.rotation);
    for (const entt::entity p : parts) {
        const auto* c = reg.try_get<Collider>(p);
        const auto* pw = reg.try_get<WorldTransform>(p);
        if (!c || c->shape == ColliderShape::None || !pw) continue;
        const Placement part = decompose(pw->world);
        BodyRecord::Kind ignored{};
        if (auto s = shapeOf(p, *c, part.scale, rb.motion, keep, ignored, record.shared))
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
    if (rb.motion == Motion::Static) ++staticsBuilt;
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
    // The moved entities' world placements, decomposed on the job pool, each into its own slot (chunks of
    // kDecomposesPerChunk, step 4b-6: about 30 ns one, so a chunk is about 8 us); physics writes no WorldTransform,
    // so they hold to the end.
    constexpr std::size_t kDecomposesPerChunk = 256;
    movedEntities.clear();
    for (const entt::entity e : reg.view<PhysicsMoved>()) movedEntities.push_back(e);
    movedPlacements.resize(movedEntities.size());
    if (!movedEntities.empty()) {
        const auto& worlds = reg.storage<WorldTransform>();
        jobs::parallelFor(0, movedEntities.size(), kDecomposesPerChunk, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i)
                if (worlds.contains(movedEntities[i])) movedPlacements[i] = decompose(worlds.get(movedEntities[i]).world);
        }, "physics.moved");
    }
    std::vector<std::size_t> moved;   // into movedEntities
    for (std::size_t i = 0; i < movedEntities.size(); ++i) {
        const entt::entity e = movedEntities[i];
        if (reg.all_of<ColliderPart>(e)) {
            addBodyOf(e);
            continue;
        }
        const auto it = bodies.find(e);
        if (it == bodies.end()) continue;
        if (it->second.hasParts || differs(movedPlacements[i].scale, it->second.scale, 1e-6f)) rebuild.push_back(e);
        else moved.push_back(i);
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
    // Static bodies added one at a time leave the broad phase's tree as they came: rebuilt here, so that a query
    // before the next step is as fast as one after it.
    if (staticsBuilt > 0 && backend) {
        backend->optimizeBroadPhase();
        staticsBuilt = 0;
    }

    syncJoints();

    // A snapshot's simulation, restored: every body as it was, so a body whose entity the
    // restore put back where its body now is stays (no teleport, which would drop the contact
    // cache); one the snapshot had already moved without a step goes there below.
    const bool restored = savedPending && restoreSaved();

    // 3. Moved bodies follow their entities. A dynamic body's entity moved by the write-back
    // (its world pose is the one written) stays; moved by anything else, the body jumps
    // there, at rest.
    for (const std::size_t i : moved) {
        const entt::entity e = movedEntities[i];
        const auto it = bodies.find(e);
        if (it == bodies.end() || !backend) continue;
        BodyRecord& r = it->second;
        const Placement& p = movedPlacements[i];
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

} // namespace ohao::ecs
