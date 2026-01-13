#include "contact_manifold.hpp"
#include "dynamics/rigid_body.hpp"
#include <algorithm>
#include <cmath>

namespace ohao {
namespace physics {
namespace collision {

ContactManifold::ContactManifold()
    : m_bodyA(nullptr)
    , m_bodyB(nullptr)
    , m_contactCount(0)
    , m_normal(0.0f, 1.0f, 0.0f)
    , m_tangent1(1.0f, 0.0f, 0.0f)
    , m_tangent2(0.0f, 0.0f, 1.0f)
    , m_friction(0.5f)
    , m_restitution(0.0f)
{
}

ContactManifold::ContactManifold(RigidBody* bodyA, RigidBody* bodyB)
    : m_bodyA(bodyA)
    , m_bodyB(bodyB)
    , m_contactCount(0)
    , m_normal(0.0f, 1.0f, 0.0f)
    , m_tangent1(1.0f, 0.0f, 0.0f)
    , m_tangent2(0.0f, 0.0f, 1.0f)
    , m_friction(0.5f)
    , m_restitution(0.0f)
{
}

void ContactManifold::addContact(const glm::vec3& position, const glm::vec3& normal, float penetration) {
    // Generate contact ID
    uint32_t id = generateContactID(position);

    // Check if this contact already exists (merge nearby contacts)
    for (int i = 0; i < m_contactCount; ++i) {
        float dist = glm::length(m_contacts[i].position - position);
        if (dist < CONTACT_MERGE_THRESHOLD) {
            // Merge with existing contact
            m_contacts[i].position = position;
            m_contacts[i].penetration = std::max(m_contacts[i].penetration, penetration);
            return;
        }
    }

    // Add new contact
    if (m_contactCount < MAX_CONTACTS) {
        ContactPoint& contact = m_contacts[m_contactCount];
        contact.position = position;
        contact.penetration = penetration;
        contact.id = id;

        // Compute local positions
        if (m_bodyA) {
            glm::mat4 invTransformA = glm::inverse(m_bodyA->getTransformMatrix());
            contact.localPosA = glm::vec3(invTransformA * glm::vec4(position, 1.0f));
        }
        if (m_bodyB) {
            glm::mat4 invTransformB = glm::inverse(m_bodyB->getTransformMatrix());
            contact.localPosB = glm::vec3(invTransformB * glm::vec4(position, 1.0f));
        }

        ++m_contactCount;
    } else {
        // Need to reduce contacts - add then reduce
        ContactPoint newContact;
        newContact.position = position;
        newContact.penetration = penetration;
        newContact.id = id;

        if (m_bodyA) {
            glm::mat4 invTransformA = glm::inverse(m_bodyA->getTransformMatrix());
            newContact.localPosA = glm::vec3(invTransformA * glm::vec4(position, 1.0f));
        }
        if (m_bodyB) {
            glm::mat4 invTransformB = glm::inverse(m_bodyB->getTransformMatrix());
            newContact.localPosB = glm::vec3(invTransformB * glm::vec4(position, 1.0f));
        }

        // Temporarily add 5th contact
        m_contacts[MAX_CONTACTS - 1] = newContact;
        m_contactCount = MAX_CONTACTS;

        // Reduce back to 4
        reduceContacts();
    }

    // Update normal
    m_normal = normal;
    updateContactFrame();
}

void ContactManifold::reduceContacts() {
    if (m_contactCount <= MAX_CONTACTS) return;

    // Keep the 4 most separated points for stability
    // Algorithm: Find point with max distance to existing set

    // Start with deepest penetration
    int deepest = 0;
    for (int i = 1; i < m_contactCount; ++i) {
        if (m_contacts[i].penetration > m_contacts[deepest].penetration) {
            deepest = i;
        }
    }

    std::array<int, MAX_CONTACTS> keep;
    keep[0] = deepest;
    int keepCount = 1;

    // Find point furthest from first
    float maxDist = 0.0f;
    int furthest = -1;
    for (int i = 0; i < m_contactCount; ++i) {
        if (i == deepest) continue;
        float dist = glm::length(m_contacts[i].position - m_contacts[deepest].position);
        if (dist > maxDist) {
            maxDist = dist;
            furthest = i;
        }
    }
    if (furthest >= 0) {
        keep[keepCount++] = furthest;
    }

    // Find remaining points that maximize area
    for (int remaining = keepCount; remaining < MAX_CONTACTS && remaining < m_contactCount; ++remaining) {
        float maxArea = 0.0f;
        int best = -1;

        for (int i = 0; i < m_contactCount; ++i) {
            // Skip already kept points
            bool alreadyKept = false;
            for (int j = 0; j < keepCount; ++j) {
                if (i == keep[j]) {
                    alreadyKept = true;
                    break;
                }
            }
            if (alreadyKept) continue;

            // Compute area contribution
            float area = 0.0f;
            if (keepCount >= 2) {
                glm::vec3 a = m_contacts[keep[0]].position;
                glm::vec3 b = m_contacts[keep[1]].position;
                glm::vec3 c = m_contacts[i].position;
                area = glm::length(glm::cross(b - a, c - a));
            }

            if (area > maxArea) {
                maxArea = area;
                best = i;
            }
        }

        if (best >= 0) {
            keep[keepCount++] = best;
        }
    }

    // Compact array
    std::array<ContactPoint, MAX_CONTACTS> temp = m_contacts;
    for (int i = 0; i < keepCount; ++i) {
        m_contacts[i] = temp[keep[i]];
    }
    m_contactCount = keepCount;
}

void ContactManifold::updateContactFrame() {
    // Build orthogonal tangent basis from normal
    if (std::abs(m_normal.x) > std::abs(m_normal.y)) {
        // Use Y-axis to compute first tangent
        m_tangent1 = glm::normalize(glm::vec3(-m_normal.z, 0.0f, m_normal.x));
    } else {
        // Use X-axis to compute first tangent
        m_tangent1 = glm::normalize(glm::vec3(0.0f, m_normal.z, -m_normal.y));
    }

    m_tangent2 = glm::cross(m_normal, m_tangent1);
}

uint32_t ContactManifold::generateContactID(const glm::vec3& position) {
    // Simple hash of position for contact ID
    uint32_t hash = 0;
    hash ^= static_cast<uint32_t>(position.x * 73856093);
    hash ^= static_cast<uint32_t>(position.y * 19349663);
    hash ^= static_cast<uint32_t>(position.z * 83492791);
    return hash;
}

void ContactManifold::matchContacts(const ContactManifold& oldManifold) {
    // Match contacts from previous frame for warm starting
    for (int i = 0; i < m_contactCount; ++i) {
        int match = findMatchingContact(oldManifold, m_contacts[i].id);
        if (match >= 0) {
            // Transfer cached impulses (warm starting)
            m_contacts[i].normalImpulse = oldManifold.m_contacts[match].normalImpulse * 0.8f;
            m_contacts[i].tangentImpulse1 = oldManifold.m_contacts[match].tangentImpulse1 * 0.8f;
            m_contacts[i].tangentImpulse2 = oldManifold.m_contacts[match].tangentImpulse2 * 0.8f;
        }
    }
}

int ContactManifold::findMatchingContact(const ContactManifold& oldManifold, uint32_t id) const {
    for (int i = 0; i < oldManifold.m_contactCount; ++i) {
        if (oldManifold.m_contacts[i].id == id) {
            return i;
        }
    }
    return -1;
}

}}} // namespace ohao::physics::collision
