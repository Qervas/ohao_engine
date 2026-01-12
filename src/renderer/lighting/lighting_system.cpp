#include "lighting_system.hpp"
#include "shadow_map_pool.hpp"
#include "renderer/rhi/vk/ohao_vk_device.hpp"
#include "renderer/rhi/vk/ohao_vk_buffer.hpp"
#include "renderer/components/light_component.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <iostream>

namespace ohao {

LightingSystem::~LightingSystem() {
    cleanup();
}

bool LightingSystem::initialize(OhaoVkDevice* dev, uint32_t frames) {
    device = dev;
    frameCount = frames;

    // Initialize default shadow parameters
    lightingUBO.shadowBias = 0.005f;
    lightingUBO.shadowStrength = 0.7f;
    lightingUBO.numLights = 0;

    if (!createUniformBuffers()) {
        std::cerr << "LightingSystem: Failed to create uniform buffers" << std::endl;
        return false;
    }

    std::cout << "LightingSystem: Initialized with " << frameCount << " frame buffers" << std::endl;
    return true;
}

void LightingSystem::cleanup() {
    if (device) {
        device->waitIdle();
    }

    for (auto& buffer : uniformBuffers) {
        if (buffer) {
            buffer->cleanup();
        }
    }
    uniformBuffers.clear();
    mappedMemory.clear();
    lights.clear();
}

bool LightingSystem::createUniformBuffers() {
    uniformBuffers.resize(frameCount);
    mappedMemory.resize(frameCount, nullptr);

    for (uint32_t i = 0; i < frameCount; ++i) {
        uniformBuffers[i] = std::make_unique<OhaoVkBuffer>();

        if (!uniformBuffers[i]->initialize(device)) {
            std::cerr << "LightingSystem: Failed to initialize buffer " << i << std::endl;
            return false;
        }

        if (!uniformBuffers[i]->create(
                sizeof(LightingUBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            std::cerr << "LightingSystem: Failed to create uniform buffer " << i << std::endl;
            return false;
        }

        // Map memory persistently
        if (!uniformBuffers[i]->map()) {
            std::cerr << "LightingSystem: Failed to map memory for buffer " << i << std::endl;
            return false;
        }

        mappedMemory[i] = uniformBuffers[i]->getMappedMemory();
    }

    return true;
}

void LightingSystem::collectLightsFromScene(Scene* scene) {
    if (!scene) return;

    clearLights();

    for (const auto& [actorId, actor] : scene->getAllActors()) {
        if (!actor) continue;

        auto lightComp = actor->getComponent<LightComponent>();
        if (!lightComp) continue;

        UnifiedLight light = convertFromComponent(lightComp.get(), actor.get());
        addLight(light);
    }

    std::cout << "LightingSystem: Collected " << lights.size() << " lights from scene" << std::endl;
}

UnifiedLight LightingSystem::convertFromComponent(const LightComponent* comp, Actor* actor) {
    UnifiedLight light{};

    // Get world position from transform
    auto transform = actor->getComponent<TransformComponent>();
    if (transform) {
        light.position = transform->getPosition();
    }

    // Convert light type
    switch (comp->getLightType()) {
        case LightType::Directional:
            light.type = UnifiedLightTypes::Directional;
            light.direction = glm::normalize(comp->getDirection());
            break;
        case LightType::Point:
            light.type = UnifiedLightTypes::Point;
            break;
        case LightType::Spot:
            light.type = UnifiedLightTypes::Spot;
            light.direction = glm::normalize(comp->getDirection());
            light.innerCone = comp->getInnerConeAngle();
            light.outerCone = comp->getOuterConeAngle();
            break;
    }

    light.color = comp->getColor();
    light.intensity = comp->getIntensity();
    light.range = comp->getRange();
    light.shadowMapIndex = -1;  // No shadow by default
    light.lightSpaceMatrix = glm::mat4(1.0f);

    return light;
}

LightHandle LightingSystem::addLight(const LightConfig& config) {
    UnifiedLight light{};

    light.type = config.type;
    light.position = config.position;
    light.direction = glm::normalize(config.direction);
    light.color = config.color;
    light.intensity = config.intensity;
    light.range = config.range;
    light.innerCone = config.innerCone;
    light.outerCone = config.outerCone;
    light.shadowMapIndex = -1;
    light.lightSpaceMatrix = glm::mat4(1.0f);

    return addLight(light);
}

LightHandle LightingSystem::addLight(const UnifiedLight& light) {
    if (lights.size() >= MAX_UNIFIED_LIGHTS) {
        std::cerr << "LightingSystem: Maximum light count (" << MAX_UNIFIED_LIGHTS << ") reached" << std::endl;
        return LightHandle::invalid();
    }

    lights.push_back(light);
    return LightHandle{nextLightId++};
}

void LightingSystem::removeLight(LightHandle handle) {
    if (!isValidHandle(handle)) return;
    lights.erase(lights.begin() + handle.id);
}

void LightingSystem::clearLights() {
    lights.clear();
    nextLightId = 0;
}

bool LightingSystem::enableShadowCasting(LightHandle handle, ShadowMapPool* pool) {
    if (!isValidHandle(handle) || !pool) return false;

    auto shadowMapHandle = pool->allocate();
    if (!shadowMapHandle.isValid()) {
        std::cerr << "LightingSystem: No available shadow maps in pool" << std::endl;
        return false;
    }

    lights[handle.id].shadowMapIndex = static_cast<int32_t>(shadowMapHandle.id);
    return true;
}

void LightingSystem::disableShadowCasting(LightHandle handle) {
    if (!isValidHandle(handle)) return;
    lights[handle.id].shadowMapIndex = -1;
    lights[handle.id].lightSpaceMatrix = glm::mat4(1.0f);
}

void LightingSystem::setLightPosition(LightHandle handle, const glm::vec3& pos) {
    if (!isValidHandle(handle)) return;
    lights[handle.id].position = pos;
}

void LightingSystem::setLightDirection(LightHandle handle, const glm::vec3& dir) {
    if (!isValidHandle(handle)) return;
    lights[handle.id].direction = glm::normalize(dir);
}

void LightingSystem::setLightColor(LightHandle handle, const glm::vec3& color) {
    if (!isValidHandle(handle)) return;
    lights[handle.id].color = color;
}

void LightingSystem::setLightIntensity(LightHandle handle, float intensity) {
    if (!isValidHandle(handle)) return;
    lights[handle.id].intensity = intensity;
}

void LightingSystem::updateLightSpaceMatrix(
    LightHandle handle,
    const glm::vec3& sceneCenter,
    float orthoSize,
    float nearPlane,
    float farPlane
) {
    if (!isValidHandle(handle)) return;

    UnifiedLight& light = lights[handle.id];

    if (light.isDirectional()) {
        // For directional light, position it far away from scene center
        glm::vec3 lightDir = glm::normalize(light.direction);
        glm::vec3 lightPos = sceneCenter - lightDir * farPlane * 0.5f;

        // Calculate up vector (avoid gimbal lock)
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, up);
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);

        // Vulkan clip space has Y pointing down
        lightProj[1][1] *= -1.0f;

        light.lightSpaceMatrix = lightProj * lightView;
    } else if (light.isSpot()) {
        // For spot light, use perspective projection
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 lightDir = glm::normalize(light.direction);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        glm::mat4 lightView = glm::lookAt(light.position, light.position + lightDir, up);
        glm::mat4 lightProj = glm::perspective(
            glm::radians(light.outerCone * 2.0f),
            1.0f,
            nearPlane,
            light.range
        );

        // Vulkan clip space
        lightProj[1][1] *= -1.0f;

        light.lightSpaceMatrix = lightProj * lightView;
    }
    // Point lights require omnidirectional shadow maps (cubemaps) - not implemented yet
}

void LightingSystem::updateGPUBuffer(uint32_t frameIndex) {
    if (frameIndex >= frameCount || !mappedMemory[frameIndex]) return;

    // Copy lights to UBO
    lightingUBO.numLights = static_cast<int32_t>(lights.size());

    for (size_t i = 0; i < lights.size() && i < MAX_UNIFIED_LIGHTS; ++i) {
        lightingUBO.lights[i] = lights[i];
    }

    // Clear unused light slots
    for (size_t i = lights.size(); i < MAX_UNIFIED_LIGHTS; ++i) {
        lightingUBO.lights[i] = UnifiedLight{};
        lightingUBO.lights[i].shadowMapIndex = -1;
    }

    // Write to GPU
    std::memcpy(mappedMemory[frameIndex], &lightingUBO, sizeof(LightingUBO));
}

const UnifiedLight* LightingSystem::getLight(LightHandle handle) const {
    if (!isValidHandle(handle)) return nullptr;
    return &lights[handle.id];
}

UnifiedLight* LightingSystem::getMutableLight(LightHandle handle) {
    if (!isValidHandle(handle)) return nullptr;
    return &lights[handle.id];
}

std::vector<std::pair<LightHandle, const UnifiedLight*>> LightingSystem::getShadowCastingLights() const {
    std::vector<std::pair<LightHandle, const UnifiedLight*>> result;

    for (size_t i = 0; i < lights.size(); ++i) {
        if (lights[i].castsShadow()) {
            result.push_back({LightHandle{static_cast<uint32_t>(i)}, &lights[i]});
        }
    }

    return result;
}

std::optional<LightHandle> LightingSystem::getFirstDirectionalLight() const {
    for (size_t i = 0; i < lights.size(); ++i) {
        if (lights[i].isDirectional()) {
            return LightHandle{static_cast<uint32_t>(i)};
        }
    }
    return std::nullopt;
}

OhaoVkBuffer* LightingSystem::getUniformBuffer(uint32_t frameIndex) const {
    if (frameIndex >= frameCount) return nullptr;
    return uniformBuffers[frameIndex].get();
}

bool LightingSystem::isValidHandle(LightHandle handle) const {
    return handle.isValid() && handle.id < lights.size();
}

} // namespace ohao
