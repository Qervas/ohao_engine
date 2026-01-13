#include "epa_solver.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"
#include "physics/collision/shapes/capsule_shape.hpp"
#include <limits>
#include <algorithm>

namespace ohao {
namespace physics {
namespace collision {

EPAFace::EPAFace(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    vertices[0] = a;
    vertices[1] = b;
    vertices[2] = c;
    computeNormal();
}

void EPAFace::computeNormal() {
    glm::vec3 ab = vertices[1] - vertices[0];
    glm::vec3 ac = vertices[2] - vertices[0];
    normal = glm::normalize(glm::cross(ab, ac));
    distance = glm::dot(normal, vertices[0]);

    // Ensure normal points towards origin
    if (distance < 0.0f) {
        normal = -normal;
        distance = -distance;
    }
}

EPASolver::EPASolver()
    : m_maxIterations(64)
    , m_tolerance(0.0001f)
    , m_lastIterations(0)
{
}

EPASolver::Result EPASolver::solve(const Simplex& simplex,
                                     const CollisionShape* shapeA, const glm::mat4& transformA,
                                     const CollisionShape* shapeB, const glm::mat4& transformB)
{
    Result result;
    result.success = false;
    result.penetrationDepth = 0.0f;

    // Build initial polytope from simplex
    buildInitialPolytope(simplex);

    if (m_faces.empty()) {
        return result;
    }

    m_lastIterations = 0;

    for (int iter = 0; iter < m_maxIterations; ++iter) {
        m_lastIterations = iter + 1;

        // Find closest face
        int closestIdx = findClosestFace();
        if (closestIdx < 0) break;

        EPAFace& closest = m_faces[closestIdx];

        // Get support point in direction of face normal
        glm::vec3 support = supportMinkowski(shapeA, transformA, shapeB, transformB, closest.normal);

        float distance = glm::dot(support, closest.normal);

        // Check if we've expanded as far as possible
        if (distance - closest.distance < m_tolerance) {
            // Converged
            result.success = true;
            result.penetrationDepth = closest.distance;
            result.normal = closest.normal;

            // Approximate contact points (could be refined)
            result.contactPointA = closest.vertices[0];
            result.contactPointB = closest.vertices[0] - closest.normal * closest.distance;

            return result;
        }

        // Expand polytope by adding new faces
        // (Simplified: just replace closest face with three new faces)
        glm::vec3 a = closest.vertices[0];
        glm::vec3 b = closest.vertices[1];
        glm::vec3 c = closest.vertices[2];

        m_faces.erase(m_faces.begin() + closestIdx);

        m_faces.emplace_back(support, a, b);
        m_faces.emplace_back(support, b, c);
        m_faces.emplace_back(support, c, a);
    }

    // Max iterations reached - return best result
    int closestIdx = findClosestFace();
    if (closestIdx >= 0) {
        EPAFace& closest = m_faces[closestIdx];
        result.success = true;
        result.penetrationDepth = closest.distance;
        result.normal = closest.normal;
        result.contactPointA = closest.vertices[0];
        result.contactPointB = closest.vertices[0] - closest.normal * closest.distance;
    }

    return result;
}

void EPASolver::buildInitialPolytope(const Simplex& simplex) {
    m_faces.clear();

    if (simplex.size() < 4) {
        // Need at least a tetrahedron
        return;
    }

    glm::vec3 a = simplex[0];
    glm::vec3 b = simplex[1];
    glm::vec3 c = simplex[2];
    glm::vec3 d = simplex[3];

    // Create 4 faces of tetrahedron
    m_faces.emplace_back(a, b, c);
    m_faces.emplace_back(a, c, d);
    m_faces.emplace_back(a, d, b);
    m_faces.emplace_back(b, d, c);
}

int EPASolver::findClosestFace() const {
    if (m_faces.empty()) return -1;

    int closestIdx = 0;
    float minDistance = m_faces[0].distance;

    for (size_t i = 1; i < m_faces.size(); ++i) {
        if (m_faces[i].distance < minDistance) {
            minDistance = m_faces[i].distance;
            closestIdx = static_cast<int>(i);
        }
    }

    return closestIdx;
}

glm::vec3 EPASolver::support(const CollisionShape* shape, const glm::mat4& transform, const glm::vec3& direction) {
    // Transform direction to local space
    glm::mat3 rotation = glm::mat3(transform);
    glm::mat3 invRotation = glm::transpose(rotation);
    glm::vec3 localDir = invRotation * direction;

    glm::vec3 localSupport;

    switch (shape->getType()) {
        case ShapeType::BOX: {
            auto* box = static_cast<const BoxShape*>(shape);
            glm::vec3 halfExtents = box->getHalfExtents();
            localSupport = glm::vec3(
                localDir.x > 0.0f ? halfExtents.x : -halfExtents.x,
                localDir.y > 0.0f ? halfExtents.y : -halfExtents.y,
                localDir.z > 0.0f ? halfExtents.z : -halfExtents.z
            );
            break;
        }

        case ShapeType::SPHERE: {
            auto* sphere = static_cast<const SphereShape*>(shape);
            float radius = sphere->getRadius();
            if (glm::length(localDir) > 0.001f) {
                localSupport = glm::normalize(localDir) * radius;
            } else {
                localSupport = glm::vec3(radius, 0.0f, 0.0f);
            }
            break;
        }

        case ShapeType::CAPSULE: {
            auto* capsule = static_cast<const CapsuleShape*>(shape);
            float radius = capsule->getRadius();
            float halfHeight = capsule->getHeight() * 0.5f;
            glm::vec3 endpoint = glm::vec3(0.0f, localDir.y > 0.0f ? halfHeight : -halfHeight, 0.0f);
            if (glm::length(localDir) > 0.001f) {
                localSupport = endpoint + glm::normalize(localDir) * radius;
            } else {
                localSupport = endpoint + glm::vec3(radius, 0.0f, 0.0f);
            }
            break;
        }

        default:
            localSupport = glm::vec3(0.0f);
            break;
    }

    return glm::vec3(transform * glm::vec4(localSupport, 1.0f));
}

glm::vec3 EPASolver::supportMinkowski(const CollisionShape* shapeA, const glm::mat4& transformA,
                                       const CollisionShape* shapeB, const glm::mat4& transformB,
                                       const glm::vec3& direction)
{
    glm::vec3 supportA = support(shapeA, transformA, direction);
    glm::vec3 supportB = support(shapeB, transformB, -direction);
    return supportA - supportB;
}

}}} // namespace ohao::physics::collision
