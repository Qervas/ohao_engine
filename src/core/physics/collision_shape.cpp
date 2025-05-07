#include "collision_shape.hpp"
// Enable experimental GLM features
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

namespace ohao {

// CollisionShape base class
CollisionShape::CollisionShape(ShapeType type)
    : type(type), localCenter(0.0f)
{
}

// BoxShape implementation
BoxShape::BoxShape(const glm::vec3& halfExtents)
    : CollisionShape(ShapeType::BOX), halfExtents(halfExtents)
{
}

bool BoxShape::containsPoint(const glm::vec3& point) const {
    // Check if point is within the box bounds
    glm::vec3 localPoint = point - localCenter;
    return (
        localPoint.x >= -halfExtents.x && localPoint.x <= halfExtents.x &&
        localPoint.y >= -halfExtents.y && localPoint.y <= halfExtents.y &&
        localPoint.z >= -halfExtents.z && localPoint.z <= halfExtents.z
    );
}

// SphereShape implementation
SphereShape::SphereShape(float radius)
    : CollisionShape(ShapeType::SPHERE), radius(radius)
{
}

bool SphereShape::containsPoint(const glm::vec3& point) const {
    // Check if point is within sphere radius
    return glm::distance2(point, localCenter) <= radius * radius;
}

// CapsuleShape implementation
CapsuleShape::CapsuleShape(float radius, float height)
    : CollisionShape(ShapeType::CAPSULE), radius(radius), height(height)
{
}

bool CapsuleShape::containsPoint(const glm::vec3& point) const {
    // Simplified check - not accurate for a capsule
    // For proper capsule containment, we'd need to check distance to line segment
    
    // Check along the height axis (assumed to be y-axis)
    glm::vec3 localPoint = point - localCenter;
    
    // Check if within the cylindrical part
    if (fabs(localPoint.y) <= height * 0.5f) {
        float dist2D = glm::length(glm::vec2(localPoint.x, localPoint.z));
        return dist2D <= radius;
    }
    
    // Check if within one of the spherical caps
    float distToEndpoint = glm::length(localPoint - glm::vec3(0.0f, localPoint.y > 0 ? height * 0.5f : -height * 0.5f, 0.0f));
    return distToEndpoint <= radius;
}

// ConvexHullShape implementation
ConvexHullShape::ConvexHullShape(const std::vector<glm::vec3>& points)
    : CollisionShape(ShapeType::CONVEX_HULL), points(points)
{
    // Calculate center point as average of all vertices
    if (!points.empty()) {
        glm::vec3 sum(0.0f);
        for (const auto& p : points) {
            sum += p;
        }
        localCenter = sum / static_cast<float>(points.size());
    }
}

bool ConvexHullShape::containsPoint(const glm::vec3& point) const {
    // This is a simplified implementation
    // For a proper convex hull check, we would need to test if the point
    // is inside all half-spaces defined by the convex hull faces
    
    // For now, just check if the point is close to the center
    // This is just a placeholder and NOT correct for actual collision detection!
    return glm::distance(point, localCenter) < 1.0f; 
}

} // namespace ohao 