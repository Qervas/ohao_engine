#pragma once

#include "contact_info.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <vector>

namespace ohao {
namespace physics {
namespace collision {

// Collision response and resolution
class CollisionResolver {
public:
    // Resolve a single collision contact
    static void resolveContact(
        const ContactInfo& contact,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
    
    // Resolve multiple contacts (for better stability)
    static void resolveContacts(
        const std::vector<ContactInfo>& contacts,
        const std::vector<std::pair<dynamics::RigidBody*, dynamics::RigidBody*>>& bodyPairs
    );
    
    // Position correction to separate overlapping objects
    static void separateObjects(
        const ContactInfo& contact,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB,
        float separationRatio = 0.8f  // How much to separate (0-1)
    );
    
    // Velocity resolution for realistic bounce/friction
    static void resolveVelocity(
        const ContactInfo& contact,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
    
private:
    // Calculate relative velocity at contact point
    static glm::vec3 calculateRelativeVelocity(
        const ContactInfo& contact,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
    
    // Calculate impulse magnitude for collision response
    static float calculateImpulseMagnitude(
        const ContactInfo& contact,
        const glm::vec3& relativeVelocity,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
    
    // Apply friction forces
    static void applyFriction(
        const ContactInfo& contact,
        const glm::vec3& relativeVelocity,
        float normalImpulse,
        dynamics::RigidBody* bodyA,
        dynamics::RigidBody* bodyB
    );
};

} // namespace collision
} // namespace physics
} // namespace ohao