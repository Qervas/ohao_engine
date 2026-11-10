#pragma once
#include <mutex>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

#include "gpu/vulkan/present_target.hpp"

namespace ohao {

// Presentation target. Images are TRANSFER_DST only: the renderer blits its
// final image into them and never renders into them directly.
//
// Render-finished semaphores are one PER IMAGE, indexed by the acquired image,
// not per frame-in-flight slot: a slot-indexed semaphore can still be pending
// on a presentation of a different image when the slot comes round again.
//
// The swapchain handle is used from two threads when presents are waited on
// (gpu/vulkan/present_waiter.hpp): every host access to it (present, rebuild,
// destroy, vkWaitForPresentKHR) holds hostMutex(), as Vulkan requires.
class Swapchain {
public:
    enum class Acquire { Ok, OutOfDate, Timeout, Error };   // Timeout: no image within gpu::kGpuTimeout

    Swapchain() = default;
    ~Swapchain() { destroy(); }
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                              VkExtent2D desired, PresentMode mode);
    void destroy();
    // Waits for the device to idle, then rebuilds at `desired`. Returns false for
    // a 0x0 extent (minimized) or on failure; the swapchain is then invalid until
    // a later recreate succeeds.
    [[nodiscard]] bool recreate(VkExtent2D desired);
    [[nodiscard]] Acquire  acquire(VkSemaphore imageAcquired, uint32_t& imageIndex);
    // `presentId` > 0 tags the present (VK_KHR_present_id) so its display can be waited on.
    [[nodiscard]] VkResult present(VkQueue queue, uint32_t imageIndex, uint64_t presentId = 0);

    [[nodiscard]] VkImage     image(uint32_t i) const { return m_images[i]; }
    [[nodiscard]] VkSemaphore renderFinished(uint32_t i) const { return m_renderFinished[i]; }
    [[nodiscard]] VkExtent2D  extent() const noexcept { return m_extent; }
    [[nodiscard]] VkFormat    format() const noexcept { return m_format; }
    [[nodiscard]] uint32_t    imageCount() const noexcept { return static_cast<uint32_t>(m_images.size()); }
    [[nodiscard]] bool        valid() const noexcept { return m_swapchain != VK_NULL_HANDLE; }
    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return m_swapchain; }
    [[nodiscard]] std::mutex& hostMutex() noexcept { return m_hostMutex; }
    // The mode the surface gave (the one asked for, or the fallback).
    [[nodiscard]] PresentMode presentMode() const noexcept { return m_chosen; }
    // Bumped by every rebuild: present ids waited on belong to one generation.
    [[nodiscard]] uint64_t generation() const noexcept { return m_generation; }

    // Pure selection rules, unit-tested without a GPU.
    static VkSurfaceFormatKHR chooseFormat(std::span<const VkSurfaceFormatKHR> formats);
    // The mode asked for, else the next one down: immediate -> mailbox -> fifo. Mailbox
    // never falls back to immediate (it must not tear).
    static VkPresentModeKHR   choosePresentMode(std::span<const VkPresentModeKHR> modes, PresentMode want);
    static uint32_t           chooseImageCount(const VkSurfaceCapabilitiesKHR& caps);
    static VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D desired);

private:
    [[nodiscard]] bool build(VkExtent2D desired, VkSwapchainKHR old);
    void destroySemaphores();

    VkPhysicalDevice m_physical{VK_NULL_HANDLE};
    VkDevice         m_device{VK_NULL_HANDLE};
    VkSurfaceKHR     m_surface{VK_NULL_HANDLE};
    VkSwapchainKHR   m_swapchain{VK_NULL_HANDLE};
    PresentMode      m_want{PresentMode::Mailbox};
    PresentMode      m_chosen{PresentMode::Mailbox};
    bool             m_loggedMode{false};
    uint64_t         m_generation{0};
    std::mutex       m_hostMutex;
    VkFormat         m_format{VK_FORMAT_UNDEFINED};
    VkExtent2D       m_extent{0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkSemaphore> m_renderFinished;   // one per image
};

} // namespace ohao
