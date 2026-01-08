#include "picking_system.hpp"
#include "renderer/components/mesh_component.hpp"
#include "engine/component/transform_component.hpp"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <limits>

namespace ohao {

Ray PickingSystem::screenToWorldRay(
    const glm::vec2& screenPos,
    const glm::vec2& viewportSize,
    const Camera& camera
) const {
    // Convert screen position to Normalized Device Coordinates (NDC)
    // NDC range: [-1, 1] for both X and Y
    float ndcX = (2.0f * screenPos.x) / viewportSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * screenPos.y) / viewportSize.y;  // Flip Y for Vulkan

    // Get inverse matrices for unprojection
    glm::mat4 invProj = glm::inverse(camera.getProjectionMatrix());
    glm::mat4 invView = glm::inverse(camera.getViewMatrix());

    // For Vulkan, depth range is [0, 1] not [-1, 1]
    // Near plane in NDC is 0, far plane is 1
    glm::vec4 nearPointNDC = glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPointNDC = glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

    // Unproject to view space
    glm::vec4 nearPointView = invProj * nearPointNDC;
    glm::vec4 farPointView = invProj * farPointNDC;

    // Perspective divide
    nearPointView /= nearPointView.w;
    farPointView /= farPointView.w;

    // Transform to world space
    glm::vec4 nearPointWorld = invView * nearPointView;
    glm::vec4 farPointWorld = invView * farPointView;

    glm::vec3 rayOrigin = glm::vec3(nearPointWorld);
    glm::vec3 rayDirection = glm::normalize(glm::vec3(farPointWorld - nearPointWorld));

    return Ray(rayOrigin, rayDirection);
}

PickResult PickingSystem::pickActor(
    const Ray& ray,
    Scene* scene,
    const std::unordered_set<Actor*>& excludeActors
) const {
    PickResult result;

    if (!scene) return result;

    const auto& allActors = scene->getAllActors();
    for (const auto& [id, actorPtr] : allActors) {
        Actor* actor = actorPtr.get();

        // Skip excluded actors
        if (excludeActors.count(actor)) continue;

        float distance;
        glm::vec3 hitPoint, hitNormal;

        if (testActorIntersection(ray, actor, distance, hitPoint, hitNormal)) {
            if (distance < result.distance && distance > 0.0f) {
                result.actor = actor;
                result.distance = distance;
                result.hitPoint = hitPoint;
                result.hitNormal = hitNormal;
                result.hit = true;
            }
        }
    }

    return result;
}

std::vector<PickResult> PickingSystem::pickAllActors(
    const Ray& ray,
    Scene* scene
) const {
    std::vector<PickResult> results;

    if (!scene) return results;

    const auto& allActors = scene->getAllActors();
    for (const auto& [id, actorPtr] : allActors) {
        Actor* actor = actorPtr.get();

        float distance;
        glm::vec3 hitPoint, hitNormal;

        if (testActorIntersection(ray, actor, distance, hitPoint, hitNormal)) {
            if (distance > 0.0f) {
                PickResult result;
                result.actor = actor;
                result.distance = distance;
                result.hitPoint = hitPoint;
                result.hitNormal = hitNormal;
                result.hit = true;
                results.push_back(result);
            }
        }
    }

    // Sort by distance (closest first)
    std::sort(results.begin(), results.end());
    return results;
}

bool PickingSystem::testActorIntersection(
    const Ray& ray,
    Actor* actor,
    float& outDistance,
    glm::vec3& outHitPoint,
    glm::vec3& outHitNormal
) const {
    if (!actor) return false;

    // Get mesh component
    auto meshComponent = actor->getComponent<MeshComponent>();
    if (!meshComponent || !meshComponent->isVisible()) return false;

    auto model = meshComponent->getModel();
    if (!model || model->vertices.empty()) return false;

    // Get world transform
    auto transform = actor->getTransform();
    if (!transform) return false;

    glm::mat4 worldMatrix = transform->getWorldMatrix();

    // First: Quick AABB rejection test
    AABB worldAABB = calculateWorldAABB(model->vertices, worldMatrix);

    float tMin, tMax;
    if (!rayIntersectsAABB(ray, worldAABB, tMin, tMax)) {
        return false;
    }

    // If not using precise testing, return AABB hit
    if (!usePreciseMeshTesting) {
        outDistance = tMin > 0 ? tMin : tMax;
        outHitPoint = ray.pointAt(outDistance);
        outHitNormal = glm::vec3(0.0f, 1.0f, 0.0f);  // Default normal
        return outDistance > 0.0f;
    }

    // Second: Precise mesh triangle intersection
    float tHit;
    glm::vec3 hitNormal;
    if (rayIntersectsMesh(ray, model->vertices, model->indices, worldMatrix, tHit, hitNormal)) {
        outDistance = tHit;
        outHitPoint = ray.pointAt(tHit);
        outHitNormal = hitNormal;
        return true;
    }

    return false;
}

