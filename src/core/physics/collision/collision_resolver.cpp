#include "collision_resolver.hpp"
#include "../utils/physics_math.hpp"

namespace ohao {
namespace physics {
namespace collision {

void CollisionResolver::resolveContact(
    const ContactInfo& contact,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB) {
    
    if (!contact.hasContact || !bodyA || !bodyB) return;
    
    
    // Skip if both bodies are static
    if (bodyA->isStatic() && bodyB->isStatic()) {
        return;
    }
    
    // First separate the objects to prevent penetration
    separateObjects(contact, bodyA, bodyB);
    
    // Then resolve velocities for realistic collision response
    resolveVelocity(contact, bodyA, bodyB);
}

void CollisionResolver::resolveContacts(
    const std::vector<ContactInfo>& contacts,
    const std::vector<std::pair<dynamics::RigidBody*, dynamics::RigidBody*>>& bodyPairs) {
    
    if (contacts.size() != bodyPairs.size()) return;
    
    // Resolve each contact individually
    for (size_t i = 0; i < contacts.size(); ++i) {
        resolveContact(contacts[i], bodyPairs[i].first, bodyPairs[i].second);
    }
}

void CollisionResolver::separateObjects(
    const ContactInfo& contact,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB,
    float separationRatio) {
    
    if (!contact.hasContact || contact.penetrationDepth <= 0.0f) return;
    
    
    // Calculate how much to move each object
    float totalInverseMass = bodyA->getInverseMass() + bodyB->getInverseMass();
    
    if (totalInverseMass <= 0.0f) {
        return; // Both objects are static/infinite mass
    }
    
    // Calculate separation vector
    glm::vec3 separation = contact.contactNormal * contact.penetrationDepth * separationRatio;
    
    // Move objects based on their inverse mass ratio
    float moveA = bodyA->getInverseMass() / totalInverseMass;
    float moveB = bodyB->getInverseMass() / totalInverseMass;
    
    
    // Apply position corrections
    if (!bodyA->isStatic()) {
        glm::vec3 newPosA = bodyA->getPosition() - separation * moveA;
        bodyA->setPosition(newPosA);
    }
    
    if (!bodyB->isStatic()) {
        glm::vec3 newPosB = bodyB->getPosition() + separation * moveB;
        bodyB->setPosition(newPosB);
    }
}

void CollisionResolver::resolveVelocity(
    const ContactInfo& contact,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB) {
    
    // Calculate relative velocity at contact point
    glm::vec3 relativeVelocity = calculateRelativeVelocity(contact, bodyA, bodyB);
    
    // Check if objects are separating (relative velocity along normal > 0)
    float separatingVelocity = glm::dot(relativeVelocity, contact.contactNormal);
    
    
    if (separatingVelocity > 0.0f) {
        return; // Objects already separating
    }
    
    // Calculate impulse magnitude needed to resolve collision
    float impulseMagnitude = calculateImpulseMagnitude(contact, relativeVelocity, bodyA, bodyB);
    
    
    // Apply impulse to both bodies
    glm::vec3 impulse = contact.contactNormal * impulseMagnitude;
    
    if (!bodyA->isStatic()) {
        glm::vec3 newVelA = bodyA->getLinearVelocity() - impulse * bodyA->getInverseMass();
        bodyA->setLinearVelocity(newVelA);
    }
    
    if (!bodyB->isStatic()) {
        glm::vec3 newVelB = bodyB->getLinearVelocity() + impulse * bodyB->getInverseMass();
        bodyB->setLinearVelocity(newVelB);
    }
    
    // Apply friction
    applyFriction(contact, relativeVelocity, impulseMagnitude, bodyA, bodyB);
}

glm::vec3 CollisionResolver::calculateRelativeVelocity(
    const ContactInfo& contact,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB) {
    
    // For now, simplified calculation using center of mass velocities
    // TODO: Include angular velocity contributions for more accurate physics
    glm::vec3 velA = bodyA->getLinearVelocity();
    glm::vec3 velB = bodyB->getLinearVelocity();
    
    return velB - velA;
}

float CollisionResolver::calculateImpulseMagnitude(
    const ContactInfo& contact,
    const glm::vec3& relativeVelocity,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB) {
    
    // Relative velocity along contact normal
    float relativeNormalVelocity = glm::dot(relativeVelocity, contact.contactNormal);
    
    // Combined restitution (how bouncy the collision is)
    float restitution = contact.restitution;
    
    // Calculate impulse magnitude: J = -(1 + e) * v_rel / (1/m1 + 1/m2)
    float numerator = -(1.0f + restitution) * relativeNormalVelocity;
    float denominator = bodyA->getInverseMass() + bodyB->getInverseMass();
    
    if (denominator <= 0.0f) {
        return 0.0f; // Both bodies have infinite mass
    }
    
    return numerator / denominator;
}

void CollisionResolver::applyFriction(
    const ContactInfo& contact,
    const glm::vec3& relativeVelocity,
    float normalImpulse,
    dynamics::RigidBody* bodyA,
    dynamics::RigidBody* bodyB) {
    
    // Calculate tangent (friction) direction
    glm::vec3 normal = contact.contactNormal;
    glm::vec3 tangent = relativeVelocity - normal * glm::dot(relativeVelocity, normal);
    
    // Check if there's any tangential movement
    if (math::isNearZero(tangent)) {
        return; // No friction needed
    }
    
    tangent = math::safeNormalize(tangent);
    
    // Calculate friction impulse magnitude
    float tangentVelocity = glm::dot(relativeVelocity, tangent);
    float frictionImpulse = -tangentVelocity / (bodyA->getInverseMass() + bodyB->getInverseMass());
    
    // Limit friction by Coulomb's law: |friction| <= μ * |normal_force|
    float maxFriction = contact.friction * normalImpulse;
    frictionImpulse = glm::clamp(frictionImpulse, -maxFriction, maxFriction);
    
    // Apply friction impulse
    glm::vec3 frictionVector = tangent * frictionImpulse;
    
    if (!bodyA->isStatic()) {
        glm::vec3 newVelA = bodyA->getLinearVelocity() - frictionVector * bodyA->getInverseMass();
        bodyA->setLinearVelocity(newVelA);
    }
    
    if (!bodyB->isStatic()) {
        glm::vec3 newVelB = bodyB->getLinearVelocity() + frictionVector * bodyB->getInverseMass();
        bodyB->setLinearVelocity(newVelB);
    }
}

} // namespace collision
} // namespace physics
} // namespace ohao