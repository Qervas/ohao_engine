#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ohao {

// Forward declaration
class Ray;

class CollisionShape {
public:
    enum class Type {
        BOX,
        SPHERE,
        CAPSULE,
        CONVEX_HULL,
        TRIANGLE_MESH
    };
    
    CollisionShape();
    virtual ~CollisionShape() = default;
    
    // Shape creation
    void createBox(const glm::vec3& size);
    void createSphere(float radius);
    void createCapsule(float radius, float height);
    void createConvexHull(const std::vector<glm::vec3>& vertices);
    void createTriangleMesh(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices);
    
    // Shape type
    Type getType() const { return shapeType; }
    
    // Bounding info
    virtual glm::vec3 getCenter() const { return center; }
    virtual void setCenter(const glm::vec3& newCenter) { this->center = newCenter; }
    
    // Collision tests
    virtual bool containsPoint(const glm::vec3& point) const;
    virtual bool intersectsRay(const Ray& ray, float& outDistance) const;
    
    // Shape parameters
    const glm::vec3& getBoxSize() const { return boxSize; }
    float getSphereRadius() const { return sphereRadius; }
    float getCapsuleRadius() const { return capsuleRadius; }
    float getCapsuleHeight() const { return capsuleHeight; }
    
protected:
    Type shapeType;
    glm::vec3 center{0.0f};
    
    // Shape-specific parameters
    glm::vec3 boxSize{1.0f};
    float sphereRadius{0.5f};
    float capsuleRadius{0.5f};
    float capsuleHeight{1.0f};
    
    // Mesh data
    std::vector<glm::vec3> vertices;
    std::vector<unsigned int> indices;
};

} // namespace ohao 