#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace ohao {
namespace physics {
namespace collision {

// Forward declarations
class CollisionShape;

// Simplex for GJK algorithm (point, line, triangle, or tetrahedron)
class Simplex {
public:
    Simplex() : m_size(0) {}

    void push(const glm::vec3& point) {
        assert(m_size < 4);
        m_points[m_size++] = point;
    }

    void set(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        m_points[0] = a;
        m_points[1] = b;
        m_points[2] = c;
        m_size = 3;
    }

    void set(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d) {
        m_points[0] = a;
        m_points[1] = b;
        m_points[2] = c;
        m_points[3] = d;
        m_size = 4;
    }

    glm::vec3& operator[](size_t index) {
        assert(index < m_size);
        return m_points[index];
    }

    const glm::vec3& operator[](size_t index) const {
        assert(index < m_size);
        return m_points[index];
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    void clear() { m_size = 0; }

private:
    glm::vec3 m_points[4];
    size_t m_size;
};

// GJK (Gilbert-Johnson-Keerthi) Distance Algorithm
class GJKSolver {
public:
    struct Result {
        bool intersecting;      // True if shapes overlap
        float distance;         // Closest distance (0 if intersecting)
        glm::vec3 closestA;     // Closest point on shape A
        glm::vec3 closestB;     // Closest point on shape B
        glm::vec3 normal;       // Normal direction (from B to A)
        Simplex simplex;        // Final simplex (for EPA)
    };

    GJKSolver();

    // Run GJK algorithm
    Result solve(const CollisionShape* shapeA, const glm::mat4& transformA,
                 const CollisionShape* shapeB, const glm::mat4& transformB);

    // Configuration
    void setMaxIterations(int maxIter) { m_maxIterations = maxIter; }
    void setTolerance(float tol) { m_tolerance = tol; }

    // Stats
    int getLastIterationCount() const { return m_lastIterations; }

private:
    // Support function: furthest point in direction
    glm::vec3 support(const CollisionShape* shape, const glm::mat4& transform, const glm::vec3& direction);

    // Minkowski difference support
    glm::vec3 supportMinkowski(const CollisionShape* shapeA, const glm::mat4& transformA,
                                const CollisionShape* shapeB, const glm::mat4& transformB,
                                const glm::vec3& direction);

    // Simplex evolution
    bool updateSimplex(Simplex& simplex, glm::vec3& direction);
    bool handleLine(Simplex& simplex, glm::vec3& direction);
    bool handleLineAB(Simplex& simplex, const glm::vec3& a, const glm::vec3& b,
                      const glm::vec3& ab, const glm::vec3& ao, glm::vec3& direction);
    bool handleTriangle(Simplex& simplex, glm::vec3& direction);
    bool handleTetrahedron(Simplex& simplex, glm::vec3& direction);

    // Closest point helpers
    bool sameDirection(const glm::vec3& a, const glm::vec3& b) const {
        return glm::dot(a, b) > 0.0f;
    }

    // Configuration
    int m_maxIterations;
    float m_tolerance;

    // Stats
    int m_lastIterations;
};

}}} // namespace ohao::physics::collision
