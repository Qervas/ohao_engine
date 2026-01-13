#pragma once
#include <glm/glm.hpp>
#include <array>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace collision {
using dynamics::RigidBody;

// Single contact point
struct ContactPoint {
    glm::vec3 position;         // World space contact position
    glm::vec3 localPosA;        // Local space on body A
    glm::vec3 localPosB;        // Local space on body B
    float normalImpulse;        // Accumulated normal impulse
    float tangentImpulse1;      // Accumulated tangent impulse 1
    float tangentImpulse2;      // Accumulated tangent impulse 2
    float penetration;          // Penetration depth
    uint32_t id;                // Unique ID for persistence

    ContactPoint()
        : position(0.0f)
        , localPosA(0.0f)
        , localPosB(0.0f)
        , normalImpulse(0.0f)
        , tangentImpulse1(0.0f)
        , tangentImpulse2(0.0f)
        , penetration(0.0f)
        , id(0)
    {}
};

// Contact manifold (up to 4 contact points for stability)
class ContactManifold {
public:
    ContactManifold();
    ContactManifold(RigidBody* bodyA, RigidBody* bodyB);

    // Body accessors
    RigidBody* getBodyA() const { return m_bodyA; }
    RigidBody* getBodyB() const { return m_bodyB; }
    void setBodies(RigidBody* bodyA, RigidBody* bodyB) {
        m_bodyA = bodyA;
        m_bodyB = bodyB;
    }

    // Contact points
    int getContactCount() const { return m_contactCount; }
    ContactPoint& getContact(int index) { return m_contacts[index]; }
    const ContactPoint& getContact(int index) const { return m_contacts[index]; }

    // Add contact (with automatic reduction to 4 points)
    void addContact(const glm::vec3& position, const glm::vec3& normal, float penetration);

    // Clear all contacts
    void clear() { m_contactCount = 0; }

    // Collision data
    glm::vec3 getNormal() const { return m_normal; }
    void setNormal(const glm::vec3& normal) { m_normal = normal; }

    glm::vec3 getTangent1() const { return m_tangent1; }
    glm::vec3 getTangent2() const { return m_tangent2; }

    float getFriction() const { return m_friction; }
    void setFriction(float friction) { m_friction = friction; }

    float getRestitution() const { return m_restitution; }
    void setRestitution(float restitution) { m_restitution = restitution; }

    // Update contact frame (normal, tangent basis)
    void updateContactFrame();

    // Warm starting (for persistent contacts)
    void matchContacts(const ContactManifold& oldManifold);

    // Constants
    static constexpr int MAX_CONTACTS = 4;
    static constexpr float CONTACT_MERGE_THRESHOLD = 0.01f;  // 1cm

private:
    // Contact reduction (keep 4 most separated points)
    void reduceContacts();

    // Generate unique contact ID
    uint32_t generateContactID(const glm::vec3& position);

    // Find matching contact from previous frame
    int findMatchingContact(const ContactManifold& oldManifold, uint32_t id) const;

    // Data
    RigidBody* m_bodyA;
    RigidBody* m_bodyB;

    std::array<ContactPoint, MAX_CONTACTS> m_contacts;
    int m_contactCount;

    glm::vec3 m_normal;        // Contact normal (from B to A)
    glm::vec3 m_tangent1;      // First tangent direction
    glm::vec3 m_tangent2;      // Second tangent direction

    float m_friction;
    float m_restitution;
};

}}} // namespace ohao::physics::collision
