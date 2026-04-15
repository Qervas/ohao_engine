#include "scene_framer.hpp"
#include "camera.hpp"
#include "scene/asset/model.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/light_component.hpp"

#include <algorithm>
#include <cmath>

namespace ohao {

FrameResult SceneFramer::computeFraming(const std::vector<Vertex>& vertices) {
    FrameResult result;

    // Compute AABB
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v.position);
        bmax = glm::max(bmax, v.position);
    }
    glm::vec3 extent = bmax - bmin;
    glm::vec3 center = (bmin + bmax) * 0.5f;

    // Detect up axis: most models are Y-up, some are Z-up
    bool isYUp = (extent.y >= extent.z);
    float modelHeight = isYUp ? extent.y : extent.z;


    // Fixed comfortable room size — model is scaled to fit
    float S = 15.0f;
    result.roomHalfSize = S;

    // Scale to fit: use largest extent so crouching/wide models don't overflow
    float maxExtent = std::max({extent.x, extent.y, extent.z});
    result.modelScale = (S * 1.2f) / std::max(maxExtent, 0.001f);

    // Rotation: face camera, handle Z-up conversion
    if (isYUp) {
        result.modelRotation = glm::quat(glm::radians(glm::vec3(0, 180, 0)));
    } else {
        // Z-up: rotate -90° around X to convert +Z→+Y (head up)
        // Then check if feet are at min or max Z to determine direction
        float zMid = (bmin.z + bmax.z) * 0.5f;
        // Most Z-up models: +Z is up. Rotate -90° X: +Z→+Y
        // Some inverted models: -Z is up. Rotate +90° X: -Z→+Y
        bool posZIsUp = (bmax.z - zMid) >= (zMid - bmin.z);
        result.modelRotation = glm::quat(glm::radians(glm::vec3(posZIsUp ? -90.0f : 90.0f, 0, 0)));
    }

    // Position: feet on the floor (Y = -S)
    float scale = result.modelScale;
    if (isYUp) {
        float feetOffset = -bmin.y * scale;
        result.modelPosition = {-center.x * scale, -S + feetOffset, -center.z * scale};
    } else {
        // Z-up: after +90° X rotation, old Z becomes Y. Feet at bmin.z → -bmin.z offset.
        float feetOffset = -bmin.z * scale;
        result.modelPosition = {-center.x * scale, -S + feetOffset, center.y * scale};
    }

    // Camera: far enough to see the full model
    result.cameraPosition = {0, 0, S * 1.7f};
    result.cameraFov = 45.0f;

    // 4-light studio setup (tuned for S=15 room)
    result.lights = {
        // Key light — warm, strong, upper right
        {"KeyLight",  {5.0f, 8.0f, 10.0f}, {1.0f, 0.95f, 0.9f}, 200.0f, 2.0f},
        // Fill light — cool, softer, left side
        {"FillLight", {-5.0f, 6.0f, 8.0f}, {0.7f, 0.8f, 1.0f}, 80.0f, 2.0f},
        // Rim light — warm back light for depth
        {"RimLight",  {0.0f, 5.0f, -8.0f}, {1.0f, 0.85f, 0.7f}, 60.0f, 1.5f},
        // Front fill — soft face light
        {"FrontFill", {0.0f, 3.0f, 15.0f}, {1.0f, 0.95f, 0.9f}, 40.0f, 3.0f},
    };

    return result;
}

void SceneFramer::applyLights(Scene* scene, const FrameResult& result) {
    for (const auto& light : result.lights) {
        auto actor = scene->createActor(light.name);
        auto lc = actor->addComponent<LightComponent>();
        lc->setLightType(LightType::Sphere);
        lc->setColor(light.color);
        lc->setIntensity(light.intensity);
        lc->setRadius(light.radius);
        actor->getTransform()->setPosition(light.position);
    }
}

void SceneFramer::applyCamera(Camera& camera, const FrameResult& result) {
    camera.setPosition(result.cameraPosition);
    camera.setFov(result.cameraFov);
    camera.setRotation(0.0f, -90.0f);
}

} // namespace ohao
