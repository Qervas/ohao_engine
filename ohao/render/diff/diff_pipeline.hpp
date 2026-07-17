#pragma once

// Diff-IR Vulkan host: real GPU resources (albedo map image + staging + cmd pool).

#include "render/diff/diff_types.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace ohao::diff {

/// Owns Diff-IR GPU objects. Feature logic stays out of renderer.cpp.
class DiffPipeline {
public:
    DiffPipeline() = default;
    ~DiffPipeline() { destroy(); }

    DiffPipeline(const DiffPipeline&) = delete;
    DiffPipeline& operator=(const DiffPipeline&) = delete;

    /// Create map image (RGBA32F), staging buffer, one-shot command pool.
    [[nodiscard]] DiffStatus init(VkDevice device, VkPhysicalDevice physical,
                                  std::uint32_t mapW, std::uint32_t mapH,
                                  std::uint32_t beautyW, std::uint32_t beautyH);

    void resize(std::uint32_t beautyW, std::uint32_t beautyH);
    void destroy() noexcept;

    /// Upload linear RGB map (w*h*3 floats) → GPU RGBA32F image via vkCmdCopyBufferToImage.
    [[nodiscard]] DiffStatus uploadAlbedoMap(const float* rgb, std::uint32_t w, std::uint32_t h);

    /// Download GPU map → linear RGB (proves image is live; source of truth for materials).
    [[nodiscard]] DiffStatus downloadAlbedoMap(float* rgbOut, std::uint32_t w, std::uint32_t h);

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] VkImage albedoImage() const noexcept { return albedoImage_; }
    [[nodiscard]] VkImageView albedoView() const noexcept { return albedoView_; }
    [[nodiscard]] DiffMapDesc mapDesc() const noexcept { return mapDesc_; }
    [[nodiscard]] DiffRenderDesc beautyDesc() const noexcept { return beautyDesc_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }

private:
    [[nodiscard]] std::uint32_t findMemType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const;

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkCommandPool cmdPool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    std::uint32_t queueFamily_{0};

    VkImage albedoImage_{VK_NULL_HANDLE};
    VkDeviceMemory albedoMem_{VK_NULL_HANDLE};
    VkImageView albedoView_{VK_NULL_HANDLE};

    VkBuffer stagingBuf_{VK_NULL_HANDLE};
    VkDeviceMemory stagingMem_{VK_NULL_HANDLE};
    void* stagingMapped_{nullptr};
    VkDeviceSize stagingBytes_{0};

    DiffMapDesc mapDesc_{};
    DiffRenderDesc beautyDesc_{};
    bool ready_{false};
};

} // namespace ohao::diff
