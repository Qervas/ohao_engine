#pragma once

#include "physics/utils/physics_math.hpp"

#include <memory>
#include <concepts>

namespace ohao {
namespace physics {
namespace collision {

enum class ShapeType {
    BOX = 0,
    SPHERE = 1,
    CAPSULE = 2,
    CYLINDER = 3,
    PLANE = 4,
    CONVEX_HULL = 5,
    TRIANGLE_MESH = 6,
    CONE = 7,
    ELLIPSOID = 8
};

// Base class for all collision shapes
class CollisionShape {
public:
    using Ptr = std::shared_ptr<CollisionShape>;

    explicit CollisionShape(ShapeType type) : m_type(type) {}
    virtual ~CollisionShape() = default;

    [[nodiscard]] ShapeType getType() const noexcept { return m_type; }

    void setLocalTransform(const glm::vec3& position,
                           const glm::quat& rotation = glm::quat(1, 0, 0, 0)) {
        m_localPosition = position;
        m_localRotation = rotation;
    }

    [[nodiscard]] glm::vec3 getLocalPosition() const noexcept { return m_localPosition; }
    [[nodiscard]] glm::quat getLocalRotation() const noexcept { return m_localRotation; }

    [[nodiscard]] virtual math::AABB getAABB(const glm::vec3& worldPosition,
                                             const glm::quat& worldRotation) const = 0;
    [[nodiscard]] virtual bool containsPoint(const glm::vec3& worldPoint,
                                             const glm::vec3& shapePosition,
                                             const glm::quat& shapeRotation) const = 0;
    [[nodiscard]] virtual glm::vec3 getSize() const = 0;
    [[nodiscard]] virtual float getVolume() const = 0;

    [[nodiscard]] glm::mat4 getWorldTransform(const glm::vec3& worldPosition,
                                             const glm::quat& worldRotation) const {
        return math::createTransformMatrix(worldPosition + m_localPosition,
                                           worldRotation * m_localRotation);
    }

protected:
    ShapeType m_type;
    glm::vec3 m_localPosition{0.0f};
    glm::quat m_localRotation{1, 0, 0, 0};
};

template<typename T>
concept CollisionShapeLike = requires(const T& s, glm::vec3 p, glm::quat q) {
    { s.getType() } -> std::convertible_to<ShapeType>;
    { s.getAABB(p, q) } -> std::same_as<math::AABB>;
    { s.getVolume() } -> std::convertible_to<float>;
    { s.getSize() } -> std::convertible_to<glm::vec3>;
};

} // namespace collision
} // namespace physics
} // namespace ohao
