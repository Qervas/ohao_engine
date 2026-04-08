#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace ohao {

enum class ParticleType : uint32_t {
    MUZZLE_FLASH  = 0,
    IMPACT_SPARK  = 1,
    EXPLOSION     = 2,
    SMOKE         = 3,
    DEBRIS        = 4,
    FIRE          = 5,
    WATER_SPLASH  = 6   // upward cone of water droplets, blue-tinted
};

struct ParticleEmitterConfig {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float spreadAngle{0.5f};       // radians
    float minSpeed{1.0f};
    float maxSpeed{5.0f};
    float minLifetime{0.5f};
    float maxLifetime{2.0f};
    float startSize{0.1f};
    float endSize{0.01f};
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    float gravity{9.81f};
    float drag{0.1f};
    uint32_t emitCount{32};
    ParticleType type{ParticleType::IMPACT_SPARK};
};

class ParticleSystem {
public:
    ParticleSystem() = default;
    ~ParticleSystem();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t maxParticles = 65536);
    void cleanup();

    // Emit particles with given configuration
    void emit(VkCommandBuffer cmd, const ParticleEmitterConfig& config, float time);

    // Update all particles (call once per frame before render)
    void update(VkCommandBuffer cmd, float deltaTime);

    // Create the render pipeline (needs a render pass to be provided)
    bool initRenderPipeline(VkRenderPass renderPass);

    // Render particles (call during forward pass over HDR buffer)
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                const glm::vec3& cameraRight, const glm::vec3& cameraUp);

    // Preset emitter configurations for common FPS effects
    static ParticleEmitterConfig presetMuzzleFlash(const glm::vec3& pos, const glm::vec3& dir);
    static ParticleEmitterConfig presetImpactSpark(const glm::vec3& pos, const glm::vec3& normal);
    static ParticleEmitterConfig presetExplosion(const glm::vec3& pos);
    static ParticleEmitterConfig presetSmoke(const glm::vec3& pos);
    static ParticleEmitterConfig presetWaterSplash(const glm::vec3& pos, const glm::vec3& dir);

    uint32_t getMaxParticles() const { return m_maxParticles; }

private:
    bool createBuffers();
    bool createComputePipelines();
    bool createRenderPipeline(VkRenderPass renderPass);
    bool createDescriptors();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    uint32_t m_maxParticles{65536};

    // GPU buffers
    VkBuffer m_particleBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_particleMemory{VK_NULL_HANDLE};
    VkBuffer m_counterBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_counterMemory{VK_NULL_HANDLE};
    VkBuffer m_indirectBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indirectMemory{VK_NULL_HANDLE};

    // Compute pipelines
    VkPipeline m_emitPipeline{VK_NULL_HANDLE};
    VkPipeline m_updatePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_emitPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_updatePipelineLayout{VK_NULL_HANDLE};

    // Render pipeline
    VkPipeline m_renderPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_renderPipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_computeDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_renderDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_computeDescriptorSet{VK_NULL_HANDLE};
    VkDescriptorSet m_renderDescriptorSet{VK_NULL_HANDLE};

    // Helper
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule loadShaderModule(const std::string& path);
};

} // namespace ohao
