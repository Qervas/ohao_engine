#pragma once

#include "collision_shape.hpp"
#include <vector>

namespace ohao {
namespace physics {
namespace collision {

struct Triangle {
    glm::vec3 v0, v1, v2;  // Vertices
    glm::vec3 normal;      // Face normal
    
    Triangle() = default;
    Triangle(const glm::vec3& vertex0, const glm::vec3& vertex1, const glm::vec3& vertex2) 
        : v0(vertex0), v1(vertex1), v2(vertex2) {
        calculateNormal();
    }
    
    void calculateNormal() {
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        normal = math::safeNormalize(glm::cross(edge1, edge2));
    }
    
    glm::vec3 getCenter() const {
        return (v0 + v1 + v2) / 3.0f;
    }
    
    float getArea() const {
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        return 0.5f * glm::length(glm::cross(edge1, edge2));
    }
};

class TriangleMeshShape : public CollisionShape {
public:
    TriangleMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices)
        : CollisionShape(ShapeType::TRIANGLE_MESH), m_vertices(vertices), m_indices(indices) {
        buildTriangles();
        calculateBounds();
    }
    
    // Alternative constructor taking triangles directly
    TriangleMeshShape(const std::vector<Triangle>& triangles)
        : CollisionShape(ShapeType::TRIANGLE_MESH), m_triangles(triangles) {
        calculateBounds();
        
        // Build vertices and indices from triangles
        m_vertices.reserve(triangles.size() * 3);
        m_indices.reserve(triangles.size() * 3);
        
        for (size_t i = 0; i < triangles.size(); ++i) {
            const Triangle& tri = triangles[i];
            
            uint32_t baseIndex = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(tri.v0);
            m_vertices.push_back(tri.v1);
            m_vertices.push_back(tri.v2);
            
            m_indices.push_back(baseIndex);
            m_indices.push_back(baseIndex + 1);
            m_indices.push_back(baseIndex + 2);
        }
    }
    
    // Getters
    const std::vector<glm::vec3>& getVertices() const { return m_vertices; }
    const std::vector<uint32_t>& getIndices() const { return m_indices; }
    const std::vector<Triangle>& getTriangles() const { return m_triangles; }
    size_t getTriangleCount() const { return m_triangles.size(); }
    
    // CollisionShape interface implementation
    math::AABB getAABB(const glm::vec3& worldPosition, const glm::quat& worldRotation) const override {
        if (m_vertices.empty()) {
            return math::AABB(worldPosition, worldPosition);
        }
        
        // Transform all vertices and find bounds
        glm::mat4 transform = getWorldTransform(worldPosition, worldRotation);
        
        glm::vec3 minBounds(FLT_MAX);
        glm::vec3 maxBounds(-FLT_MAX);
        
        for (const glm::vec3& vertex : m_vertices) {
            glm::vec3 transformedVertex = math::transformPoint(vertex, transform);
            minBounds = glm::min(minBounds, transformedVertex);
            maxBounds = glm::max(maxBounds, transformedVertex);
        }
        
        return math::AABB(minBounds, maxBounds);
    }
    
    bool containsPoint(const glm::vec3& worldPoint, const glm::vec3& shapePosition, 
                      const glm::quat& shapeRotation) const override {
        // For triangle meshes, we typically consider a point "inside" if it's close to the surface
        // This is more complex than other shapes and might use ray casting or closest point queries
        
        // Transform point to local space
        glm::mat4 worldToLocal = glm::inverse(getWorldTransform(shapePosition, shapeRotation));
        glm::vec3 localPoint = math::transformPoint(worldPoint, worldToLocal);
        
        // Find the closest triangle and check distance
        float minDistanceSquared = FLT_MAX;
        
        for (const Triangle& triangle : m_triangles) {
            glm::vec3 closestPoint = getClosestPointOnTriangle(localPoint, triangle);
            float distanceSquared = glm::length2(localPoint - closestPoint);
            minDistanceSquared = glm::min(minDistanceSquared, distanceSquared);
        }
        
        // Consider the point "inside" if it's very close to the surface
        const float threshold = 0.001f; // 1mm threshold
        return minDistanceSquared <= (threshold * threshold);
    }
    
    glm::vec3 getSize() const override {
        return m_bounds.max - m_bounds.min;
    }
    
    float getVolume() const override {
        // For a triangle mesh, calculating volume requires it to be a closed manifold
        // For now, return 0 (surface has no volume)
        return 0.0f;
    }
    
