#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace ohao {

// A-Trous wavelet denoiser — edge-aware spatial filter
// Runs as compute shader passes after path tracer output
class RTDenoiser {
public:
    bool init(VkDevice device, VkPhysicalDevice physDevice, uint32_t width, uint32_t height);
    void destroy();

    // Run denoiser: reads from inputImage, writes to outputImage
    // Uses normalAOV and accumBuffer for edge detection
    void denoise(VkCommandBuffer cmd,
                 VkImageView inputView,    // noisy tonemapped image
                 VkImageView outputView,   // denoised result (can be same as input after all iterations)
                 VkImageView normalView,   // GBuffer normals
                 VkImageView accumView,    // accumulation buffer (depth proxy)
                 uint32_t width, uint32_t height,
                 int iterations = 5);

private:
    bool createPipeline();
    bool createDescriptors();
    bool createTempImage(uint32_t width, uint32_t height);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;

    // Temp image for ping-pong
    VkImage m_tempImage = VK_NULL_HANDLE;
    VkDeviceMemory m_tempMemory = VK_NULL_HANDLE;
    VkImageView m_tempView = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;

    struct PushConstants {
        int stepSize;
        float sigmaColor;
        float sigmaNormal;
        float sigmaDepth;
    };
};

} // namespace ohao