bool PickingSystem::rayIntersectsAABB(
    const Ray& ray,
    const AABB& aabb,
    float& tMin,
    float& tMax
) const {
    tMin = 0.0f;
    tMax = std::numeric_limits<float>::max();

    for (int i = 0; i < 3; i++) {
        float invD = 1.0f / ray.direction[i];
        float t0 = (aabb.min[i] - ray.origin[i]) * invD;
        float t1 = (aabb.max[i] - ray.origin[i]) * invD;

        if (invD < 0.0f) {
            std::swap(t0, t1);
        }

        tMin = t0 > tMin ? t0 : tMin;
        tMax = t1 < tMax ? t1 : tMax;

        if (tMax < tMin) {
            return false;
        }
    }

    return true;
}

bool PickingSystem::rayIntersectsMesh(
    const Ray& ray,
    const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    const glm::mat4& worldMatrix,
    float& tHit,
    glm::vec3& hitNormal
) const {
    tHit = std::numeric_limits<float>::max();
    bool foundHit = false;

    // Normal matrix for transforming normals
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldMatrix)));

    // Test each triangle
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        // Transform vertices to world space
        glm::vec3 v0 = glm::vec3(worldMatrix * glm::vec4(vertices[indices[i]].position, 1.0f));
        glm::vec3 v1 = glm::vec3(worldMatrix * glm::vec4(vertices[indices[i + 1]].position, 1.0f));
        glm::vec3 v2 = glm::vec3(worldMatrix * glm::vec4(vertices[indices[i + 2]].position, 1.0f));

        float t;
        glm::vec3 bary;
        if (rayIntersectsTriangle(ray, v0, v1, v2, t, bary)) {
            if (t > 0.0f && t < tHit) {
                tHit = t;
                foundHit = true;

                // Interpolate normal using barycentric coordinates
                glm::vec3 n0 = vertices[indices[i]].normal;
                glm::vec3 n1 = vertices[indices[i + 1]].normal;
                glm::vec3 n2 = vertices[indices[i + 2]].normal;
                glm::vec3 localNormal = n0 * bary.x + n1 * bary.y + n2 * bary.z;
                hitNormal = glm::normalize(normalMatrix * localNormal);
            }
        }
    }

    return foundHit;
}

bool PickingSystem::rayIntersectsTriangle(
    const Ray& ray,
    const glm::vec3& v0,
    const glm::vec3& v1,
    const glm::vec3& v2,
    float& t,
    glm::vec3& barycentricCoords
) const {
    // Moller-Trumbore algorithm
    const float EPSILON = 1e-6f;

    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(ray.direction, edge2);
    float a = glm::dot(edge1, h);

    // Ray parallel to triangle
    if (a > -EPSILON && a < EPSILON) {
        return false;
    }

    float f = 1.0f / a;
    glm::vec3 s = ray.origin - v0;
    float u = f * glm::dot(s, h);

    if (u < 0.0f || u > 1.0f) {
        return false;
    }

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(ray.direction, q);

    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }

    // Calculate t to find intersection point
    t = f * glm::dot(edge2, q);

    if (t > EPSILON) {
        // Barycentric coordinates
        barycentricCoords.x = 1.0f - u - v;  // weight for v0
        barycentricCoords.y = u;              // weight for v1
        barycentricCoords.z = v;              // weight for v2
        return true;
    }

    return false;
}

AABB PickingSystem::calculateWorldAABB(
    const std::vector<Vertex>& vertices,
    const glm::mat4& worldMatrix
) const {
    AABB aabb;

    for (const auto& vertex : vertices) {
        glm::vec3 worldPos = glm::vec3(worldMatrix * glm::vec4(vertex.position, 1.0f));
        aabb.expand(worldPos);
    }

    return aabb;
}

} // namespace ohao
