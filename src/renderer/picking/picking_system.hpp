#pragma once
#include "ray.hpp"
#include "renderer/camera/camera.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/asset/model.hpp"
#include <vector>
#include <unordered_set>

namespace ohao {

class PickingSystem {
public:
    PickingSystem() = default;
    ~PickingSystem() = default;

    // Convert screen coordinates to world-space ray
    // screenPos: pixel coordinates relative to viewport (0,0 = top-left)
    // viewportSize: viewport dimensions in pixels
    Ray screenToWorldRay(
        const glm::vec2& screenPos,
        const glm::vec2& viewportSize,
        const Camera& camera
    ) const;

    // Pick the closest actor in the scene
    PickResult pickActor(
        const Ray& ray,
        Scene* scene,
        const std::unordered_set<Actor*>& excludeActors = {}
    ) const;

    // Pick all actors along the ray (useful for selection through objects)
    std::vector<PickResult> pickAllActors(
        const Ray& ray,
        Scene* scene
    ) const;

    // Test ray against a specific actor
    bool testActorIntersection(
        const Ray& ray,
        Actor* actor,
        float& outDistance,
        glm::vec3& outHitPoint,
        glm::vec3& outHitNormal
    ) const;

    // Configuration
    void setUsePreciseMeshTesting(bool precise) { usePreciseMeshTesting = precise; }
    bool getUsePreciseMeshTesting() const { return usePreciseMeshTesting; }

private:
    // Ray vs AABB intersection (fast bounding box test)
    bool rayIntersectsAABB(
        const Ray& ray,
        const AABB& aabb,
        float& tMin,
        float& tMax
    ) const;

    // Ray vs mesh triangles (precise test)
    bool rayIntersectsMesh(
        const Ray& ray,
        const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        const glm::mat4& worldMatrix,
        float& tHit,
        glm::vec3& hitNormal
    ) const;

    // Moller-Trumbore ray-triangle intersection
    bool rayIntersectsTriangle(
        const Ray& ray,
        const glm::vec3& v0,
        const glm::vec3& v1,
        const glm::vec3& v2,
        float& t,
        glm::vec3& barycentricCoords
    ) const;

    // Calculate AABB for mesh in world space
    AABB calculateWorldAABB(
        const std::vector<Vertex>& vertices,
        const glm::mat4& worldMatrix
    ) const;

    // Configuration
    bool usePreciseMeshTesting = true;  // If false, only uses AABB
};

} // namespace ohao
