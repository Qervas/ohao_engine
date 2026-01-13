#include "gjk_solver.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"
#include "physics/collision/shapes/capsule_shape.hpp"
#include "physics/collision/shapes/plane_shape.hpp"
#include <limits>

namespace ohao {
namespace physics {
namespace collision {

GJKSolver::GJKSolver()
    : m_maxIterations(32)
    , m_tolerance(0.001f)
    , m_lastIterations(0)
{
}

GJKSolver::Result GJKSolver::solve(const CollisionShape* shapeA, const glm::mat4& transformA,
                                     const CollisionShape* shapeB, const glm::mat4& transformB)
{
    Result result;
    result.intersecting = false;
    result.distance = std::numeric_limits<float>::max();

    // Initial direction: from center of B to center of A
    glm::vec3 centerA = glm::vec3(transformA[3]);
    glm::vec3 centerB = glm::vec3(transformB[3]);
    glm::vec3 direction = centerA - centerB;

    if (glm::length(direction) < m_tolerance) {
        direction = glm::vec3(1.0f, 0.0f, 0.0f);  // Arbitrary direction
    }

    Simplex simplex;
    glm::vec3 support = supportMinkowski(shapeA, transformA, shapeB, transformB, direction);
    simplex.push(support);

    direction = -support;  // Direction towards origin

    m_lastIterations = 0;

    for (int iter = 0; iter < m_maxIterations; ++iter) {
        m_lastIterations = iter + 1;

        // Safety check: direction must not be zero
        float dirLength = glm::length(direction);
        if (dirLength < 0.0001f) {
            // Direction collapsed - assume separation
            result.intersecting = false;
            result.normal = glm::vec3(1.0f, 0.0f, 0.0f);  // Arbitrary normal
            result.simplex = simplex;
            return result;
        }

        // Normalize direction for numerical stability
        direction = direction / dirLength;

        support = supportMinkowski(shapeA, transformA, shapeB, transformB, direction);

        if (glm::dot(support, direction) < 0.0f) {
            // No intersection - support point didn't pass origin
            result.intersecting = false;
            result.normal = direction;
            result.simplex = simplex;
            return result;
        }

        simplex.push(support);

        if (updateSimplex(simplex, direction)) {
            // Simplex contains origin - shapes are intersecting
            result.intersecting = true;
            result.distance = 0.0f;
            result.simplex = simplex;
            return result;
        }

        // Check convergence
        if (glm::length(direction) < m_tolerance) {
            result.intersecting = true;
            result.distance = 0.0f;
            result.simplex = simplex;
            return result;
        }
    }

    // Max iterations reached - assume separation
    result.intersecting = false;
    float finalLength = glm::length(direction);
    result.normal = finalLength > 0.0001f ? direction / finalLength : glm::vec3(1.0f, 0.0f, 0.0f);
    result.simplex = simplex;
    return result;
}

glm::vec3 GJKSolver::support(const CollisionShape* shape, const glm::mat4& transform, const glm::vec3& direction) {
    // Transform direction to local space
    glm::mat3 rotation = glm::mat3(transform);
    glm::mat3 invRotation = glm::transpose(rotation);
    glm::vec3 localDir = invRotation * direction;

    glm::vec3 localSupport;

    switch (shape->getType()) {
        case ShapeType::BOX: {
            auto* box = static_cast<const BoxShape*>(shape);
            glm::vec3 halfExtents = box->getHalfExtents();

            // Support point is corner in direction
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

            // Support point is center + radius in direction
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

            // Support point is endpoint + radius in direction
            glm::vec3 endpoint = glm::vec3(0.0f, localDir.y > 0.0f ? halfHeight : -halfHeight, 0.0f);
            if (glm::length(localDir) > 0.001f) {
                localSupport = endpoint + glm::normalize(localDir) * radius;
            } else {
                localSupport = endpoint + glm::vec3(radius, 0.0f, 0.0f);
            }
            break;
        }

        case ShapeType::PLANE: {
            // Plane is infinite - use large extent in direction
            if (glm::length(localDir) > 0.001f) {
                localSupport = glm::normalize(localDir) * 10000.0f;
            } else {
                localSupport = glm::vec3(0.0f);
            }
            break;
        }

        default:
            localSupport = glm::vec3(0.0f);
            break;
    }

    // Transform back to world space
    glm::vec3 worldSupport = glm::vec3(transform * glm::vec4(localSupport, 1.0f));
    return worldSupport;
}

glm::vec3 GJKSolver::supportMinkowski(const CollisionShape* shapeA, const glm::mat4& transformA,
                                       const CollisionShape* shapeB, const glm::mat4& transformB,
                                       const glm::vec3& direction)
{
    // Minkowski difference: A - B
    glm::vec3 supportA = support(shapeA, transformA, direction);
    glm::vec3 supportB = support(shapeB, transformB, -direction);
    return supportA - supportB;
}

bool GJKSolver::updateSimplex(Simplex& simplex, glm::vec3& direction) {
    switch (simplex.size()) {
        case 2: return handleLine(simplex, direction);
        case 3: return handleTriangle(simplex, direction);
        case 4: return handleTetrahedron(simplex, direction);
        default: return false;
    }
}

bool GJKSolver::handleLine(Simplex& simplex, glm::vec3& direction) {
    glm::vec3 a = simplex[1];  // Most recent point
    glm::vec3 b = simplex[0];

    glm::vec3 ab = b - a;
    glm::vec3 ao = -a;  // Direction to origin

    if (sameDirection(ab, ao)) {
        // Origin is towards B - keep line
        direction = glm::cross(glm::cross(ab, ao), ab);
        if (glm::length(direction) < 0.001f) {
            // AB and AO are parallel - use perpendicular
            direction = glm::vec3(-ab.y, ab.x, 0.0f);
            if (glm::length(direction) < 0.001f) {
                direction = glm::vec3(-ab.z, 0.0f, ab.x);
            }
        }
    } else {
        // Origin is towards A - use only A
        simplex.set(a, a, a);  // Hack: set to single point
        simplex.clear();
        simplex.push(a);
        direction = ao;
    }

    return false;
}

bool GJKSolver::handleTriangle(Simplex& simplex, glm::vec3& direction) {
    glm::vec3 a = simplex[2];  // Most recent point
    glm::vec3 b = simplex[1];
    glm::vec3 c = simplex[0];

    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ao = -a;

    glm::vec3 abc = glm::cross(ab, ac);  // Triangle normal

    // Test edge AC
    glm::vec3 acPerp = glm::cross(abc, ac);
    if (sameDirection(acPerp, ao)) {
        if (sameDirection(ac, ao)) {
            // Origin is in AC region
            simplex.set(a, c, c);
            simplex.clear();
            simplex.push(c);
            simplex.push(a);
            direction = glm::cross(glm::cross(ac, ao), ac);
            return false;
        } else {
            // Check AB edge
            return handleLineAB(simplex, a, b, ab, ao, direction);
        }
    }

    // Test edge AB
    glm::vec3 abPerp = glm::cross(ab, abc);
    if (sameDirection(abPerp, ao)) {
        return handleLineAB(simplex, a, b, ab, ao, direction);
    }

    // Origin is above or below triangle
    if (sameDirection(abc, ao)) {
        direction = abc;
    } else {
        simplex.set(a, c, b);  // Reverse winding
        direction = -abc;
    }

    return false;
}

bool GJKSolver::handleLineAB(Simplex& simplex, const glm::vec3& a, const glm::vec3& b,
                               const glm::vec3& ab, const glm::vec3& ao, glm::vec3& direction)
{
    if (sameDirection(ab, ao)) {
        simplex.set(a, b, b);
        simplex.clear();
        simplex.push(b);
        simplex.push(a);
        direction = glm::cross(glm::cross(ab, ao), ab);
        return false;
    } else {
        simplex.set(a, a, a);
        simplex.clear();
        simplex.push(a);
        direction = ao;
        return false;
    }
}

bool GJKSolver::handleTetrahedron(Simplex& simplex, glm::vec3& direction) {
    glm::vec3 a = simplex[3];  // Most recent point
    glm::vec3 b = simplex[2];
    glm::vec3 c = simplex[1];
    glm::vec3 d = simplex[0];

    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ad = d - a;
    glm::vec3 ao = -a;

    // Test faces
    glm::vec3 abc = glm::cross(ab, ac);
    glm::vec3 acd = glm::cross(ac, ad);
    glm::vec3 adb = glm::cross(ad, ab);

    // Face ABC
    if (sameDirection(abc, ao)) {
        simplex.set(a, b, c);
        simplex.clear();
        simplex.push(c);
        simplex.push(b);
        simplex.push(a);
        return handleTriangle(simplex, direction);
    }

    // Face ACD
    if (sameDirection(acd, ao)) {
        simplex.set(a, c, d);
        simplex.clear();
        simplex.push(d);
        simplex.push(c);
        simplex.push(a);
        return handleTriangle(simplex, direction);
    }

    // Face ADB
    if (sameDirection(adb, ao)) {
        simplex.set(a, d, b);
        simplex.clear();
        simplex.push(b);
        simplex.push(d);
        simplex.push(a);
        return handleTriangle(simplex, direction);
    }

    // Origin is inside tetrahedron - collision!
    return true;
}

}}} // namespace ohao::physics::collision
