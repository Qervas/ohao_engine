#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Volumetric Lighting/Fog using froxel-based ray marching
class VolumetricPass : public RenderPassBase {
public:
    VolumetricPass() = default;
    ~VolumetricPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "VolumetricPass"; }

    // Input configuration
    void setDepthBuffer(VkImageView depth);
    void setShadowMap(VkImageView shadow, VkSampler shadowSampler);
    void setLightBuffer(VkBuffer lightBuffer);
    void setMatrices(const glm::mat4& view, const glm::mat4& proj,
                     const glm::mat4& invView, const glm::mat4& invProj);

    void updateDescriptorSet();

    // Configuration
    void setDensity(float density) { m_density = density; }
    void setScattering(float g) { m_scattering = g; }   // Henyey-Greenstein g parameter
    void setAbsorption(float absorption) { m_absorption = absorption; }
    void setFogColor(const glm::vec3& color) { m_fogColor = color; }
    void setFogHeight(float height) { m_fogHeight = height; }
    void setFogFalloff(float falloff) { m_fogFalloff = falloff; }
    void setMaxDistance(float dist) { m_maxDistance = dist; }
    void setSampleCount(uint32_t samples) { m_sampleCount = samples; }

    // Output
    VkImageView getScatteringView() const { return m_scatteringView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createFroxelVolume();
    bool createScatteringOutput();
    bool createDescriptors();
    bool createInjectPipeline();
    bool createScatterPipeline();
    bool createIntegratePipeline();
    void destroyResources();

    // Froxel volume (3D texture for volumetric data)
    // Resolution: width/8 x height/8 x 128 depth slices
    VkImage m_froxelVolume{VK_NULL_HANDLE};
    VkDeviceMemory m_froxelMemory{VK_NULL_HANDLE};
    VkImageView m_froxelView{VK_NULL_HANDLE};

    // Scattering accumulation (3D texture)
    VkImage m_scatterVolume{VK_NULL_HANDLE};
    VkDeviceMemory m_scatterMemory{VK_NULL_HANDLE};
    VkImageView m_scatterVolumeView{VK_NULL_HANDLE};

    // Final 2D scattering output (integrated)
    VkImage m_scatteringOutput{VK_NULL_HANDLE};
    VkDeviceMemory m_scatteringMemory{VK_NULL_HANDLE};
    VkImageView m_scatteringView{VK_NULL_HANDLE};

    // Pipelines
    VkPipeline m_injectPipeline{VK_NULL_HANDLE};     // Inject light into froxels
    VkPipeline m_scatterPipeline{VK_NULL_HANDLE};    // Compute scattering
    VkPipeline m_integratePipeline{VK_NULL_HANDLE};  // Integrate along view rays
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Samplers
    VkSampler m_sampler{VK_NULL_HANDLE};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};

    // Input views
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_shadowView{VK_NULL_HANDLE};
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};

    // Matrices
    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_invView;
    glm::mat4 m_invProjection;

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};
    static constexpr uint32_t FROXEL_TILE_SIZE = 8;
    static constexpr uint32_t FROXEL_DEPTH_SLICES = 128;

    // Parameters
    float m_density{0.01f};        // Base fog density
    float m_scattering{0.8f};      // Henyey-Greenstein g parameter (-1 to 1)
    float m_absorption{0.01f};     // Light absorption coefficient
    glm::vec3 m_fogColor{1.0f};    // Fog/scattering color
    float m_fogHeight{10.0f};      // Height fog maximum height
    float m_fogFalloff{0.5f};      // Height fog falloff rate
    float m_maxDistance{500.0f};   // Maximum fog distance
    uint32_t m_sampleCount{64};    // Ray march samples

    // Push constants
    struct VolumetricParams {
        glm::mat4 invView;
        glm::mat4 invProjection;
        glm::vec4 fogColorDensity;    // rgb = color, a = density
        glm::vec4 scatterParams;      // x = g, y = absorption, z = height, w = falloff
        glm::vec4 volumeParams;       // xyz = froxel dims, w = max distance
        float nearPlane;
        float farPlane;
        uint32_t sampleCount;
        uint32_t frameIndex;
    };
};

} // namespace ohao
