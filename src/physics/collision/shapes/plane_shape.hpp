#pragma once

#include "collision_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

class PlaneShape : public CollisionShape {
public:
    // Constructor with normal and distance from origin
    PlaneShape(const glm::vec3& normal, float distance = 0.0f) 
        : CollisionShape(ShapeType::PLANE), m_normal(math::safeNormalize(normal)), m_distance(distance) {}
    
    // Constructor with normal and a point on the plane
    PlaneShape(const glm::vec3& normal, const glm::vec3& pointOnPlane) 
        : CollisionShape(ShapeType::PLANE), m_normal(math::safeNormalize(normal)) {
        m_distance = glm::dot(m_normal, pointOnPlane);
    }
    
    // Getters
    glm::vec3 getNormal() const { return m_normal; }
    float getDistance() const { return m_distance; }
    
    // Get the normal in world space (considering rotation)
    glm::vec3 getWorldNormal(const glm::quat& worldRotation) const {
        return worldRotation * m_normal;
    }
    
    // Get a point on the plane in world space
    glm::vec3 getPointOnPlane(const glm::vec3& worldPosition, const glm::quat& worldRotation) const {
        glm::vec3 worldNormal = getWorldNormal(worldRotation);
        return worldPosition + worldNormal * m_distance;
    }
    
    // CollisionShape interface implementation
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        // Infinite planes don't have meaningful AABBs, but we provide a large finite one
        // for broad-phase collision detection purposes
        const float largeValue = 10000.0f;
        glm::vec3 worldNormal = getWorldNormal(worldRotation);
        glm::vec3 pointOnPlane = getPointOnPlane(worldPosition, worldRotation);
        
        // Create a large AABB centered on a point on the plane
        glm::vec3 extent(largeValue);
        
        // Reduce the extent in the normal direction to make it more like a thick plane
        glm::vec3 absNormal = glm::abs(worldNormal);
        float thickness = 1.0f;
        
        // Find which axis is most aligned with the normal and reduce extent there
        if (absNormal.x > absNormal.y && absNormal.x > absNormal.z) {
            extent.x = thickness;
        } else if (absNormal.y > absNormal.z) {
            extent.y = thickness;
        } else {
            extent.z = thickness;
        }
        
        return math::AABB(pointOnPlane - extent, pointOnPlane + extent);
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, 
                      const glm::quat& shapeRotation) const override {
        // Points are not "inside" an infinite plane, but we can check if they're on it
        float signedDistance = getSignedDistanceToPoint(worldPoint, shapePosition, shapeRotation);
        return glm::abs(signedDistance) < math::constants::EPSILON;
    }
    
    glm::vec3 getSize() const override {
        // Infinite planes don't have a meaningful size
        const float largeValue = 10000.0f;
        return glm::vec3(largeValue);
    }
    
    float getVolume() const override {
        // Infinite planes have zero volume (they're 2D)
        return 0.0f;
    }
    
    // Utility functions for collision detection
    float getSignedDistanceToPoint(const glm::vec3& point, const glm::vec3& position, 
                                  const glm::quat& rotation) const {
        glm::vec3 worldNormal = getWorldNormal(rotation);
        glm::vec3 pointOnPlane = getPointOnPlane(position, rotation);
        
        // Signed distance from point to plane
        return glm::dot(point - pointOnPlane, worldNormal);
    }
    
    glm::vec3 getClosestPointOnPlane(const glm::vec3& point, const glm::vec3& position, 
                                    const glm::quat& rotation) const {
        float signedDistance = getSignedDistanceToPoint(point, position, rotation);
        glm::vec3 worldNormal = getWorldNormal(rotation);
        
        // Project the point onto the plane
        return point - worldNormal * signedDistance;
    }
    
    // Check which side of the plane a point is on
    bool isPointInFrontOfPlane(const glm::vec3& point, const glm::vec3& position, 
                              const glm::quat& rotation) const {
        return getSignedDistanceToPoint(point, position, rotation) > 0.0f;
    }
    
    // Ray-plane intersection (useful for ray tracing)
    struct RayIntersection {
        bool hasIntersection = false;
        float t = 0.0f;  // Parameter along the ray
        glm::vec3 point = glm::vec3(0.0f);  // Intersection point
    };
    
    RayIntersection intersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                const glm::vec3& position, const glm::quat& rotation) const {
        RayIntersection result;
        
        glm::vec3 worldNormal = getWorldNormal(rotation);
        glm::vec3 pointOnPlane = getPointOnPlane(position, rotation);
        
        float denominator = glm::dot(rayDirection, worldNormal);
        
        // Check if ray is parallel to plane
        if (glm::abs(denominator) < math::constants::EPSILON) {
            return result; // No intersection or ray lies in plane
        }
        
        float t = glm::dot(pointOnPlane - rayOrigin, worldNormal) / denominator;
        
        // Check if intersection is in front of ray origin
        if (t >= 0.0f) {
            result.hasIntersection = true;
            result.t = t;
            result.point = rayOrigin + rayDirection * t;
        }
        
        return result;
    }
    
    // Set plane equation directly (ax + by + cz + d = 0 form)
    void setPlaneEquation(const glm::vec4& equation) {
        m_normal = glm::vec3(equation.x, equation.y, equation.z);
        float length = glm::length(m_normal);
        if (length > math::constants::EPSILON) {
            m_normal /= length;
            m_distance = -equation.w / length;
        } else {
            m_normal = glm::vec3(0, 1, 0); // Default up
            m_distance = 0.0f;
        }
    }
    
    glm::vec4 getPlaneEquation(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 worldNormal = getWorldNormal(rotation);
        glm::vec3 pointOnPlane = getPointOnPlane(position, rotation);
        float d = -glm::dot(worldNormal, pointOnPlane);
        return glm::vec4(worldNormal, d);
    }
    
private:
    glm::vec3 m_normal;  // Normal vector of the plane
    float m_distance;    // Distance from origin along the normal
};

} // namespace collision
} // namespace physics
} // namespace ohao