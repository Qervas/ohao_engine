#pragma once

#include "collision_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

class CapsuleShape : public CollisionShape {
public:
    CapsuleShape(float radius, float height) 
        : CollisionShape(ShapeType::CAPSULE), m_radius(radius), m_height(height) {
        // Height is the total height of the capsule (including the spherical ends)
        // The cylindrical part height is m_height - 2 * m_radius
        m_cylinderHeight = glm::max(0.0f, m_height - 2.0f * m_radius);
    }
    
    // Getters
    float getRadius() const { return m_radius; }
    float getHeight() const { return m_height; }
    float getCylinderHeight() const { return m_cylinderHeight; }
    
    // Get the two sphere centers (top and bottom of the capsule)
    glm::vec3 getTopCenter(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        return position + upDirection * (m_cylinderHeight * 0.5f);
    }
    
    glm::vec3 getBottomCenter(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 upDirection = rotation * glm::vec3(0, 1, 0);
        return position - upDirection * (m_cylinderHeight * 0.5f);
    }
    
    // Get the line segment that defines the capsule's core
    void getLineSegment(const glm::vec3& position, const glm::quat& rotation, 
                       glm::vec3& start, glm::vec3& end) const {
        start = getBottomCenter(position, rotation);
        end = getTopCenter(position, rotation);
    }
    
    // CollisionShape interface implementation
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        // For a capsule, we need to consider both the radius and the height
        // The AABB should contain both sphere caps and the cylindrical body
        
        glm::vec3 topCenter = getTopCenter(worldPosition, worldRotation);
        glm::vec3 bottomCenter = getBottomCenter(worldPosition, worldRotation);
        
        // Find the extents by considering both sphere centers + radius
        glm::vec3 minPoint = glm::min(topCenter, bottomCenter) - glm::vec3(m_radius);
        glm::vec3 maxPoint = glm::max(topCenter, bottomCenter) + glm::vec3(m_radius);
        
        return math::AABB(minPoint, maxPoint);
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, 
                      const glm::quat& shapeRotation) const override {
        glm::vec3 start, end;
        getLineSegment(shapePosition, shapeRotation, start, end);
        
        // Find the closest point on the line segment to the test point
        glm::vec3 closestPoint = closestPointOnLineSegment(worldPoint, start, end);
        
        // Check if the distance from the closest point is within the radius
        float distanceSquared = glm::length2(worldPoint - closestPoint);
        return distanceSquared <= (m_radius * m_radius);
    }
    
    glm::vec3 getSize() const override {
        // Return the bounding box size of the capsule
        float diameter = m_radius * 2.0f;
        return glm::vec3(diameter, m_height, diameter);
    }
    
    float getVolume() const override {
        // Volume = cylinder volume + sphere volume
        // V = π * r² * h_cylinder + (4/3) * π * r³
        float cylinderVolume = glm::pi<float>() * m_radius * m_radius * m_cylinderHeight;
        float sphereVolume = (4.0f / 3.0f) * glm::pi<float>() * m_radius * m_radius * m_radius;
        return cylinderVolume + sphereVolume;
    }
    
    // Utility function for collision detection
    glm::vec3 closestPointOnLineSegment(const glm::vec3& point, const glm::vec3& start, const glm::vec3& end) const {
        glm::vec3 segmentVector = end - start;
        float segmentLength = glm::length(segmentVector);
        
        if (segmentLength < math::constants::EPSILON) {
            return start; // Degenerate segment
        }
        
        glm::vec3 segmentDirection = segmentVector / segmentLength;
        glm::vec3 toPoint = point - start;
        
        float projection = glm::dot(toPoint, segmentDirection);
        projection = glm::clamp(projection, 0.0f, segmentLength);
        
        return start + segmentDirection * projection;
    }
    
private:
    float m_radius;
    float m_height;        // Total height including spherical caps
    float m_cylinderHeight; // Height of the cylindrical part only
};

} // namespace collision
} // namespace physics
} // namespace ohao