#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * ForceVolume — applies a constant directional force to any dynamic body
 * whose center is inside the volume (box or sphere).
 *
 * Designed for wind tunnels, jump pads, anti-gravity zones, etc.
 */
class ForceVolume : public GlobalForceGenerator {
public:
    enum class Shape { BOX, SPHERE };

    // Box volume
    ForceVolume(const glm::vec3& center, const glm::vec3& halfExtents,
                const glm::vec3& force)
        : m_center(center), m_halfExtents(halfExtents), m_force(force),
          m_shape(Shape::BOX) {}

    // Sphere volume
    ForceVolume(const glm::vec3& center, float radius, const glm::vec3& force)
        : m_center(center), m_halfExtents(glm::vec3(radius)),
          m_radius(radius), m_force(force), m_shape(Shape::SPHERE) {}

    void setForce(const glm::vec3& force) { m_force = force; }
    glm::vec3 getForce() const { return m_force; }

    void setCenter(const glm::vec3& center) { m_center = center; }
    glm::vec3 getCenter() const { return m_center; }

    void applyForce(dynamics::RigidBody* body, float /*deltaTime*/) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;

    std::string getName() const override { return "ForceVolume"; }

private:
    glm::vec3 m_center{0.0f};
    glm::vec3 m_halfExtents{1.0f};
    float m_radius{1.0f};
    glm::vec3 m_force{0.0f};
    Shape m_shape{Shape::BOX};
};

} // namespace forces
} // namespace physics
} // namespace ohao
