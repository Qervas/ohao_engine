#include "collision_shape.hpp"
#include "ray.hpp"
#include <algorithm>
#include <cmath>

namespace ohao {

CollisionShape::CollisionShape()
    : shapeType(Type::BOX)
{
}

void CollisionShape::createBox(const glm::vec3& size) {
    shapeType = Type::BOX;
    boxSize = size;
}

void CollisionShape::createSphere(float radius) {
    shapeType = Type::SPHERE;
    sphereRadius = radius;
}

void CollisionShape::createCapsule(float radius, float height) {
    shapeType = Type::CAPSULE;
    capsuleRadius = radius;
    capsuleHeight = height;
}

void CollisionShape::createConvexHull(const std::vector<glm::vec3>& verts) {
    shapeType = Type::CONVEX_HULL;
    vertices = verts;
    
    // Calculate center as average of vertices
    center = glm::vec3(0.0f);
    for (const auto& v : vertices) {
        center += v;
}
    if (!vertices.empty()) {
        center /= static_cast<float>(vertices.size());
    }
}

void CollisionShape::createTriangleMesh(const std::vector<glm::vec3>& verts, const std::vector<unsigned int>& inds) {
    shapeType = Type::TRIANGLE_MESH;
    vertices = verts;
    indices = inds;
    
    // Calculate center as average of vertices
    center = glm::vec3(0.0f);
    for (const auto& v : vertices) {
        center += v;
    }
    if (!vertices.empty()) {
        center /= static_cast<float>(vertices.size());
    }
}

bool CollisionShape::containsPoint(const glm::vec3& point) const {
    // Point relative to shape center
    glm::vec3 localPoint = point - center;
    
    switch (shapeType) {
        case Type::BOX: {
            // Check if point is inside box
            return std::abs(localPoint.x) <= boxSize.x * 0.5f &&
                   std::abs(localPoint.y) <= boxSize.y * 0.5f &&
                   std::abs(localPoint.z) <= boxSize.z * 0.5f;
        }
        
        case Type::SPHERE: {
            // Check if point is inside sphere
            return glm::length(localPoint) <= sphereRadius;
    }
    
        case Type::CAPSULE: {
            // Check if point is inside capsule
            // First find the line segment endpoints
            glm::vec3 a(0, -capsuleHeight * 0.5f, 0);
            glm::vec3 b(0, capsuleHeight * 0.5f, 0);
            
            // Find closest point on line segment
            float t = glm::dot(localPoint - a, b - a) / glm::dot(b - a, b - a);
            t = std::clamp(t, 0.0f, 1.0f);
            glm::vec3 closestPoint = a + t * (b - a);
            
            // Check if distance to closest point is less than radius
            return glm::length(localPoint - closestPoint) <= capsuleRadius;
    }
        
        case Type::CONVEX_HULL:
        case Type::TRIANGLE_MESH:
            // These require more complex algorithms
            // For now, just return false
            return false;
    }
    
    return false;
}

bool CollisionShape::intersectsRay(const Ray& ray, float& outDistance) const {
    // Simplified implementation for now
    outDistance = -1.0f;
    return false;
}

} // namespace ohao 