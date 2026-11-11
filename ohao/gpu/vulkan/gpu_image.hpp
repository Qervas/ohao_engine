#pragma once
// A device image that knows what last touched it (system design 4.6, step 4a). Every image a frame's passes read
// or write is one of these: made by GpuAllocator::createImage (VMA, named, so a frame's memory can be listed by
// image), given back with GpuAllocator::retire (destroyed after the frames in flight, never after a device wait),
// and moved between layouts only by render::Pass (render/graph/pass.hpp), which reads and writes `states`.
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ohao::gpu {

// What last touched a resource (an image subresource or a buffer): the write that made its contents and the
// stages that write has been made visible to since. A barrier is derived from this and the next use.
struct State {
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 writeStage{0};
    VkAccessFlags2 writeAccess{0};
    VkPipelineStageFlags2 readStages{0};
    VkAccessFlags2 readAccess{0};
};

struct ImageDesc {
    std::string name;   // what the memory report and the validation layer call it
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{1};
    uint32_t height{1};
    VkImageUsageFlags usage{0};
    uint32_t mips{1};
    uint32_t layers{1};
    bool cube{false};   // 6 layers, viewed as a cube
};

struct Image {
    static constexpr uint32_t kAll = ~0u;

    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};   // every mip and layer
    ImageDesc desc;
    VkDeviceSize bytes{0};
    std::vector<State> states;          // [layer * mips + mip]

    [[nodiscard]] explicit operator bool() const noexcept { return image != VK_NULL_HANDLE; }
    [[nodiscard]] State& state(uint32_t mip, uint32_t layer) { return states[layer * desc.mips + mip]; }
    [[nodiscard]] VkImageAspectFlags aspect() const noexcept;
    [[nodiscard]] VkExtent2D extent(uint32_t mip = 0) const noexcept {
        return {desc.width >> mip ? desc.width >> mip : 1u, desc.height >> mip ? desc.height >> mip : 1u};
    }
    // A view of one mip of one layer (`layer` kAll: of every layer, as an array), made on first use and destroyed
    // with the image.
    [[nodiscard]] VkImageView subView(uint32_t mip, uint32_t layer = kAll);

    // An image somebody else owns (a swapchain's): tracked, never destroyed here.
    [[nodiscard]] static Image wrap(VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                                    std::string name);
    // The image was left in `layout` by code that records its own barriers (NRD, DLSS); `stage` and `access` are
    // what a later use must wait for.
    void assume(VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);

    // GpuAllocator's.
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};   // null: wrapped
    struct SubView { uint32_t mip, layer; VkImageView view; };
    std::vector<SubView> subViews;
};

[[nodiscard]] bool isDepthFormat(VkFormat format) noexcept;

} // namespace ohao::gpu
