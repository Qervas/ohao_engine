#pragma once

#include "box_shape.hpp"
#include "sphere_shape.hpp"
#include "capsule_shape.hpp"
#include "cylinder_shape.hpp"
#include "plane_shape.hpp"
#include "triangle_mesh_shape.hpp"
#include <memory>

namespace ohao {
namespace physics {
namespace collision {

// Factory class for creating collision shapes
class ShapeFactory {
public:
    // Box shapes
    static std::shared_ptr<BoxShape> createBox(const glm::vec3& halfExtents) {
        return std::make_shared<BoxShape>(halfExtents);
    }
    
    static std::shared_ptr<BoxShape> createBox(float width, float height, float depth) {
        return createBox(glm::vec3(width * 0.5f, height * 0.5f, depth * 0.5f));
    }
    
    static std::shared_ptr<BoxShape> createCube(float size) {
        float halfSize = size * 0.5f;
        return createBox(glm::vec3(halfSize));
    }
    
    // Sphere shapes
    static std::shared_ptr<SphereShape> createSphere(float radius) {
        return std::make_shared<SphereShape>(radius);
    }
    
    // Capsule shapes
    static std::shared_ptr<CapsuleShape> createCapsule(float radius, float height) {
        return std::make_shared<CapsuleShape>(radius, height);
    }
    
    // Cylinder shapes
    static std::shared_ptr<CylinderShape> createCylinder(float radius, float height) {
        return std::make_shared<CylinderShape>(radius, height);
    }
    
    // Plane shapes
    static std::shared_ptr<PlaneShape> createPlane(const glm::vec3& normal, float distance = 0.0f) {
        return std::make_shared<PlaneShape>(normal, distance);
    }
    
    static std::shared_ptr<PlaneShape> createPlane(const glm::vec3& normal, const glm::vec3& pointOnPlane) {
        return std::make_shared<PlaneShape>(normal, pointOnPlane);
    }
    
    static std::shared_ptr<PlaneShape> createGroundPlane(float yPosition = 0.0f) {
        return std::make_shared<PlaneShape>(glm::vec3(0, 1, 0), yPosition);
    }
    
    // Triangle mesh shapes
    static std::shared_ptr<TriangleMeshShape> createTriangleMesh(
        const std::vector<glm::vec3>& vertices, 
        const std::vector<uint32_t>& indices) {
        return std::make_shared<TriangleMeshShape>(vertices, indices);
    }
    
    static std::shared_ptr<TriangleMeshShape> createTriangleMesh(const std::vector<Triangle>& triangles) {
        return std::make_shared<TriangleMeshShape>(triangles);
    }
    
    // Common presets
    static std::shared_ptr<BoxShape> createUnitBox() {
        return createBox(glm::vec3(0.5f)); // 1x1x1 box
    }
    
    static std::shared_ptr<SphereShape> createUnitSphere() {
        return createSphere(0.5f); // Radius 0.5 sphere
    }
    
    static std::shared_ptr<CapsuleShape> createUnitCapsule() {
        return createCapsule(0.5f, 2.0f); // Radius 0.5, height 2.0
    }
    
    static std::shared_ptr<CylinderShape> createUnitCylinder() {
        return createCylinder(0.5f, 1.0f); // Radius 0.5, height 1.0
    }
    
    // Deprecated: Use createGroundPlane() instead
    static std::shared_ptr<BoxShape> createGroundPlane(float width = 100.0f, float depth = 100.0f, float thickness = 1.0f) {
        return createBox(width, thickness, depth);
    }
    
    // Ray tracing friendly shapes
    static std::shared_ptr<TriangleMeshShape> createQuad(float width = 1.0f, float height = 1.0f) {
        std::vector<glm::vec3> vertices = {
            glm::vec3(-width * 0.5f, 0, -height * 0.5f),  // Bottom left
            glm::vec3( width * 0.5f, 0, -height * 0.5f),  // Bottom right
            glm::vec3( width * 0.5f, 0,  height * 0.5f),  // Top right
            glm::vec3(-width * 0.5f, 0,  height * 0.5f)   // Top left
        };
        
        std::vector<uint32_t> indices = {
            0, 1, 2,  // First triangle
            2, 3, 0   // Second triangle
        };
        
        return createTriangleMesh(vertices, indices);
    }
    
