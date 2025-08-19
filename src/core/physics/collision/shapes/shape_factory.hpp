#pragma once

#include "box_shape.hpp"
#include "sphere_shape.hpp"
#include <memory>

namespace ohao {
namespace physics {
namespace collision {

// Factory class for creating collision shapes
class ShapeFactory {
public:
    // Box shapes
    static std::shared_ptr<BoxShape> createBox(const glm::vec3& halfExtents) {
        return std::make_shared<BoxShape>(halfExtents);
    }
    
    static std::shared_ptr<BoxShape> createBox(float width, float height, float depth) {
        return createBox(glm::vec3(width * 0.5f, height * 0.5f, depth * 0.5f));
    }
    
    static std::shared_ptr<BoxShape> createCube(float size) {
        float halfSize = size * 0.5f;
        return createBox(glm::vec3(halfSize));
    }
    
    // Sphere shapes
    static std::shared_ptr<SphereShape> createSphere(float radius) {
        return std::make_shared<SphereShape>(radius);
    }
    
    // Common presets
    static std::shared_ptr<BoxShape> createUnitBox() {
        return createBox(glm::vec3(0.5f)); // 1x1x1 box
    }
    
    static std::shared_ptr<SphereShape> createUnitSphere() {
        return createSphere(0.5f); // Radius 0.5 sphere
    }
    
    static std::shared_ptr<BoxShape> createGroundPlane(float width = 100.0f, float depth = 100.0f, float thickness = 1.0f) {
        return createBox(width, thickness, depth);
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao