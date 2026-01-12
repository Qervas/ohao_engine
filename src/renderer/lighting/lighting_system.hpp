#pragma once

#include "unified_light.hpp"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <optional>
#include <glm/glm.hpp>

namespace ohao {

class OhaoVkDevice;
class OhaoVkBuffer;
class Scene;
class Actor;
class LightComponent;
class ShadowMapPool;

// LightingSystem - Single point of truth for all lighting and shadows
// Key principle: Light + Shadow = One Unit
class LightingSystem {
public:
    LightingSystem() = default;
    ~LightingSystem();

    // Initialize with device and frame count (for multiple frames in flight)
    bool initialize(OhaoVkDevice* device, uint32_t frameCount);
    void cleanup();

    // Collect lights from scene - converts LightComponents to UnifiedLights
    void collectLightsFromScene(Scene* scene);

    // Manual light management (for procedural lights)
    LightHandle addLight(const LightConfig& config);
    LightHandle addLight(const UnifiedLight& light);
    void removeLight(LightHandle handle);
    void clearLights();

    // Shadow management - returns true if shadow was enabled successfully
    bool enableShadowCasting(LightHandle handle, ShadowMapPool* pool);
    void disableShadowCasting(LightHandle handle);

    // Update light properties
    void setLightPosition(LightHandle handle, const glm::vec3& pos);
    void setLightDirection(LightHandle handle, const glm::vec3& dir);
    void setLightColor(LightHandle handle, const glm::vec3& color);
    void setLightIntensity(LightHandle handle, float intensity);

    // Calculate light space matrix for a shadow-casting light
    void updateLightSpaceMatrix(
        LightHandle handle,
        const glm::vec3& sceneCenter,
        float orthoSize = 20.0f,
        float nearPlane = 0.1f,
        float farPlane = 100.0f
    );

    // Atomic GPU update - all lights + shadows updated together
    void updateGPUBuffer(uint32_t frameIndex);

    // Shadow parameters
    void setShadowBias(float bias) { lightingUBO.shadowBias = bias; }
    void setShadowStrength(float strength) { lightingUBO.shadowStrength = strength; }
    float getShadowBias() const { return lightingUBO.shadowBias; }
    float getShadowStrength() const { return lightingUBO.shadowStrength; }

    // Accessors
    int getNumLights() const { return static_cast<int>(lights.size()); }
    const UnifiedLight* getLight(LightHandle handle) const;
    UnifiedLight* getMutableLight(LightHandle handle);
    const std::vector<UnifiedLight>& getAllLights() const { return lights; }

    // Get lights that cast shadows (for shadow map rendering)
    std::vector<std::pair<LightHandle, const UnifiedLight*>> getShadowCastingLights() const;

    // Get first directional light (convenience for simple scenes)
    std::optional<LightHandle> getFirstDirectionalLight() const;

    // GPU buffer accessor
    OhaoVkBuffer* getUniformBuffer(uint32_t frameIndex) const;
    VkDeviceSize getUniformBufferSize() const { return sizeof(LightingUBO); }
    const LightingUBO& getLightingUBO() const { return lightingUBO; }

    // Validation
    bool isValidHandle(LightHandle handle) const;

private:
    OhaoVkDevice* device{nullptr};

    // Light data
    std::vector<UnifiedLight> lights;
    uint32_t nextLightId{0};

    // GPU resources
    std::vector<std::unique_ptr<OhaoVkBuffer>> uniformBuffers;
    std::vector<void*> mappedMemory;
    uint32_t frameCount{0};

    // Cached UBO for CPU-side updates
    LightingUBO lightingUBO{};

    // Convert LightComponent to UnifiedLight
    UnifiedLight convertFromComponent(const class LightComponent* comp, class Actor* actor);

    // Create uniform buffers for multiple frames in flight
    bool createUniformBuffers();
};

} // namespace ohao
