#pragma once
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

// Presentation target. Images are TRANSFER_DST only: the renderer blits its
// final image into them and never renders into them directly.
//
// Render-finished semaphores are one PER IMAGE, indexed by the acquired image,
// not per frame-in-flight slot: a slot-indexed semaphore can still be pending
// on a presentation of a different image when the slot comes round again.
class Swapchain {
public:
    enum class Acquire { Ok, OutOfDate, Error };

    Swapchain() = default;
    ~Swapchain() { destroy(); }
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                              VkExtent2D desired, bool vsync);
    void destroy();
    // Waits for the device to idle, then rebuilds at `desired`. Returns false for
    // a 0x0 extent (minimized) or on failure; the swapchain is then invalid until
    // a later recreate succeeds.
    [[nodiscard]] bool recreate(VkExtent2D desired);
    [[nodiscard]] Acquire  acquire(VkSemaphore imageAcquired, uint32_t& imageIndex);
    [[nodiscard]] VkResult present(VkQueue queue, uint32_t imageIndex);

    [[nodiscard]] VkImage     image(uint32_t i) const { return m_images[i]; }
    [[nodiscard]] VkSemaphore renderFinished(uint32_t i) const { return m_renderFinished[i]; }
    [[nodiscard]] VkExtent2D  extent() const noexcept { return m_extent; }
    [[nodiscard]] VkFormat    format() const noexcept { return m_format; }
    [[nodiscard]] uint32_t    imageCount() const noexcept { return static_cast<uint32_t>(m_images.size()); }
    [[nodiscard]] bool        valid() const noexcept { return m_swapchain != VK_NULL_HANDLE; }

    // Pure selection rules, unit-tested without a GPU.
    static VkSurfaceFormatKHR chooseFormat(std::span<const VkSurfaceFormatKHR> formats);
    static VkPresentModeKHR   choosePresentMode(std::span<const VkPresentModeKHR> modes, bool vsync);
    static uint32_t           chooseImageCount(const VkSurfaceCapabilitiesKHR& caps);
    static VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D desired);

private:
    [[nodiscard]] bool build(VkExtent2D desired, VkSwapchainKHR old);
    void destroySemaphores();

    VkPhysicalDevice m_physical{VK_NULL_HANDLE};
    VkDevice         m_device{VK_NULL_HANDLE};
    VkSurfaceKHR     m_surface{VK_NULL_HANDLE};
    VkSwapchainKHR   m_swapchain{VK_NULL_HANDLE};
    bool             m_vsync{false};
    VkFormat         m_format{VK_FORMAT_UNDEFINED};
    VkExtent2D       m_extent{0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkSemaphore> m_renderFinished;   // one per image
};

} // namespace ohao
