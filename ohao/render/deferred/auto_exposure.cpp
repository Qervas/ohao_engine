#include "render/deferred/auto_exposure.hpp"
#include "render/shader_bindings/postprocess_exposure.hpp"

#include "core/log.hpp"

namespace ohao {

namespace {
constexpr uint32_t kBins = 128;   // exposure.slang's kExposureBins

struct ExposureParams {
    glm::uvec2 size;
    float minLog2, rangeLog2;
    float lowFraction, highFraction;
    float compensation, dt;
    float speedUp, speedDown;
    uint32_t reset, pad;
};
OHAO_CHECK_PUSH(ExposureParams, postprocess_exposure)   // postprocess/exposure.slang's

bool createDeviceBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size, VkBuffer& buffer,
                        VkDeviceMemory& memory) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buffer, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = rtc::findMemoryType(physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

VkMemoryBarrier memoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    return b;
}
}  // namespace

bool AutoExposure::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    if (!createDeviceBuffer(device, physicalDevice, 2 * kBins * sizeof(int32_t), m_bins, m_binsMemory) ||
        !createDeviceBuffer(device, physicalDevice, 4 * sizeof(float), m_state, m_stateMemory))
        return false;
    for (rtc::HostBuffer& b : m_readback)
        if (!rtc::createHostBuffer(device, physicalDevice, 4 * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, b))
            return false;

    const VkDescriptorSetLayoutBinding bindings[] = {
        shader::layoutBinding(shader::postprocess_exposure::hdr),
        shader::layoutBinding(shader::postprocess_exposure::bins),
        shader::layoutBinding(shader::postprocess_exposure::state),
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &ai, m_sets.data()) != VK_SUCCESS) return false;

    if (!rtc::createComputePipeline(device, "postprocess_exposure.slang.histogram.spv", "histogram", m_setLayout,
                                    sizeof(ExposureParams), m_layout, m_histogram) ||
        !rtc::createComputePipeline(device, "postprocess_exposure.slang.adapt.spv", "adapt", m_setLayout,
                                    sizeof(ExposureParams), m_adaptLayout, m_adapt)) {
        OHAO_LOG(Error, render, "auto exposure: the pipelines could not be created");
        return false;
    }
    return true;
}

void AutoExposure::cleanup() {
    if (!m_device) return;
    for (VkPipeline* p : {&m_histogram, &m_adapt})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_layout, &m_adaptLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_pool) { vkDestroyDescriptorPool(m_device, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
    if (m_setLayout) { vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr); m_setLayout = VK_NULL_HANDLE; }
    for (rtc::HostBuffer& b : m_readback) rtc::destroyHostBuffer(m_device, b);
    if (m_bins) { vkDestroyBuffer(m_device, m_bins, nullptr); m_bins = VK_NULL_HANDLE; }
    if (m_binsMemory) { vkFreeMemory(m_device, m_binsMemory, nullptr); m_binsMemory = VK_NULL_HANDLE; }
    if (m_state) { vkDestroyBuffer(m_device, m_state, nullptr); m_state = VK_NULL_HANDLE; }
    if (m_stateMemory) { vkFreeMemory(m_device, m_stateMemory, nullptr); m_stateMemory = VK_NULL_HANDLE; }
    m_device = VK_NULL_HANDLE;
}

void AutoExposure::record(VkCommandBuffer cmd, uint32_t slot, VkImageView hdr, VkSampler sampler, uint32_t width,
                          uint32_t height, float dt, const Settings& settings) {
    if (!m_histogram || !m_adapt || slot >= MAX_FRAMES_IN_FLIGHT || hdr == VK_NULL_HANDLE) return;
    const VkDescriptorSet set = m_sets[slot];
    const VkDescriptorImageInfo image{sampler, hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo bins{m_bins, 0, VK_WHOLE_SIZE}, state{m_state, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3] = {shader::write(shader::postprocess_exposure::hdr, set),
                                 shader::write(shader::postprocess_exposure::bins, set),
                                 shader::write(shader::postprocess_exposure::state, set)};
    w[0].pImageInfo = &image;
    w[1].pBufferInfo = &bins;
    w[2].pBufferInfo = &state;
    vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);

    // The frame's colour writes before the histogram reads them; last frame's display pass read
    // the state and its adaptation and copy wrote or read it before this one writes it.
    const VkMemoryBarrier before =
        memoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
    if (!m_cleared) {
        vkCmdFillBuffer(cmd, m_bins, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, m_state, 0, VK_WHOLE_SIZE, 0);
        const VkMemoryBarrier filled =
            memoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0,
                             nullptr, 0, nullptr);
        m_cleared = true;
    }

    const ExposureParams pc{glm::uvec2(width, height),
                            settings.minLog2,
                            settings.rangeLog2,
                            settings.lowFraction,
                            settings.highFraction,
                            settings.compensation,
                            dt,
                            settings.speedUp,
                            settings.speedDown,
                            m_reset ? 1u : 0u,
                            0u};
    m_reset = false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_histogram);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    const VkMemoryBarrier binned =
        memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &binned,
                         0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_adapt);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_adaptLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_adaptLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, 1, 1, 1);

    // The display pass reads the EV100; this slot's copy is what render.stats reports.
    const VkMemoryBarrier metered =
        memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &metered, 0,
                         nullptr, 0, nullptr);
    const VkBufferCopy copy{0, 0, 4 * sizeof(float)};
    vkCmdCopyBuffer(cmd, m_state, m_readback[slot].buffer, 1, &copy);
    const VkMemoryBarrier toHost = memoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0,
                         nullptr);
    m_slotFrame[slot] = ++m_frames;
}

}  // namespace ohao
