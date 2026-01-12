#pragma once
#include <memory>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include "renderer/lighting/unified_light.hpp"
#include "shadow_map_render_target.hpp"

namespace ohao {

class VulkanContext;
class OhaoVkPipeline;
class OhaoVkUniformBuffer;
class OhaoVkShaderModule;
class OhaoVkBuffer;

class ShadowRenderer {
public:
    ShadowRenderer() = default;
    ~ShadowRenderer();

    bool initialize(VulkanContext* context);
    void cleanup();

    // Render shadow map for directional light
    void beginShadowPass(VkCommandBuffer cmd);
    void renderShadowMap(VkCommandBuffer cmd, uint32_t frameIndex);
    void endShadowPass(VkCommandBuffer cmd);

    // Calculate light space matrix for any light type (uses UnifiedLight)
    // Automatically chooses orthographic (directional) or perspective (spot/point) projection
    glm::mat4 calculateLightSpaceMatrix(const UnifiedLight& light, const glm::vec3& sceneCenter) const;

    // Calculate perspective light space matrix for spot/point lights
    glm::mat4 calculateSpotLightSpaceMatrix(const UnifiedLight& light) const;

    // Update the shadow uniform buffer
    void updateShadowUniforms(uint32_t frameIndex, const glm::mat4& lightSpaceMatrix);

    // Getters
    ShadowMapRenderTarget* getShadowMapTarget() const { return shadowMapTarget.get(); }
    VkImageView getShadowMapImageView() const;
    VkSampler getShadowMapSampler() const;
    float getShadowBias() const { return shadowBias; }
    float getShadowStrength() const { return shadowStrength; }
    glm::mat4 getLightSpaceMatrix() const { return lightSpaceMatrix; }
    bool isEnabled() const { return enabled; }

    // Setters
    void setShadowBias(float bias) { shadowBias = bias; }
    void setShadowStrength(float strength) { shadowStrength = strength; }
    void setOrthoSize(float size) { orthoSize = size; }
    void setNearPlane(float near) { nearPlane = near; }
    void setFarPlane(float far) { farPlane = far; }
    void setEnabled(bool enable) { enabled = enable; }

private:
    VulkanContext* context{nullptr};

    std::unique_ptr<ShadowMapRenderTarget> shadowMapTarget;
    std::unique_ptr<OhaoVkPipeline> shadowPipeline;
    std::unique_ptr<OhaoVkShaderModule> shadowShaderModule;
    std::unique_ptr<OhaoVkUniformBuffer> shadowUniformBuffer;

    // Shadow parameters
    float shadowBias{0.005f};
    float shadowStrength{0.75f};
    float orthoSize{200.0f};    // Orthographic projection size for directional light
    float nearPlane{0.1f};
    float farPlane{500.0f};     // Extended for larger scenes
    bool enabled{true};

    glm::mat4 lightSpaceMatrix{1.0f};

    bool createShadowPipeline();
    bool createShadowUniformBuffer();
};

} // namespace ohao
