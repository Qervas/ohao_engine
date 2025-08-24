#pragma once

#include "physics/utils/physics_math.hpp"
#include <memory>
#include <vector>

namespace ohao {
namespace physics {

// Forward declare from physics::dynamics namespace
namespace dynamics { class RigidBody; }

namespace collision {

// Forward declarations
class CollisionShape;

// Single contact point with additional properties
struct ContactPoint {
    glm::vec3 position{0.0f};           // World space contact position
    glm::vec3 localPositionA{0.0f};     // Contact position relative to body A
    glm::vec3 localPositionB{0.0f};     // Contact position relative to body B
    float normalImpulse{0.0f};          // Accumulated normal impulse
    float tangentImpulse1{0.0f};        // Accumulated tangent impulse (friction direction 1)
    float tangentImpulse2{0.0f};        // Accumulated tangent impulse (friction direction 2)
    float bias{0.0f};                   // Positional correction bias
    bool isNew{true};                   // True if this is a new contact this frame
    
    ContactPoint() = default;
    ContactPoint(const glm::vec3& pos) : position(pos) {}
};

// Enhanced contact manifold with multiple contact points
struct ContactManifold {
    // Contact geometry
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // Contact normal (from A to B)
    float penetration{0.0f};             // Maximum penetration depth
    
    // Contact points (up to 4 for stability)
    std::vector<ContactPoint> points;
    static constexpr size_t MAX_CONTACT_POINTS = 4;
    
    // Material properties
    float restitution{0.3f};
    float staticFriction{0.6f};
    float dynamicFriction{0.4f};
    
    // Bodies involved
    dynamics::RigidBody* bodyA{nullptr};
    dynamics::RigidBody* bodyB{nullptr};
    
    // Collision shapes
    std::shared_ptr<CollisionShape> shapeA;
    std::shared_ptr<CollisionShape> shapeB;
    
    // Solver data
    glm::vec3 tangent1{0.0f}; // First friction direction (perpendicular to normal)
    glm::vec3 tangent2{0.0f}; // Second friction direction (perpendicular to normal and tangent1)
    
    // Lifetime management
    float lifetime{0.0f};     // How long this manifold has existed
    bool isActive{false};     // Whether this manifold should be processed
    bool wasColliding{false}; // Was colliding in previous frame
    
    ContactManifold() = default;
    
    // Constructor with normal and penetration
    ContactManifold(const glm::vec3& contactNormal, float pen)
        : normal(math::safeNormalize(contactNormal)), penetration(pen), isActive(true) {
        updateTangents();
    }
    
    // Add a contact point to the manifold
    void addContactPoint(const ContactPoint& point) {
        if (points.size() < MAX_CONTACT_POINTS) {
            points.push_back(point);
        } else {
            // Replace the contact point that would provide the least stability
            replaceWorstContact(point);
        }
    }
    
    // Update friction tangent vectors based on normal
    void updateTangents() {
        if (math::isNearZero(normal)) {
            normal = glm::vec3(0, 1, 0);
        }
        
        // Choose a direction that's not parallel to normal
        glm::vec3 temp = (glm::abs(normal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        
        // Create orthogonal tangent vectors
        tangent1 = math::safeNormalize(glm::cross(normal, temp));
        tangent2 = glm::cross(normal, tangent1);
    }
    
    // Warm start solver with previous frame's impulses
    void warmStart() {
        for (auto& point : points) {
            point.isNew = false;
        }
    }
    
    // Reset accumulated impulses
    void clearImpulses() {
        for (auto& point : points) {
            point.normalImpulse = 0.0f;
            point.tangentImpulse1 = 0.0f;
            point.tangentImpulse2 = 0.0f;
            point.bias = 0.0f;
        }
    }
    
    // Check if manifold is valid
    bool isValid() const {
        return isActive && !points.empty() && penetration > -0.001f && 
               !math::isNearZero(normal) && bodyA && bodyB;
    }
    
    // Get average contact position
    glm::vec3 getAverageContactPoint() const {
        if (points.empty()) return glm::vec3(0.0f);
        
        glm::vec3 avg(0.0f);
        for (const auto& point : points) {
            avg += point.position;
        }
        return avg / static_cast<float>(points.size());
    }
    
    // Update lifetime
    void updateLifetime(float deltaTime) {
        lifetime += deltaTime;
    }
    
private:
    // Replace the contact point that contributes least to stability
    void replaceWorstContact(const ContactPoint& newPoint) {
        if (points.size() < 2) {
            points.push_back(newPoint);
            return;
        }
        
        // Find the contact point that, when removed, maintains the largest area
        // This is a simplified version - proper implementation would use convex hull
        size_t worstIndex = 0;
        float minArea = std::numeric_limits<float>::max();
        
        for (size_t i = 0; i < points.size(); ++i) {
            // Calculate area without this point
            float area = calculateAreaWithoutPoint(i, newPoint);
            if (area < minArea) {
                minArea = area;
                worstIndex = i;
            }
        }
        
        points[worstIndex] = newPoint;
    }
    
    // Helper function to calculate contact area without a specific point
    float calculateAreaWithoutPoint(size_t excludeIndex, const ContactPoint& newPoint) const {
        // Simplified area calculation - in practice would use proper 2D projection
        std::vector<glm::vec3> positions;
        for (size_t i = 0; i < points.size(); ++i) {
            if (i != excludeIndex) {
                positions.push_back(points[i].position);
            }
        }
        positions.push_back(newPoint.position);
        
        if (positions.size() < 3) return 0.0f;
        
        // Project onto plane perpendicular to contact normal and calculate area
        // For simplicity, just return distance-based metric
        float totalDistance = 0.0f;
        for (size_t i = 0; i < positions.size(); ++i) {
            for (size_t j = i + 1; j < positions.size(); ++j) {
                totalDistance += glm::length(positions[i] - positions[j]);
            }
        }
        return totalDistance;
    }
};

// Legacy ContactInfo for backward compatibility
struct ContactInfo {
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f};
    float penetrationDepth{0.0f};
    bool hasContact{false};
    
    // Additional contact properties
    float restitution{0.0f};
    float friction{0.5f};
    
    ContactInfo() = default;
    
    ContactInfo(const glm::vec3& point, const glm::vec3& normal, float depth)
        : contactPoint(point), contactNormal(normal), penetrationDepth(depth), hasContact(true) {}
    
    // Convert to ContactManifold
    ContactManifold toManifold() const {
        ContactManifold manifold(contactNormal, penetrationDepth);
        if (hasContact) {
            ContactPoint point(contactPoint);
            manifold.addContactPoint(point);
        }
        manifold.restitution = restitution;
        manifold.staticFriction = friction;
        manifold.dynamicFriction = friction;
        return manifold;
    }
    
    bool isValid() const {
        return hasContact && penetrationDepth > 0.0f && !math::isNearZero(contactNormal);
    }
    
    void flip() {
        contactNormal = -contactNormal;
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao