#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "gjk_solver.hpp"

namespace ohao {
namespace physics {
namespace collision {

// Forward declarations
class CollisionShape;

// Triangle face for EPA polytope
struct EPAFace {
    glm::vec3 vertices[3];
    glm::vec3 normal;
    float distance;  // Distance to origin

    EPAFace() : distance(0.0f) {}
    EPAFace(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

    void computeNormal();
};

// EPA (Expanding Polytope Algorithm) for penetration depth
class EPASolver {
public:
    struct Result {
        bool success;
        float penetrationDepth;
        glm::vec3 normal;          // Direction from B to A
        glm::vec3 contactPointA;   // Contact on shape A
        glm::vec3 contactPointB;   // Contact on shape B
    };

    EPASolver();

    // Run EPA algorithm (starting from GJK simplex)
    Result solve(const Simplex& simplex,
                 const CollisionShape* shapeA, const glm::mat4& transformA,
                 const CollisionShape* shapeB, const glm::mat4& transformB);

    // Configuration
    void setMaxIterations(int maxIter) { m_maxIterations = maxIter; }
    void setTolerance(float tol) { m_tolerance = tol; }

    // Stats
    int getLastIterationCount() const { return m_lastIterations; }

private:
    // Build initial polytope from simplex
    void buildInitialPolytope(const Simplex& simplex);

    // Find face closest to origin
    int findClosestFace() const;

    // Expand polytope along face normal
    glm::vec3 supportMinkowski(const CollisionShape* shapeA, const glm::mat4& transformA,
                                const CollisionShape* shapeB, const glm::mat4& transformB,
                                const glm::vec3& direction);

    // Helpers
    glm::vec3 support(const CollisionShape* shape, const glm::mat4& transform, const glm::vec3& direction);

    // Data
    std::vector<EPAFace> m_faces;

    // Configuration
    int m_maxIterations;
    float m_tolerance;

    // Stats
    int m_lastIterations;
};

}}} // namespace ohao::physics::collision
