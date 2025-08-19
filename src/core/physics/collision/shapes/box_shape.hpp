#pragma once

#include "collision_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

class BoxShape : public CollisionShape {
public:
    BoxShape(const glm::vec3& halfExtents) 
        : CollisionShape(ShapeType::BOX), m_halfExtents(halfExtents) {}
    
    ~BoxShape() override = default;
    
    // Accessors
    const glm::vec3& getHalfExtents() const { return m_halfExtents; }
    void setHalfExtents(const glm::vec3& halfExtents) { m_halfExtents = halfExtents; }
    
    // CollisionShape interface
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        // For rotated boxes, we need to compute the oriented bounding box
        // For now, use conservative AABB (works for axis-aligned boxes)
        glm::vec3 center = worldPosition + m_localPosition;
        
        if (math::isNearZero(glm::length(glm::axis(worldRotation * m_localRotation)))) {
            // No rotation - simple case
            return math::AABB(center, m_halfExtents);
        }
        
        // With rotation - compute all 8 corners and find min/max
        glm::mat4 transform = getWorldTransform(worldPosition, worldRotation);
        
        glm::vec3 corners[8] = {
            glm::vec3(-m_halfExtents.x, -m_halfExtents.y, -m_halfExtents.z),
            glm::vec3( m_halfExtents.x, -m_halfExtents.y, -m_halfExtents.z),
            glm::vec3(-m_halfExtents.x,  m_halfExtents.y, -m_halfExtents.z),
            glm::vec3( m_halfExtents.x,  m_halfExtents.y, -m_halfExtents.z),
            glm::vec3(-m_halfExtents.x, -m_halfExtents.y,  m_halfExtents.z),
            glm::vec3( m_halfExtents.x, -m_halfExtents.y,  m_halfExtents.z),
            glm::vec3(-m_halfExtents.x,  m_halfExtents.y,  m_halfExtents.z),
            glm::vec3( m_halfExtents.x,  m_halfExtents.y,  m_halfExtents.z)
        };
        
        math::AABB aabb;
        aabb.min = aabb.max = math::transformPoint(corners[0], transform);
        
        for (int i = 1; i < 8; ++i) {
            glm::vec3 worldCorner = math::transformPoint(corners[i], transform);
            aabb.min = glm::min(aabb.min, worldCorner);
            aabb.max = glm::max(aabb.max, worldCorner);
        }
        
        return aabb;
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, const glm::quat& shapeRotation) const override {
        // Transform point to local space
        glm::mat4 worldToLocal = glm::inverse(getWorldTransform(shapePosition, shapeRotation));
        glm::vec3 localPoint = math::transformPoint(worldPoint, worldToLocal);
        
        // Check if point is inside box
        return glm::abs(localPoint.x) <= m_halfExtents.x &&
               glm::abs(localPoint.y) <= m_halfExtents.y &&
               glm::abs(localPoint.z) <= m_halfExtents.z;
    }
    
    glm::vec3 getSize() const override {
        return m_halfExtents * 2.0f;
    }
    
    float getVolume() const override {
        glm::vec3 size = getSize();
        return size.x * size.y * size.z;
    }
    
private:
    glm::vec3 m_halfExtents{1.0f};
};

} // namespace collision
} // namespace physics
} // namespace ohao