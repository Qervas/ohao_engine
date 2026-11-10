#include "ecs/physics_scene_impl.hpp"

namespace ohao::ecs {

// The step's contact and trigger events, from the backend's queue (filled by Jolt's callbacks
// during the substeps, on whatever thread; drained here on the simulating one).
void PhysicsScene::Impl::drainContacts(std::vector<PhysicsEvent>& out) {
    for (const pb::ContactEvent& ce : backend->getContactEvents()) {
        if (ce.type == pb::ContactEvent::Type::PERSIST) continue;
        if (ce.body1 == pb::INVALID_BODY || ce.body2 == pb::INVALID_BODY) continue;   // a removed body's
        const uint64_t key = pairKey(ce.body1, ce.body2);
        if (ce.type == pb::ContactEvent::Type::END) {
            const auto it = pairs.find(key);
            if (it == pairs.end() || it->second.count == 0) continue;   // ended when its body was removed
            if (--it->second.count == 0) {
                if (asleep(key)) {
                    it->second.dormant = true;   // removed because its bodies went to sleep
                    ++dormant;
                } else {
                    out.push_back(endEvent(it->second));
                    pairs.erase(it);
                }
            }
            continue;
        }
        const auto e1 = entityOf.find(ce.body1), e2 = entityOf.find(ce.body2);
        if (e1 == entityOf.end() || e2 == entityOf.end()) continue;
        Pair& p = pairs[key];
        if (p.count++ > 0) continue;
        if (p.dormant) {   // woken still touching: the same contact
            p.dormant = false;
            --dormant;
            continue;
        }
        const auto r1 = bodies.find(e1->second), r2 = bodies.find(e2->second);
        const bool t1 = r1 != bodies.end() && r1->second.trigger, t2 = r2 != bodies.end() && r2->second.trigger;
        p.trigger = t1 || t2;
        const uint64_t id1 = idOf(e1->second), id2 = idOf(e2->second);
        // Contacts: a has the lower id; triggers: a is the trigger. Jolt's normal points from
        // body 1 toward body 2; ours from a toward b.
        const bool swap = p.trigger ? (!t1 && t2) : id2 < id1;
        p.a = swap ? e2->second : e1->second;
        p.b = swap ? e1->second : e2->second;
        p.aId = swap ? id2 : id1;
        p.bId = swap ? id1 : id2;
        p.point = ce.contactPoint;
        p.since = steps;
        const float n = glm::length(ce.contactNormal);
        p.normal = n > 0.0f ? (swap ? -ce.contactNormal : ce.contactNormal) / n : glm::vec3(0.0f);
        PhysicsEvent ev;
        ev.kind = p.trigger ? PhysicsEvent::Kind::TriggerEnter : PhysicsEvent::Kind::ContactBegin;
        ev.aId = p.aId;
        ev.bId = p.bId;
        ev.point = p.point;
        ev.normal = p.normal;
        ev.speed = std::fabs(glm::dot(ce.relativeVelocity, p.normal));
        out.push_back(ev);
    }
    // Dormant pairs whose bodies woke and were not added again have come apart.
    if (dormant == 0) return;
    for (auto it = pairs.begin(); it != pairs.end();) {
        if (!it->second.dormant || asleep(it->first)) {
            ++it;
            continue;
        }
        --dormant;
        out.push_back(endEvent(it->second));
        it = pairs.erase(it);
    }
}

// Every moving body of the pair asleep (a static one never wakes).
auto PhysicsScene::Impl::asleep(uint64_t key) const -> bool {
    for (const pb::BodyHandle h : {pb::BodyHandle(key >> 32), pb::BodyHandle(key & 0xffffffffu)}) {
        const auto e = entityOf.find(h);
        if (e == entityOf.end()) continue;
        if (players.contains(e->second)) return false;   // a character's inner body never sleeps
        const auto r = bodies.find(e->second);
        if (r == bodies.end() || r->second.motion == Motion::Static) continue;
        if (backend->isAwake(h)) return false;
    }
    return true;
}

auto PhysicsScene::Impl::idOf(entt::entity e) const -> uint64_t {
    const auto* id = reg.valid(e) ? reg.try_get<EntityId>(e) : nullptr;
    return id ? id->value : 0;
}

// Every dynamic body awake now or at the end of the last step: its pose into its entity's
// Transform, through its parent's world transform (the local scale is kept), and its motion
// into BodyMotion (asleep at rest, once, as it falls asleep).
void PhysicsScene::Impl::writeBack() {
    for (auto& [e, r] : bodies) {
        if (r.motion != Motion::Dynamic) continue;
        const bool awake = backend->isAwake(r.handle);
        if (!awake && !r.awake) continue;
        r.awake = awake;
        writeMotion(e, awake ? BodyMotion{backend->getLinearVelocity(r.handle), backend->getAngularVelocity(r.handle), false}
                             : BodyMotion{glm::vec3(0.0f), glm::vec3(0.0f), true});
        const glm::vec3 pos = backend->getPosition(r.handle);
        const glm::quat rot = backend->getRotation(r.handle);
        recordMove(e, r.wpos, r.wrot, pos, rot);
        r.wpos = pos;
        r.wrot = rot;
        glm::vec3 localPos = pos;
        glm::quat localRot = rot;
        if (const auto* parent = reg.try_get<Parent>(e); parent && reg.valid(parent->entity))
            if (const auto* pw = reg.try_get<WorldTransform>(parent->entity)) {
                const Placement pp = decompose(pw->world);
                if (nonUniform(pp.scale))
                    warn(e, "W_PHYSICS_PARENT_SCALE",
                         "a dynamic body under a parent scaled unevenly: its pose is written approximately");
                localPos = glm::vec3(glm::inverse(pw->world) * glm::vec4(pos, 1.0f));
                localRot = glm::normalize(glm::inverse(pp.rotation) * rot);
            }
        writePose(e, [&](Transform& t) {
            t.position = localPos;
            t.rotation = localRot;
        });
        ++timing.writes;
    }
}

void PhysicsScene::Impl::publish(std::vector<PhysicsEvent>& fresh) {
    std::stable_sort(fresh.begin(), fresh.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        const int kx = kindRank(x.kind), ky = kindRank(y.kind);
        if (kx != ky) return kx < ky;
        return x.aId != y.aId ? x.aId < y.aId : x.bId < y.bId;
    });
    for (const PhysicsEvent& ev : fresh) emit(ev);   // after the removals' ends, emitted as they happened
}

void PhysicsScene::Impl::emit(const PhysicsEvent& ev) {
    using K = PhysicsEvent::Kind;
    switch (ev.kind) {
        case K::ContactBegin:
        case K::ContactEnd: {
            const auto& w = ev.kind == K::ContactBegin ? out.contactBegin : out.contactEnd;
            if (w) w.emit(ContactEvent{ev.aId, ev.bId, ev.point, ev.normal, ev.speed});
            break;
        }
        case K::TriggerEnter:
        case K::TriggerExit: {
            const auto& w = ev.kind == K::TriggerEnter ? out.triggerEnter : out.triggerExit;
            if (w) w.emit(TriggerEvent{ev.aId, ev.bId, ev.point, ev.normal, ev.speed});
            break;
        }
        case K::Footstep:
        case K::Land: {
            const auto& w = ev.kind == K::Footstep ? out.footstep : out.land;
            if (w) w.emit(FootEvent{ev.aId, ev.bId, ev.point, ev.normal, ev.speed, ev.surface});
            break;
        }
        case K::JointBreak:
            if (out.jointBreak) out.jointBreak.emit(JointBreakEvent{ev.aId, ev.bId, ev.point});
            break;
    }
}

} // namespace ohao::ecs
