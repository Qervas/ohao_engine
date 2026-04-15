#include "scene_framer.hpp"
#include "camera.hpp"
#include "scene/asset/model.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/light_component.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ohao {

FrameResult SceneFramer::computeFraming(const std::vector<Vertex>& vertices, bool forceYUp) {
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
    // GLB/glTF spec mandates Y-up, so forceYUp overrides the extent check
    bool isYUp = forceYUp || (extent.y >= extent.z);
    // modelHeight = the extent along the up axis (used for camera centering)
    // For forced Y-up with atypical models, still use the actual Y extent
    float modelHeight = isYUp ? extent.y : extent.z;



    // Fixed comfortable room size — model is scaled to fit
    float S = 15.0f;
    result.roomHalfSize = S;

    // Scale to fit: use largest extent so crouching/wide models don't overflow
    float maxExtent = std::max({extent.x, extent.y, extent.z});
    result.modelScale = (S * 1.2f) / std::max(maxExtent, 0.001f);

    // Rotation + position: handle Y-up vs Z-up
    float scale = result.modelScale;
    bool posZIsUp = true;  // only used for Z-up models

    if (isYUp) {
        result.modelRotation = glm::quat(glm::radians(glm::vec3(0, 180, 0)));
        float feetOffset = -bmin.y * scale;
        result.modelPosition = {-center.x * scale, -S + feetOffset, -center.z * scale};
    } else {
        // Z-up: detect which Z direction is "up".
        // If Z range is entirely ≤ 0, the model uses -Z as up (head at negative Z).
        // If Z range spans positive values, +Z is up (standard).
        posZIsUp = (bmax.z > 0.01f);
        result.modelRotation = glm::quat(glm::radians(glm::vec3(posZIsUp ? -90.0f : 90.0f, 0, 0)));

        if (posZIsUp) {
            // +Z up: -90° X maps Z→Y. Feet at bmin.z.
            float feetOffset = -bmin.z * scale;
            result.modelPosition = {-center.x * scale, -S + feetOffset, center.y * scale};
        } else {
            // -Z up: +90° X maps -Z→+Y. Feet at bmax.z.
            float feetOffset = bmax.z * scale;
            result.modelPosition = {-center.x * scale, -S + feetOffset, -center.y * scale};
        }
    }

    // Camera: centered on model vertically (toe to head)
    // For Z-up models, after rotation the height in Y = modelHeight (the Z extent)
    float scaledHeight = modelHeight * scale;
    float modelCenterY = -S + scaledHeight * 0.5f;
    result.cameraPosition = {0, modelCenterY, S * 1.7f};
    result.cameraFov = 45.0f;

    // 4-light studio setup (tuned for S=15 room)
    result.lights = {
        // Ceiling light — warm overhead, simulates bedroom ceiling fixture
        {"CeilingLight", {0.0f, S * 0.9f, 2.0f}, {1.0f, 0.92f, 0.82f}, 150.0f, 3.0f},
        // Window light — cool daylight from the right, like a bedroom window
        {"WindowLight", {S * 0.8f, 3.0f, 5.0f}, {0.85f, 0.9f, 1.0f}, 120.0f, 4.0f},
        // Bedside lamp — warm point light on the nightstand
        {"BedsideLamp", {S * 0.55f, -S + 6.0f, -S + 2.0f}, {1.0f, 0.85f, 0.65f}, 40.0f, 1.0f},
        // Fill — subtle front fill to see the face
        {"FrontFill", {0.0f, 0.0f, S * 0.9f}, {1.0f, 0.95f, 0.9f}, 30.0f, 3.0f},
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
