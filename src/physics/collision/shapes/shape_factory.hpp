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
        // TODO: Implement icosphere generation for smooth sphere approximation
        // For now, return a simple octahedron
        std::vector<glm::vec3> vertices = {
            glm::vec3( 0,  radius,  0),  // Top
            glm::vec3( 0, -radius,  0),  // Bottom
            glm::vec3( radius,  0,  0),  // Right
            glm::vec3(-radius,  0,  0),  // Left
            glm::vec3( 0,  0,  radius),  // Front
            glm::vec3( 0,  0, -radius)   // Back
        };
        
        std::vector<uint32_t> indices = {
            0, 2, 4,  0, 4, 3,  0, 3, 5,  0, 5, 2,  // Top faces
            1, 4, 2,  1, 3, 4,  1, 5, 3,  1, 2, 5   // Bottom faces
        };
        
        return createTriangleMesh(vertices, indices);
    }
};

} // namespace collision
} // namespace physics
} // namespace ohao