    // Utility functions for collision detection
    glm::vec3 getClosestPointOnSurface(const glm::vec3& point, const glm::vec3& position, 
                                      const glm::quat& rotation) const {
        glm::mat4 worldToLocal = glm::inverse(getWorldTransform(position, rotation));
        glm::mat4 localToWorld = getWorldTransform(position, rotation);
        
        glm::vec3 localPoint = math::transformPoint(point, worldToLocal);
        
        glm::vec3 closestPoint = localPoint;
        float minDistanceSquared = FLT_MAX;
        
        for (const Triangle& triangle : m_triangles) {
            glm::vec3 triangleClosest = getClosestPointOnTriangle(localPoint, triangle);
            float distanceSquared = glm::length2(localPoint - triangleClosest);
            
            if (distanceSquared < minDistanceSquared) {
                minDistanceSquared = distanceSquared;
                closestPoint = triangleClosest;
            }
        }
        
        return math::transformPoint(closestPoint, localToWorld);
    }
    
    // Ray-mesh intersection (excellent for ray tracing)
    struct RayIntersection {
        bool hasIntersection = false;
        float t = FLT_MAX;
        glm::vec3 point = glm::vec3(0.0f);
        glm::vec3 normal = glm::vec3(0.0f);
        size_t triangleIndex = SIZE_MAX;
        glm::vec2 barycentric = glm::vec2(0.0f);  // u, v coordinates
    };
    
    RayIntersection intersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                               const glm::vec3& position, const glm::quat& rotation,
                               bool backfaceCulling = false) const {
        RayIntersection closestHit;
        
        // Transform ray to local space
        glm::mat4 worldToLocal = glm::inverse(getWorldTransform(position, rotation));
        glm::mat4 localToWorld = getWorldTransform(position, rotation);
        
        glm::vec3 localOrigin = math::transformPoint(rayOrigin, worldToLocal);
        glm::vec3 localDirection = math::transformVector(rayDirection, worldToLocal);
        localDirection = glm::normalize(localDirection);
        
        for (size_t i = 0; i < m_triangles.size(); ++i) {
            const Triangle& triangle = m_triangles[i];
            
            auto intersection = rayTriangleIntersect(localOrigin, localDirection, triangle, backfaceCulling);
            
            if (intersection.hasIntersection && intersection.t < closestHit.t) {
                closestHit = intersection;
                closestHit.triangleIndex = i;
                // Transform results back to world space
                closestHit.point = math::transformPoint(intersection.point, localToWorld);
                closestHit.normal = math::transformVector(intersection.normal, localToWorld);
                closestHit.normal = glm::normalize(closestHit.normal);
            }
        }
        
        return closestHit;
    }
    
    // Get triangles within a bounding box (useful for broad-phase collision detection)
    std::vector<size_t> getTrianglesInBounds(const math::AABB& bounds) const {
        std::vector<size_t> result;
        
        for (size_t i = 0; i < m_triangles.size(); ++i) {
            const Triangle& triangle = m_triangles[i];
            
            // Check if triangle overlaps with bounds
            math::AABB triangleBounds = getTriangleBounds(triangle);
            if (triangleBounds.intersects(bounds)) {
                result.push_back(i);
            }
        }
        
        return result;
    }
    
private:
    std::vector<glm::vec3> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<Triangle> m_triangles;
    math::AABB m_bounds;
    
    void buildTriangles() {
        m_triangles.clear();
        
        if (m_indices.size() % 3 != 0) {
            // Invalid index count
            return;
        }
        
        for (size_t i = 0; i < m_indices.size(); i += 3) {
            if (m_indices[i] < m_vertices.size() && 
                m_indices[i + 1] < m_vertices.size() && 
                m_indices[i + 2] < m_vertices.size()) {
                
                Triangle triangle(
                    m_vertices[m_indices[i]],
                    m_vertices[m_indices[i + 1]],
                    m_vertices[m_indices[i + 2]]
                );
                
                m_triangles.push_back(triangle);
            }
        }
    }
    
    void calculateBounds() {
        if (m_vertices.empty()) {
            m_bounds = math::AABB(glm::vec3(0.0f), glm::vec3(0.0f));
            return;
        }
        
        glm::vec3 minBounds = m_vertices[0];
        glm::vec3 maxBounds = m_vertices[0];
        
        for (const glm::vec3& vertex : m_vertices) {
            minBounds = glm::min(minBounds, vertex);
            maxBounds = glm::max(maxBounds, vertex);
        }
        
        m_bounds = math::AABB(minBounds, maxBounds);
    }
    
