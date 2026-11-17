#pragma once
// capture.preview (sub-project 2b spec, "Capture budgets"): an inline capture's preview made
// on the GPU, in the capture frame. A box filter of a rectangle of the final RGBA8 image to
// at most maxSide on its long side (render::previewExtent), written packed straight into the
// frame's host-cached staging buffer; the frame copies nothing else, so the CPU reads only
// the preview. Recorded only for a frame that asked for it (VulkanRenderer::requestPreviewReadback).
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace ohao {

class CapturePreviewPass {
public:
    CapturePreviewPass() = default;
    ~CapturePreviewPass();
    CapturePreviewPass(const CapturePreviewPass&) = delete;
    CapturePreviewPass& operator=(const CapturePreviewPass&) = delete;

    [[nodiscard]] bool initialize(VkDevice device);
    void cleanup();

    // Records the box filter of `rect` ([x, y, w, h], inside the image) of `final` (an RGBA8
    // image) down to dstWidth x dstHeight into `out` (a storage buffer of at least
    // dstWidth * dstHeight * 4 bytes, rows top down), using the frame slot's descriptor set,
    // timed as capture.preview. The writes are made visible to the host.
    void record(const render::Frame& frame, gpu::Image& final, const std::array<uint32_t, 4>& rect, uint32_t dstWidth,
                uint32_t dstHeight, GpuBuffer& out);

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};   // written at record time, one a slot
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
};

} // namespace ohao
