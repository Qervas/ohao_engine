#include "force_volume.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

bool ForceVolume::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || !body->isDynamic()) return false;
    glm::vec3 pos = body->getPosition();
    if (m_shape == Shape::SPHERE) {
        glm::vec3 d = pos - m_center;
        return glm::dot(d, d) <= m_radius * m_radius;
    }
    // BOX — axis-aligned
    glm::vec3 d = glm::abs(pos - m_center);
    return d.x <= m_halfExtents.x && d.y <= m_halfExtents.y && d.z <= m_halfExtents.z;
}

void ForceVolume::applyForce(dynamics::RigidBody* body, float /*deltaTime*/) {
    body->applyForce(m_force);
}

} // namespace forces
} // namespace physics
} // namespace ohao
