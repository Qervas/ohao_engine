#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>

namespace ohao {

// Forward declarations
class Scene;
struct FrameResources;

// Shared render target resources (defined before RenderPassBase so helpers can return it)
struct RenderTarget {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format;
    uint32_t width{0};
    uint32_t height{0};

    void destroy(VkDevice device) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
};

// Base class for all render passes
class RenderPassBase {
public:
    virtual ~RenderPassBase() = default;

    // Initialize pass resources (pipelines, descriptors, etc.)
    virtual bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) = 0;

    // Cleanup pass resources
    virtual void cleanup() = 0;

    // Record commands for this pass
    virtual void execute(VkCommandBuffer cmd, uint32_t frameIndex) = 0;

    // Resize handling
    virtual void onResize(uint32_t width, uint32_t height) {}

    // Get pass name for debugging
    virtual const char* getName() const = 0;

    // Hot-reload: load a new SPV and recreate the pipeline.
    // Returns true if the pass supports hot-reload and succeeded.
    virtual bool reloadShader(const std::string& spvPath) { (void)spvPath; return false; }

    // Set shader base path for all passes (call before initializing any pass)
    static void setShaderBasePath(const std::string& path) { s_shaderBasePath = path; }
    static const std::string& getShaderBasePath() { return s_shaderBasePath; }

protected:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Shader base path (shared across all passes)
    static inline std::string s_shaderBasePath = "bin/shaders/";

    // Helper to create shader module
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
    VkShaderModule loadShaderModule(const std::string& path);

    // Helper to find memory type
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    // --- Vulkan resource helpers (reduce per-pass boilerplate) ---

    // Create image + memory + view in one call
    RenderTarget createRenderTarget(VkFormat format, uint32_t width, uint32_t height,
                                    VkImageUsageFlags usage,
                                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Create sampler with common defaults
    VkSampler createSampler(VkFilter filter = VK_FILTER_LINEAR,
                            VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Single image layout transition
    static void transitionImage(VkCommandBuffer cmd, VkImage image,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    // Create compute pipeline from shader path
    bool createComputePipeline(const std::string& shaderPath,
                               VkDescriptorSetLayout descriptorLayout,
                               uint32_t pushConstantSize,
                               VkPipeline& outPipeline, VkPipelineLayout& outLayout);

    // Hot-reload helper: destroy old pipeline/layout, load SPV from absolute path,
    // recreate pipeline with existing descriptor layout. Returns true on success.
    bool reloadComputeShader(const std::string& absoluteSpvPath,
                             VkDescriptorSetLayout descriptorLayout,
                             uint32_t pushConstantSize,
                             VkPipeline& pipeline, VkPipelineLayout& pipelineLayout);

    // Safe Vulkan handle cleanup (null-check + destroy + reset)
    void safeDestroy(VkPipeline& handle);
    void safeDestroy(VkPipelineLayout& handle);
    void safeDestroy(VkDescriptorPool& handle);
    void safeDestroy(VkDescriptorSetLayout& handle);
    void safeDestroy(VkSampler& handle);
    void safeDestroy(VkImageView& handle);
    void safeDestroy(VkImage& handle);
    void safeDestroy(VkBuffer& handle);
    void safeDestroy(VkFramebuffer& handle);
    void safeDestroy(VkRenderPass& handle);
    void safeFree(VkDeviceMemory& handle);
};

// Attachment description for G-Buffer and render targets
struct AttachmentInfo {
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkClearValue clearValue;
    std::string name;
};

// G-Buffer attachment indices
enum class GBufferAttachment : uint32_t {
    Position = 0,    // RGB: World position, A: Metallic
    Normal = 1,      // RGB: Encoded normal, A: Roughness
    Albedo = 2,      // RGB: Albedo, A: AO
    Velocity = 3,    // RG: Motion vectors
    Depth = 4,       // Depth buffer
    Count = 5
};

// Cascade shadow map data
struct CascadeData {
    glm::mat4 viewProj[4];
    glm::vec4 splitDepths;
    float cascadeBlendWidth{0.1f};
    float shadowBias{0.012f};
    float normalBias{0.02f};
    uint32_t cascadeCount{4};
};

} // namespace ohao
