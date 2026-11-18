// The physics scene's queries and the calls a game makes (physics/physics_scene.hpp): raycasts, the scene's summary
// and contacts, the players (info, teleport, look), the event log it writes to, whether an entity has a body.
#include "physics/private/physics_scene_impl.hpp"

#include <algorithm>

namespace ohao::ecs {

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

void PhysicsScene::step(World& world, float dt) { m->step(world.inputs(), dt, world.steps()); }
void PhysicsScene::spawnPlayers() { m->spawnPlayers(); }
void PhysicsScene::setInputControl(InputControl fn) { m->inputControl = std::move(fn); }
bool PhysicsScene::hasInputControl() const noexcept { return static_cast<bool>(m->inputControl); }
void PhysicsScene::interpolate(float alpha, glm::vec2 pendingLook) { m->interpolate(alpha, pendingLook); }
bool PhysicsScene::leadsLook() const noexcept { return m->leadsLook; }
bool PhysicsScene::ignoresLook() const noexcept { return m->ignoresLook; }
void PhysicsScene::setEvents(EventLog& log) {
    namespace pe = physics_events;
    m->out.contactBegin = log.writer<ContactEvent>(pe::kContactBegin, "physics");
    m->out.contactEnd = log.writer<ContactEvent>(pe::kContactEnd, "physics");
    m->out.triggerEnter = log.writer<TriggerEvent>(pe::kTriggerEnter, "physics");
    m->out.triggerExit = log.writer<TriggerEvent>(pe::kTriggerExit, "physics");
    m->out.footstep = log.writer<FootEvent>(pe::kFootstep, "physics");
    m->out.land = log.writer<FootEvent>(pe::kLand, "physics");
    m->out.jointBreak = log.writer<JointBreakEvent>(pe::kJointBreak, "physics");
}
uint64_t PhysicsScene::steps() const noexcept { return m->steps; }
int PhysicsScene::lastSubsteps() const noexcept { return m->lastSubsteps; }
PhysicsScene::StepTiming PhysicsScene::lastStepTiming() const noexcept { return m->timing; }

bool PhysicsScene::hasBody(entt::entity e) const {
    const auto it = m->bodies.find(e);
    return it != m->bodies.end() && it->second.handle != pb::INVALID_BODY;
}

const std::vector<PhysicsWarning>& PhysicsScene::warnings() const noexcept { return m->warnings; }

std::size_t PhysicsScene::bodyBuilds() const noexcept { return m->builds; }

LookResult applyLook(float yawDeg, float pitchDeg, glm::vec2 delta, const PlayerController& pc) noexcept {
    yawDeg -= delta.x * pc.lookDegPerUnit;
    pitchDeg -= delta.y * pc.lookDegPerUnit;
    return {std::remainder(yawDeg, 360.0f), std::clamp(pitchDeg, -89.0f, 89.0f)};
}

} // namespace ohao::ecs