    glm::vec3 getClosestPointOnTriangle(const glm::vec3& point, const Triangle& triangle) const {
        // Barycentric coordinate method for closest point on triangle
        glm::vec3 edge0 = triangle.v1 - triangle.v0;
        glm::vec3 edge1 = triangle.v2 - triangle.v0;
        glm::vec3 v0ToPoint = point - triangle.v0;
        
        float a = glm::dot(edge0, edge0);
        float b = glm::dot(edge0, edge1);
        float c = glm::dot(edge1, edge1);
        float d = glm::dot(edge0, v0ToPoint);
        float e = glm::dot(edge1, v0ToPoint);
        
        float det = a * c - b * b;
        float s = b * e - c * d;
        float t = b * d - a * e;
        
        if (s + t < det) {
            if (s < 0.0f) {
                if (t < 0.0f) {
                    // Region 4
                    s = glm::clamp(-d / a, 0.0f, 1.0f);
                    t = 0.0f;
                } else {
                    // Region 3
                    s = 0.0f;
                    t = glm::clamp(-e / c, 0.0f, 1.0f);
                }
            } else if (t < 0.0f) {
                // Region 5
                t = 0.0f;
                s = glm::clamp(-d / a, 0.0f, 1.0f);
            } else {
                // Region 0
                float invDet = 1.0f / det;
                s *= invDet;
                t *= invDet;
            }
        } else {
            if (s < 0.0f) {
                // Region 2
                float tmp0 = b + d;
                float tmp1 = c + e;
                if (tmp1 > tmp0) {
                    float numer = tmp1 - tmp0;
                    float denom = a - 2 * b + c;
                    s = glm::clamp(numer / denom, 0.0f, 1.0f);
                    t = 1.0f - s;
                } else {
                    t = glm::clamp(-e / c, 0.0f, 1.0f);
                    s = 0.0f;
                }
            } else if (t < 0.0f) {
                // Region 6
                float tmp0 = b + e;
                float tmp1 = a + d;
                if (tmp1 > tmp0) {
                    float numer = tmp1 - tmp0;
                    float denom = a - 2 * b + c;
                    t = glm::clamp(numer / denom, 0.0f, 1.0f);
                    s = 1.0f - t;
                } else {
                    s = glm::clamp(-d / a, 0.0f, 1.0f);
                    t = 0.0f;
                }
            } else {
                // Region 1
                float numer = c + e - b - d;
                if (numer <= 0.0f) {
                    s = 0.0f;
                } else {
                    float denom = a - 2 * b + c;
                    s = glm::clamp(numer / denom, 0.0f, 1.0f);
                }
                t = 1.0f - s;
            }
        }
        
        return triangle.v0 + edge0 * s + edge1 * t;
    }
    
    RayIntersection rayTriangleIntersect(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                       const Triangle& triangle, bool backfaceCulling) const {
        RayIntersection result;
        
        // Möller-Trumbore intersection algorithm
        glm::vec3 edge1 = triangle.v1 - triangle.v0;
        glm::vec3 edge2 = triangle.v2 - triangle.v0;
        
        glm::vec3 h = glm::cross(rayDirection, edge2);
        float a = glm::dot(edge1, h);
        
        if (backfaceCulling && a < math::constants::EPSILON) {
            return result; // Ray is parallel or back-facing
        }
        
        if (glm::abs(a) < math::constants::EPSILON) {
            return result; // Ray is parallel to triangle
        }
        
        float f = 1.0f / a;
        glm::vec3 s = rayOrigin - triangle.v0;
        float u = f * glm::dot(s, h);
        
        if (u < 0.0f || u > 1.0f) {
            return result;
        }
        
        glm::vec3 q = glm::cross(s, edge1);
        float v = f * glm::dot(rayDirection, q);
        
        if (v < 0.0f || u + v > 1.0f) {
            return result;
        }
        
        float t = f * glm::dot(edge2, q);
        
        if (t > math::constants::EPSILON) {
            result.hasIntersection = true;
            result.t = t;
            result.point = rayOrigin + rayDirection * t;
            result.normal = triangle.normal;
            result.barycentric = glm::vec2(u, v);
        }
        
        return result;
    }
    
    math::AABB getTriangleBounds(const Triangle& triangle) const {
        glm::vec3 minBounds = glm::min(glm::min(triangle.v0, triangle.v1), triangle.v2);
        glm::vec3 maxBounds = glm::max(glm::max(triangle.v0, triangle.v1), triangle.v2);
        return math::AABB(minBounds, maxBounds);
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao