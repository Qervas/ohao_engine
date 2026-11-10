#include "gpu/vulkan/swapchain.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include <algorithm>
#include <utility>
#include "core/log.hpp"

namespace ohao {

// UNORM, not sRGB: the deferred output is already gamma-encoded by the tonemap
// shader, and an sRGB swapchain would encode it a second time on the blit.
VkSurfaceFormatKHR Swapchain::chooseFormat(std::span<const VkSurfaceFormatKHR> formats) {
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
        for (const auto& f : formats)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    return formats.empty()
               ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
               : formats.front();
}

VkPresentModeKHR Swapchain::choosePresentMode(std::span<const VkPresentModeKHR> modes, PresentMode want) {
    const auto offered = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (want == PresentMode::Immediate && offered(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (want != PresentMode::Fifo && offered(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;   // the only mode the spec guarantees
}

uint32_t Swapchain::chooseImageCount(const VkSurfaceCapabilitiesKHR& caps) {
    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) n = std::min(n, caps.maxImageCount);   // 0 means unlimited
    return n;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D desired) {
    if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;   // surface dictates
    return {std::clamp(desired.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

bool Swapchain::create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                       VkExtent2D desired, PresentMode mode) {
    m_physical = physical;
    m_device = device;
    m_surface = surface;
    m_want = mode;
    return build(desired, VK_NULL_HANDLE);
}

// Always retires `old`, on every path, so callers never have to know how far it got.
bool Swapchain::build(VkExtent2D desired, VkSwapchainKHR old) {
    auto retire = [&] { if (old != VK_NULL_HANDLE) { vkDestroySwapchainKHR(m_device, old, nullptr); old = VK_NULL_HANDLE; } };
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);
    const VkExtent2D extent = chooseExtent(caps, desired);
    if (extent.width == 0 || extent.height == 0) { retire(); return false; }   // minimized

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &n, modes.data());

    const VkSurfaceFormatKHR fmt = chooseFormat(formats);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_physical, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        OHAO_LOG(Error, gpu, "Swapchain: surface format {} cannot be a blit destination",
                 static_cast<int>(fmt.format));
        retire();
        return false;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_surface;
    ci.minImageCount    = chooseImageCount(caps);
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = choosePresentMode(modes, m_want);
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;
    m_chosen = ci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? PresentMode::Immediate
               : ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? PresentMode::Mailbox
                                                               : PresentMode::Fifo;
    if (!std::exchange(m_loggedMode, true)) {
        if (m_chosen != m_want)
            OHAO_LOG(Info, gpu, "present: {} asked for, the surface offers {}", presentModeName(m_want),
                     presentModeName(m_chosen));
        else
            OHAO_LOG(Info, gpu, "present: {}", presentModeName(m_chosen));
    }

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &sc);
    retire();
    if (r != VK_SUCCESS) return false;
    m_swapchain = sc;
    ++m_generation;
    m_format = fmt.format;
    m_extent = extent;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
    m_images.resize(n);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_images.data());

    destroySemaphores();
    m_renderFinished.resize(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& s : m_renderFinished)
        if (vkCreateSemaphore(m_device, &si, nullptr, &s) != VK_SUCCESS) return false;
    return true;
}

bool Swapchain::recreate(VkExtent2D desired) {
    vkDeviceWaitIdle(m_device);
    std::lock_guard lock(m_hostMutex);
    VkSwapchainKHR old = m_swapchain;
    m_swapchain = VK_NULL_HANDLE;
    m_images.clear();
    if (!build(desired, old)) {   // build() retired `old` whatever happened
        destroySemaphores();
        return false;
    }
    return true;
}

Swapchain::Acquire Swapchain::acquire(VkSemaphore imageAcquired, uint32_t& imageIndex) {
    if (!valid()) return Acquire::OutOfDate;
    const auto timeout = static_cast<uint64_t>(std::chrono::nanoseconds(gpu::kGpuTimeout).count());
    const VkResult r = gpu::checkResult(
        vkAcquireNextImageKHR(m_device, m_swapchain, timeout, imageAcquired, VK_NULL_HANDLE, &imageIndex), "acquire");
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return Acquire::OutOfDate;
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return Acquire::Timeout;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return Acquire::Ok;
    return Acquire::Error;
}

VkResult Swapchain::present(VkQueue queue, uint32_t imageIndex, uint64_t presentId) {
    VkPresentIdKHR id{};
    id.sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    id.swapchainCount = 1;
    id.pPresentIds    = &presentId;
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.pNext              = presentId > 0 ? &id : nullptr;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderFinished[imageIndex];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &imageIndex;
    std::lock_guard lock(m_hostMutex);
    return gpu::checkResult(vkQueuePresentKHR(queue, &pi), "present");
}

void Swapchain::destroySemaphores() {
    for (auto s : m_renderFinished)
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(m_device, s, nullptr);
    m_renderFinished.clear();
}

void Swapchain::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    std::lock_guard lock(m_hostMutex);
    destroySemaphores();
    if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_images.clear();
}

} // namespace ohao