    static std::shared_ptr<TriangleMeshShape> createIcosphere(float radius = 1.0f, int subdivisions = 2) {
        // Implement icosphere generation for smooth sphere approximation
        
        // Golden ratio constant
        const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;
        const float a = 1.0f;
        const float b = 1.0f / phi;
        
        // Create initial icosahedron vertices (12 vertices)
        std::vector<glm::vec3> vertices = {
            // Top cap
            glm::vec3( 0,  b, -a), glm::vec3( b,  a,  0), glm::vec3(-b,  a,  0),
            glm::vec3( 0,  b,  a), glm::vec3( 0, -b,  a), glm::vec3(-a,  0,  b),
            glm::vec3( 0, -b, -a), glm::vec3( a,  0, -b), glm::vec3( a,  0,  b),
            glm::vec3(-a,  0, -b), glm::vec3( b, -a,  0), glm::vec3(-b, -a,  0)
        };
        
        // Normalize to unit sphere
        for (auto& vertex : vertices) {
            vertex = glm::normalize(vertex) * radius;
        }
        
        // Create initial icosahedron faces (20 triangles)
        std::vector<uint32_t> indices = {
            // Top cap
            2, 1, 0,   1, 2, 3,   5, 4, 3,   4, 8, 3,   7, 6, 0,   6, 9, 0,
            11, 10, 4, 10, 11, 6, 9, 5, 2,   5, 9, 11,  8, 7, 1,   7, 8, 10,
            // Middle
            2, 5, 3,   8, 1, 3,   9, 2, 0,   1, 7, 0,   11, 9, 6,  7, 10, 6,
            5, 11, 4,  10, 8, 4
        };
        
        // Subdivide the mesh for smoother approximation
        for (int sub = 0; sub < subdivisions; ++sub) {
            std::vector<uint32_t> newIndices;
            std::unordered_map<uint64_t, uint32_t> edgeMap;
            
            // Process each triangle
            for (size_t i = 0; i < indices.size(); i += 3) {
                uint32_t v0 = indices[i];
                uint32_t v1 = indices[i + 1];
                uint32_t v2 = indices[i + 2];
                
                // Get or create midpoint vertices
                uint32_t a = getOrCreateMidpoint(v0, v1, vertices, edgeMap, radius);
                uint32_t b = getOrCreateMidpoint(v1, v2, vertices, edgeMap, radius);
                uint32_t c = getOrCreateMidpoint(v2, v0, vertices, edgeMap, radius);
                
                // Create 4 new triangles
                newIndices.insert(newIndices.end(), {v0, a, c});
                newIndices.insert(newIndices.end(), {v1, b, a});
                newIndices.insert(newIndices.end(), {v2, c, b});
                newIndices.insert(newIndices.end(), {a, b, c});
            }
            
            indices = std::move(newIndices);
        }
        
        return createTriangleMesh(vertices, indices);
    }

private:
    // Helper function for icosphere subdivision
    static uint32_t getOrCreateMidpoint(uint32_t i1, uint32_t i2, 
                                       std::vector<glm::vec3>& vertices,
                                       std::unordered_map<uint64_t, uint32_t>& edgeMap,
                                       float radius) {
        // Create edge key (ensure consistent ordering)
        uint64_t key = (static_cast<uint64_t>(std::min(i1, i2)) << 32) | std::max(i1, i2);
        
        auto it = edgeMap.find(key);
        if (it != edgeMap.end()) {
            return it->second;
        }
        
        // Create new midpoint vertex
        glm::vec3 midpoint = (vertices[i1] + vertices[i2]) * 0.5f;
        midpoint = glm::normalize(midpoint) * radius;
        
        uint32_t index = static_cast<uint32_t>(vertices.size());
        vertices.push_back(midpoint);
        edgeMap[key] = index;
        
        return index;
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao