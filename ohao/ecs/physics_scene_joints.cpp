#include "ecs/physics_scene_impl.hpp"

namespace ohao::ecs {

// The body an entity is part of: its own, a ColliderPart's target, else its nearest ancestor's.
auto PhysicsScene::Impl::bodyOwner(entt::entity e) const -> entt::entity {
    for (entt::entity x = e; x != entt::null && reg.valid(x);) {
        if (bodies.contains(x)) return x;
        if (const auto it = partOf.find(x); it != partOf.end() && bodies.contains(it->second)) return it->second;
        const auto* p = reg.try_get<Parent>(x);
        x = p ? p->entity : entt::entity{entt::null};
    }
    return entt::null;
}

void PhysicsScene::Impl::destroyJoint(entt::entity e) {
    const auto it = joints.find(e);
    if (it == joints.end()) return;
    if (backend && it->second.handle != pb::INVALID_CONSTRAINT) {
        backend->destroyConstraint(it->second.handle);
        jointOf.erase(it->second.handle);
    }
    joints.erase(it);
}

// Before a body goes: its joints go first (Jolt must not keep a constraint on a removed body)
// and are built again after the bodies.
void PhysicsScene::Impl::destroyJointsOf(entt::entity body) {
    std::vector<entt::entity> list;
    for (const auto& [j, r] : joints)
        if (r.a == body || r.b == body) list.push_back(j);
    sortBySeq(list);
    for (const entt::entity j : list) {
        destroyJoint(j);
        jointsTouched.push_back(j);
    }
}

void PhysicsScene::Impl::sortBySeq(std::vector<entt::entity>& list) const {
    std::sort(list.begin(), list.end(), [&](entt::entity x, entt::entity y) {
        const uint64_t sx = seqOf(x), sy = seqOf(y);
        return sx != sy ? sx < sy : entt::to_integral(x) < entt::to_integral(y);
    });
    list.erase(std::unique(list.begin(), list.end()), list.end());
}

void PhysicsScene::Impl::syncJoints() {
    if (!jointsTouched.empty()) {
        std::vector<entt::entity> list;
        list.swap(jointsTouched);
        sortBySeq(list);
        for (const entt::entity e : list) destroyJoint(e);
        for (const entt::entity e : list)
            if (reg.valid(e) && reg.all_of<Joint>(e)) buildJoint(e);
    }
    if (!jointsBroken.empty()) {
        std::vector<entt::entity> list;
        list.swap(jointsBroken);
        for (const entt::entity e : list) {
            const auto it = joints.find(e);
            if (it == joints.end() || !backend) continue;
            const auto* st = reg.valid(e) ? reg.try_get<JointState>(e) : nullptr;
            backend->setConstraintEnabled(it->second.handle, !(st && st->broken));
        }
    }
}

void PhysicsScene::Impl::buildJoint(entt::entity e) {
    const Joint& j = reg.get<Joint>(e);
    const entt::entity a = bodyOwner(e);
    if (a == entt::null) {
        warn(e, "W_JOINT_NO_BODY", "the joint's entity has no body (no Collider on it or an ancestor); no joint");
        return;
    }
    entt::entity b = entt::null;
    if (j.other != entt::null) {
        b = reg.valid(j.other) ? bodyOwner(j.other) : entt::entity{entt::null};
        if (b == entt::null) {
            warn(e, "W_JOINT_NO_BODY", "the joint's other entity has no body; no joint");
            return;
        }
    }
    if (a == b) {
        warn(e, "W_JOINT_SELF", "the joint joins a body to itself; no joint");
        return;
    }
    const bool aMoves = bodies.at(a).motion != Motion::Static;
    const bool bMoves = b != entt::null && bodies.at(b).motion != Motion::Static;
    if (!aMoves && !bMoves) {
        warn(e, "W_JOINT_STATIC", "the joint joins two bodies that never move; no joint");
        return;
    }
    const auto* wt = reg.try_get<WorldTransform>(e);
    const glm::mat4 world = wt ? wt->world : worldOf(e);
    const glm::vec3 point = glm::vec3(world * glm::vec4(j.anchor, 1.0f));
    glm::vec3 axis = glm::mat3(world) * j.axis;
    axis = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0.0f, 1.0f, 0.0f);

