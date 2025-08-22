#pragma once

#include "collision_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

class CylinderShape : public CollisionShape {
public:
    CylinderShape(float radius, float height) 
        : CollisionShape(ShapeType::CYLINDER), m_radius(radius), m_height(height) {}
    
    // Getters
    float getRadius() const { return m_radius; }
    float getHeight() const { return m_height; }
    
    // Get cylinder axis endpoints (for collision detection)
    glm::vec3 getTopCenter(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        return position + upDirection * (m_height * 0.5f);
    }
    
    glm::vec3 getBottomCenter(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        return position - upDirection * (m_height * 0.5f);
    }
    
    // CollisionShape interface implementation
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        // For a cylinder aligned with Y-axis, we need to consider rotation
        glm::vec3 upDirection = worldRotation * glm::vec3(0, 1, 0);
        
        // Calculate the extents considering the radius in all directions perpendicular to the axis
        // and half-height along the axis
        glm::vec3 axisExtent = glm::abs(upDirection) * (m_height * 0.5f);
        
        // For the radial extent, we need to find the maximum projection in each axis
        glm::vec3 rightDirection = worldRotation * glm::vec3(1, 0, 0);
        glm::vec3 forwardDirection = worldRotation * glm::vec3(0, 0, 1);
        
        glm::vec3 radialExtent = glm::vec3(
            glm::max(glm::abs(rightDirection.x), glm::abs(forwardDirection.x)) * m_radius,
            glm::max(glm::abs(rightDirection.y), glm::abs(forwardDirection.y)) * m_radius,
            glm::max(glm::abs(rightDirection.z), glm::abs(forwardDirection.z)) * m_radius
        );
        
        glm::vec3 totalExtent = axisExtent + radialExtent;
        
        return math::AABB(worldPosition - totalExtent, worldPosition + totalExtent);
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, 
                      const glm::quat& shapeRotation) const override {
        // Transform point to cylinder's local coordinate system
        glm::mat4 worldToLocal = glm::inverse(getWorldTransform(shapePosition, shapeRotation));
        glm::vec3 localPoint = math::transformPoint(worldPoint, worldToLocal);
        
        // In local space, cylinder is aligned with Y-axis
        // Check height constraint
        if (glm::abs(localPoint.y) > m_height * 0.5f) {
            return false;
        }
        
        // Check radial constraint
        float radialDistance = glm::sqrt(localPoint.x * localPoint.x + localPoint.z * localPoint.z);
        return radialDistance <= m_radius;
    }
    
    glm::vec3 getSize() const override {
        float diameter = m_radius * 2.0f;
        return glm::vec3(diameter, m_height, diameter);
    }
    
    float getVolume() const override {
        // Volume = π * r² * h
        return glm::pi<float>() * m_radius * m_radius * m_height;
    }
    
    // Utility functions for collision detection
    float getDistanceToAxis(const glm::vec3& point, const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        glm::vec3 toPoint = point - position;
        
        // Project onto the axis
        float axisProjection = glm::dot(toPoint, upDirection);
        glm::vec3 axisPoint = position + upDirection * axisProjection;
        
        // Return distance from the axis
        return glm::length(point - axisPoint);
    }
    
    float getAxisProjection(const glm::vec3& point, const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        glm::vec3 toPoint = point - position;
        return glm::dot(toPoint, upDirection);
    }
    
    // Get the closest point on the cylinder surface to a given point
    glm::vec3 getClosestPointOnSurface(const glm::vec3& point, const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        glm::vec3 toPoint = point - position;
        
        // Project onto the axis
        float axisProjection = glm::dot(toPoint, upDirection);
        axisProjection = glm::clamp(axisProjection, -m_height * 0.5f, m_height * 0.5f);
        
        // Find the point on the axis
        glm::vec3 axisPoint = position + upDirection * axisProjection;
        
        // Find the radial direction
        glm::vec3 radialVector = point - axisPoint;
        float radialDistance = glm::length(radialVector);
        
        if (radialDistance < math::constants::EPSILON) {
            // Point is on the axis, choose an arbitrary radial direction
            glm::vec3 rightDirection = rotation * glm::vec3(1, 0, 0);
            return axisPoint + rightDirection * m_radius;
        }
        
        // Scale to surface
        glm::vec3 radialDirection = radialVector / radialDistance;
        return axisPoint + radialDirection * m_radius;
    }
    
private:
    float m_radius;
    float m_height;
};

} // namespace collision
} // namespace physics
} // namespace ohao