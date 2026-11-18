// The physics scene's simulation state in snapshots (physics/physics_scene.hpp): saved, restored and hashed with the
// world (the snapshot hook), bodies matched to their entities by id.
#include "physics/private/physics_scene_impl.hpp"

#include <algorithm>

namespace ohao::ecs {

struct PhysicsScene::Impl::SavedSimulation {
    std::string backend;                                        // bodies, contact cache, characters
    std::vector<std::pair<pb::BodyHandle, uint64_t>> bodies;    // which body is whose (bodyIdentity)
    std::vector<std::pair<pb::ConstraintHandle, uint64_t>> joints;   // which constraint is whose (jointIdentity)
    std::vector<SavedPlayer> players;
    std::vector<std::pair<uint64_t, Pair>> pairs;               // the contact bookkeeping, entities by id
    std::size_t dormant{0};
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
    return s;
}

void PhysicsScene::Impl::restoreSimulation(const std::shared_ptr<const void>& state) {
    savedPending = std::static_pointer_cast<const SavedSimulation>(state);
    restoredSinceSpawn = true;
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

} // namespace ohao::ecs
