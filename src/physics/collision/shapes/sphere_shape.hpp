#pragma once

#include "collision_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

class SphereShape : public CollisionShape {
public:
    SphereShape(float radius) 
        : CollisionShape(ShapeType::SPHERE), m_radius(radius) {}
    
    ~SphereShape() override = default;
    
    // Accessors
    float getRadius() const { return m_radius; }
    void setRadius(float radius) { m_radius = radius; }
    
    // CollisionShape interface
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        glm::vec3 center = worldPosition + m_localPosition;
        glm::vec3 extents(m_radius);
        return math::AABB(center, extents);
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, const glm::quat& shapeRotation) const override {
        glm::vec3 center = shapePosition + m_localPosition;
        float distanceSquared = math::lengthSquared(worldPoint - center);
        return distanceSquared <= (m_radius * m_radius);
    }
    
    glm::vec3 getSize() const override {
        float diameter = m_radius * 2.0f;
        return glm::vec3(diameter);
    }
    
    float getVolume() const override {
        return (4.0f / 3.0f) * math::constants::PI * m_radius * m_radius * m_radius;
    }
    
private:
    float m_radius{1.0f};
};

} // namespace collision
} // namespace physics
} // namespace ohao