    pb::ConstraintSettings cs;
    cs.space = pb::ConstraintSettings::Space::World;
    cs.body1 = bodies.at(a).handle;
    cs.body2 = b == entt::null ? pb::INVALID_BODY : bodies.at(b).handle;
    cs.anchor1 = cs.anchor2 = point;
    cs.axis1 = cs.axis2 = axis;
    const bool limited = j.limitMin != 0.0f || j.limitMax != 0.0f;
    switch (j.type) {
        case JointType::Fixed: cs.type = pb::ConstraintType::FIXED; break;
        case JointType::Point: cs.type = pb::ConstraintType::POINT; break;
        // Jolt measures body 2 (the other) against body 1 (this): the Joint's limits are this
        // entity's own turn or travel, so they go in negated.
        case JointType::Hinge:
            cs.type = pb::ConstraintType::HINGE;
            cs.enableLimits = limited;
            cs.limitMin = -glm::radians(j.limitMax);
            cs.limitMax = -glm::radians(j.limitMin);
            break;
        case JointType::Slider:
            cs.type = pb::ConstraintType::SLIDER;
            cs.enableLimits = limited;
            cs.limitMin = -j.limitMax;
            cs.limitMax = -j.limitMin;
            break;
        case JointType::SwingTwist:
            cs.type = pb::ConstraintType::SWING_TWIST;
            cs.swingNormalHalfCone = cs.swingPlaneHalfCone = glm::radians(j.swingDeg);
            cs.limitMin = -glm::radians(j.limitMax);
            cs.limitMax = -glm::radians(j.limitMin);
            break;
        case JointType::Distance:
            cs.type = pb::ConstraintType::DISTANCE;
            if (limited) {
                cs.minDistance = j.limitMin;
                cs.maxDistance = j.limitMax;
            }
            cs.enableSpring = j.springHz > 0.0f;
            cs.springFrequency = j.springHz;
            cs.springDamping = j.springDamping;
            break;
    }
    // A Distance joint's other end is the other entity's origin (a rope from a hook).
    if (j.type == JointType::Distance) {
        if (j.other == entt::null) {
            warn(e, "W_JOINT_NO_BODY", "a Distance joint needs another entity, whose origin is its other end; no joint");
            return;
        }
        const auto* ow = reg.try_get<WorldTransform>(j.other);
        cs.anchor2 = glm::vec3((ow ? ow->world : worldOf(j.other))[3]);
    }
    cs.breakingForce = j.breakForce;
    cs.breakingTorque = j.breakTorque;

    pb::IPhysicsBackend& be = ensureBackend();
    const pb::ConstraintHandle h = be.createConstraint(cs);
    if (h == pb::INVALID_CONSTRAINT) {
        warn(e, "W_JOINT_FAILED", "the physics backend could not make the joint");
        return;
    }
    if (const auto* st = reg.try_get<JointState>(e); st && st->broken) be.setConstraintEnabled(h, false);
    joints[e] = JointRecord{h, a, b};
    jointOf[h] = e;
}

// After the substeps: a broken constraint's entity gets JointState and a joint_break event.
void PhysicsScene::Impl::publishBrokenJoints(std::vector<PhysicsEvent>& fresh) {
    for (const pb::ConstraintHandle h : backend->getAndClearBrokenConstraints()) {
        const auto it = jointOf.find(h);
        if (it == jointOf.end() || !reg.valid(it->second)) continue;
        const entt::entity e = it->second;
        writingJoint = true;
        reg.emplace_or_replace<JointState>(e, JointState{true});
        writingJoint = false;
        PhysicsEvent ev;
        ev.kind = PhysicsEvent::Kind::JointBreak;
        ev.a = e;
        ev.aId = idOf(e);
        const JointRecord& r = joints.at(e);
        ev.b = r.b;
        ev.bId = r.b == entt::null ? 0 : idOf(r.b);
        if (const auto* wt = reg.try_get<WorldTransform>(e))
            ev.point = glm::vec3(wt->world * glm::vec4(reg.get<Joint>(e).anchor, 1.0f));
        fresh.push_back(ev);
    }
}

// Every joint's constraint and its entity's id, by handle.
auto PhysicsScene::Impl::jointIdentity() const -> std::vector<std::pair<pb::ConstraintHandle, uint64_t>> {
    std::vector<std::pair<pb::ConstraintHandle, uint64_t>> out;
    out.reserve(jointOf.size());
    for (const auto& [h, e] : jointOf) out.emplace_back(h, idOf(e));
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace ohao::ecs
