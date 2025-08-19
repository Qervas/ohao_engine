#pragma once

#include "../../utils/physics_math.hpp"
#include <memory>
#include <vector>

namespace ohao {
namespace physics {
namespace collision {

enum class ShapeType {
    BOX = 0,
    SPHERE = 1,
    CAPSULE = 2,
    CONVEX_HULL = 3,
    MESH = 4
};

// Base class for all collision shapes
class CollisionShape {
public:
    using Ptr = std::shared_ptr<CollisionShape>;
    
    CollisionShape(ShapeType type) : m_type(type) {}
    virtual ~CollisionShape() = default;
    
    // Core interface
    ShapeType getType() const { return m_type; }
    
    // Transform properties
    void setLocalTransform(const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0)) {
        m_localPosition = position;
        m_localRotation = rotation;
    }
    
    glm::vec3 getLocalPosition() const { return m_localPosition; }
    glm::quat getLocalRotation() const { return m_localRotation; }
    
    // Abstract interface - must be implemented by derived classes
    virtual math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const = 0;
    virtual bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, const glm::quat& shapeRotation) const = 0;
    virtual glm::vec3 getSize() const = 0;
    virtual float getVolume() const = 0;
    
    // Utility methods
    glm::mat4 getWorldTransform(const glm::vec3& worldPosition, const glm::quat& worldRotation) const {
        return math::createTransformMatrix(worldPosition + m_localPosition, worldRotation * m_localRotation);
    }
    
protected:
    ShapeType m_type;
    glm::vec3 m_localPosition{0.0f};
    glm::quat m_localRotation{1, 0, 0, 0};
};

} // namespace collision
} // namespace physics
} // namespace ohao