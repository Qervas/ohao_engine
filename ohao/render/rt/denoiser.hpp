#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// Legacy A-Trous wavelet denoiser (compute). Prefer DenoiseMode::Atrous (SVGF) for new work.
class RTDenoiser {
public:
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physDevice, uint32_t width, uint32_t height);
    void destroy();

    void denoise(VkCommandBuffer cmd,
                 VkImageView inputView,
                 VkImageView outputView,
                 VkImageView normalView,
                 VkImageView accumView,
                 uint32_t width, uint32_t height,
                 int iterations = 5);

    [[nodiscard]] bool isInitialized() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

private:
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createDescriptors();
    [[nodiscard]] bool createTempImage(uint32_t width, uint32_t height);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;

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
