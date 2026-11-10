#include "capture_preview_pass.hpp"
#include "render/shader_bindings/compute_capture_preview.hpp"
#include "render/rt/rt_compute.hpp"
#include "gpu/shared_layout.hpp"
#include "core/log.hpp"

namespace ohao {

namespace shared = gpu::shared;

// maxPushConstantsSize is at least 128 on every Vulkan device.
static_assert(sizeof(shared::PreviewPush) <= 128);

CapturePreviewPass::~CapturePreviewPass() {
    cleanup();
}

bool CapturePreviewPass::initialize(VkDevice device) {
    m_device = device;
    const VkDescriptorSetLayoutBinding bindings[] = {shader::layoutBinding(shader::compute_capture_preview::finalImage),
                                                     shader::layoutBinding(shader::compute_capture_preview::preview)};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (VkDescriptorSet& set : m_sets) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) return false;
    }
    if (!rtc::createComputePipeline(m_device, "compute_capture_preview.slang.spv", "downscale", m_setLayout,
                                    sizeof(shared::PreviewPush), m_layout, m_pipeline)) {
        OHAO_LOG(Error, render, "CapturePreviewPass: pipeline failed");
        return false;
    }
    return true;
}

void CapturePreviewPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_device, m_layout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);   // frees the sets
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

void CapturePreviewPass::record(VkCommandBuffer cmd, uint32_t slot, VkImageView finalView,
                                const std::array<uint32_t, 4>& rect, uint32_t dstWidth, uint32_t dstHeight,
                                VkBuffer out) {
    const VkDescriptorSet set = m_sets[slot];
    VkDescriptorImageInfo image{VK_NULL_HANDLE, finalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo buffer{out, 0, static_cast<VkDeviceSize>(dstWidth) * dstHeight * 4};
    VkWriteDescriptorSet writes[2] = {shader::write(shader::compute_capture_preview::finalImage, set),
                                      shader::write(shader::compute_capture_preview::preview, set)};
    writes[0].pImageInfo = &image;
    writes[1].pBufferInfo = &buffer;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    // The final image's writers (tonemap's or lighting's render pass, the gizmo) before the
    // filter reads it.
    VkMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toRead, 0, nullptr, 0, nullptr);

    shared::PreviewPush pc{};
    pc.src = {rect[0], rect[1], rect[2], rect[3]};
    pc.dst = {dstWidth, dstHeight, 0u, 0u};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (dstWidth + 63) / 64, dstHeight, 1);   // rows of 64 (the shader's workgroup)

    // Visible to the host once the frame's fence signals.
    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = out;
    toHost.size = buffer.range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &toHost, 0, nullptr);
}

} // namespace ohao
