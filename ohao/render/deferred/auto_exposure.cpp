#include "render/deferred/auto_exposure.hpp"
#include "render/shader_bindings/postprocess_exposure.hpp"
#include "render/rt/rt_compute.hpp"

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
}  // namespace

bool AutoExposure::initialize(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    constexpr VkBufferUsageFlags kDeviceUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    m_bins = allocator.createBuffer(2 * kBins * sizeof(int32_t), kDeviceUsage, AllocationUsage::GpuOnly);
    m_state = allocator.createBuffer(4 * sizeof(float), kDeviceUsage, AllocationUsage::GpuOnly);
    if (!m_bins.isValid() || !m_state.isValid()) return false;
    for (GpuBuffer& b : m_readback) {
        b = allocator.createBuffer(4 * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, AllocationUsage::GpuToCpu, true);
        if (!b.isValid() || !b.getMappedData()) return false;
    }

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
    for (GpuBuffer& b : m_readback) m_allocator->retire(b);
    m_allocator->retire(m_bins);
    m_allocator->retire(m_state);
    m_device = VK_NULL_HANDLE;
}

void AutoExposure::record(const render::Frame& frame, gpu::Image& hdr, VkSampler sampler, uint32_t width,
                          uint32_t height, float dt, const Settings& settings) {
    const uint32_t slot = frame.slot;
    if (!m_histogram || !m_adapt || slot >= MAX_FRAMES_IN_FLIGHT || !hdr) return;
    const VkCommandBuffer cmd = frame.cmd;
    const VkDescriptorSet set = m_sets[slot];
    const VkDescriptorImageInfo image{sampler, hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo bins{m_bins.buffer, 0, VK_WHOLE_SIZE}, state{m_state.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3] = {shader::write(shader::postprocess_exposure::hdr, set),
                                 shader::write(shader::postprocess_exposure::bins, set),
                                 shader::write(shader::postprocess_exposure::state, set)};
    w[0].pImageInfo = &image;
    w[1].pBufferInfo = &bins;
    w[2].pBufferInfo = &state;
    vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);

    if (!m_cleared) {
        render::Pass clear(frame, nullptr, render::Pass::Transfer);
        clear.write(m_bins, render::BufferUse::CopyDst).write(m_state, render::BufferUse::CopyDst);
        clear.begin();
        vkCmdFillBuffer(cmd, m_bins.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, m_state.buffer, 0, VK_WHOLE_SIZE, 0);
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
    {
        render::Pass histogram(frame, nullptr, render::Pass::Compute);
        histogram.sampled(hdr);
        histogram.readWrite(m_bins, render::BufferUse::Storage);
        histogram.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_histogram);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
    }
    {
        render::Pass adapt(frame, nullptr, render::Pass::Compute);
        adapt.readWrite(m_bins, render::BufferUse::Storage).readWrite(m_state, render::BufferUse::Storage);
        adapt.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_adapt);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_adaptLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_adaptLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    {
        // This slot's copy is what render.stats reports.
        render::Pass copy(frame, nullptr, render::Pass::Transfer);
        copy.read(m_state, render::BufferUse::CopySrc).write(m_readback[slot], render::BufferUse::CopyDst);
        copy.begin();
        const VkBufferCopy region{0, 0, 4 * sizeof(float)};
        vkCmdCopyBuffer(cmd, m_state.buffer, m_readback[slot].buffer, 1, &region);
    }
    render::Pass toHost(frame, nullptr, render::Pass::Transfer);
    toHost.read(m_readback[slot], render::BufferUse::HostRead);
    toHost.begin();
    m_slotFrame[slot] = ++m_frames;
}

}  // namespace ohao
