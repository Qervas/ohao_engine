#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <concepts>

namespace ohao {

// Forward declarations
class Scene;
struct FrameResources;

// Shared render target resources (defined before RenderPassBase so helpers can return it)
struct RenderTarget {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{0};
    uint32_t height{0};

    [[nodiscard]] bool valid() const noexcept {
        return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return VkExtent2D{.width = width, .height = height};
    }

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

    [[nodiscard]] bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

    // Initialize pass resources (pipelines, descriptors, etc.)
    [[nodiscard]] virtual bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) = 0;

    // Cleanup pass resources
    virtual void cleanup() = 0;

    // Record commands for this pass
    virtual void execute(VkCommandBuffer cmd, uint32_t frameIndex) = 0;

    // Resize handling
    virtual void onResize(uint32_t width, uint32_t height) {}

    // Get pass name for debugging
    [[nodiscard]] virtual const char* getName() const = 0;

    // Hot-reload: load a new SPV and recreate the pipeline.
    // Returns true if the pass supports hot-reload and succeeded.
    [[nodiscard]] virtual bool reloadShader(std::string_view spvPath) { (void)spvPath; return false; }

    // Set shader base path for all passes (call before initializing any pass)
    static void setShaderBasePath(std::string_view path) { s_shaderBasePath = std::string(path); }
    [[nodiscard]] static const std::string& getShaderBasePath() { return s_shaderBasePath; }

protected:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Shader base path (shared across all passes)
    static inline std::string s_shaderBasePath = "bin/shaders/";

    // Helper to create shader module
    [[nodiscard]] VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
    [[nodiscard]] VkShaderModule loadShaderModule(std::string_view path);

    // Helper to find memory type
    [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    // --- Vulkan resource helpers (reduce per-pass boilerplate) ---

    // Create image + memory + view in one call
    [[nodiscard]] RenderTarget createRenderTarget(VkFormat format, uint32_t width, uint32_t height,
                                                  VkImageUsageFlags usage,
                                                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Create sampler with common defaults
    [[nodiscard]] VkSampler createSampler(VkFilter filter = VK_FILTER_LINEAR,
                                          VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Single image layout transition
    static void transitionImage(VkCommandBuffer cmd, VkImage image,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    // Create compute pipeline from shader path
    [[nodiscard]] bool createComputePipeline(std::string_view shaderPath,
                                             VkDescriptorSetLayout descriptorLayout,
                                             uint32_t pushConstantSize,
                                             VkPipeline& outPipeline, VkPipelineLayout& outLayout);

    // Hot-reload helper: destroy old pipeline/layout, load SPV from absolute path,
    // recreate pipeline with existing descriptor layout. Returns true on success.
    [[nodiscard]] bool reloadComputeShader(std::string_view absoluteSpvPath,
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

/// Documents the virtual pass surface for static helpers (keep runtime virtual).
template<typename T>
concept RenderPassLike = requires(T& t, VkDevice d, VkPhysicalDevice pd,
                                  VkCommandBuffer cmd, uint32_t frame) {
    { t.initialize(d, pd) } -> std::convertible_to<bool>;
    { t.cleanup() } -> std::same_as<void>;
    { t.execute(cmd, frame) } -> std::same_as<void>;
    { t.getName() } -> std::convertible_to<const char*>;
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
