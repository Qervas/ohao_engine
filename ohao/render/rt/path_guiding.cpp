#include "render/rt/path_guiding.hpp"
#include "render/shader_bindings/rt_path_guiding_update.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"
#include "render/rt/rt_compute.hpp"

#include <bit>
#include <cstdlib>
#include <string>

namespace ohao::render {

namespace shared = gpu::shared;

namespace {

constexpr VkDeviceSize kStateBytes = shared::kGuideStateWords * 4u;
constexpr VkDeviceSize kProposalBytes = shared::kGuideProposalWords * 4u;
constexpr VkDeviceSize kClaimBytes = 8u;
constexpr VkDeviceSize kPixelBytes = 16u;
constexpr const char* kSpirv = "rt_path_guiding_update.slang.";
constexpr const char* kEntries[3] = {"update", "owner", "write"};

} // namespace

bool PathGuiding::allocate(GpuBuffer& b, VkDeviceSize size, VkBufferUsageFlags extraUsage) {
    m_allocator->retire(b);
    b = m_allocator->createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage,
                                  AllocationUsage::GpuOnly);
    return b.isValid();
}

bool PathGuiding::create(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;
    if (const char* s = std::getenv("OHAO_GUIDE_PROB")) m_share = std::strtof(s, nullptr);
    if (const char* s = std::getenv("OHAO_GUIDE_PRIOR")) m_prior = std::strtof(s, nullptr);
    if (const char* s = std::getenv("OHAO_GUIDE_DEPTHS")) m_depths = uint32_t(std::strtoul(s, nullptr, 10));
    // Placeholders: one element each, bound until guiding is first used.
    constexpr VkBufferUsageFlags kCopy = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!allocate(m_states, kStateBytes, kCopy) || !allocate(m_next, kStateBytes, kCopy) ||
        !allocate(m_claims, kClaimBytes, 0) || !allocate(m_proposals, kProposalBytes, 0) ||
        !allocate(m_pixel, kPixelBytes, 0)) {
        OHAO_LOG(Error, render, "PathGuiding: placeholder buffers could not be created");
        destroy();
        return false;
    }
    m_stateBytes = kStateBytes;
    VkDescriptorSetLayoutBinding bindings[5];
    for (uint32_t i = 0; i < 5; ++i) bindings[i] = shader::layoutBinding(shader::rt_path_guiding_update::kSet0[i]);
    static_assert(shader::rt_path_guiding_update::kSet0.size() == 5);
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> setLayouts;
    setLayouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    setInfo.pSetLayouts = setLayouts.data();
    if (vkAllocateDescriptorSets(device, &setInfo, m_sets.data()) != VK_SUCCESS) {
        destroy();
        return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
        const std::string spirv = std::string(kSpirv) + kEntries[i] + ".spv";
        if (!rtc::createComputePipeline(device, spirv.c_str(), kEntries[i], m_setLayout, sizeof(shared::GuidePush),
                                        m_layouts[i], m_pipelines[i])) {
            destroy();
            return false;
        }
    }
    m_clearPending = true;
    return true;
}

bool PathGuiding::ensure(uint32_t pixels) {
    if (m_device == VK_NULL_HANDLE) return false;
    if (ready(pixels)) return true;
    constexpr VkBufferUsageFlags kCopy = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!m_full) {
        const VkDeviceSize cap = shared::kGuideCapacity;
        if (!allocate(m_states, cap * kStateBytes, kCopy) || !allocate(m_next, cap * kStateBytes, kCopy) ||
            !allocate(m_claims, cap * kClaimBytes, 0)) {
            OHAO_LOG(Error, render, "PathGuiding: the grid ({} states) could not be allocated; guiding stays off", cap);
            allocate(m_states, kStateBytes, kCopy);
            allocate(m_next, kStateBytes, kCopy);
            allocate(m_claims, kClaimBytes, 0);
            m_stateBytes = kStateBytes;
            return false;
        }
        m_stateBytes = cap * kStateBytes;
        m_full = true;
        m_clearPending = true;
    }
    if (m_pixels < pixels) {
        if (!allocate(m_proposals, VkDeviceSize(pixels) * shared::kGuideTrainVertices * kProposalBytes, 0) ||
            !allocate(m_pixel, VkDeviceSize(pixels) * kPixelBytes, 0)) {
            OHAO_LOG(Error, render, "PathGuiding: buffers for {} pixels could not be allocated; guiding stays off", pixels);
            allocate(m_proposals, kProposalBytes, 0);
            allocate(m_pixel, kPixelBytes, 0);
            m_pixels = 0;
            return false;
        }
        m_pixels = pixels;
    }
    OHAO_LOG(Info, render, "PathGuiding: a grid of {} states and buffers for {} pixels", shared::kGuideCapacity, m_pixels);
    return true;
}

void PathGuiding::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < 3; ++i) {
        if (m_pipelines[i]) vkDestroyPipeline(m_device, m_pipelines[i], nullptr);
        if (m_layouts[i]) vkDestroyPipelineLayout(m_device, m_layouts[i], nullptr);
    }
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    for (GpuBuffer* b : {&m_states, &m_next, &m_claims, &m_proposals, &m_pixel}) m_allocator->retire(*b);
    *this = PathGuiding{};
}

void PathGuiding::tracedBy(Pass& pass) {
    pass.read(m_states, BufferUse::Storage).write(m_proposals, BufferUse::Storage).write(m_pixel, BufferUse::Storage);
}

void PathGuiding::recordBeginFrame(const Frame& frame) {
    if (!m_full || !m_clearPending) return;
    m_clearPending = false;
    Pass pass(frame, nullptr, Pass::Transfer);
    pass.write(m_states, BufferUse::CopyDst).write(m_next, BufferUse::CopyDst);
    pass.begin();
    vkCmdFillBuffer(frame.cmd, m_states.buffer, 0, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(frame.cmd, m_next.buffer, 0, VK_WHOLE_SIZE, 0u);
}

void PathGuiding::recordLearn(const Frame& frame, uint32_t pixels, uint32_t sample, const glm::vec3& camera) {
    if (!ready(pixels) || frame.slot >= MAX_FRAMES_IN_FLIGHT) return;
    const VkCommandBuffer cmd = frame.cmd;
    // This slot's set (its fence was waited): the buffers as they are this frame.
    const VkDescriptorSet set = m_sets[frame.slot];
    const VkDescriptorBufferInfo infos[5] = {{m_states.buffer, 0, VK_WHOLE_SIZE}, {m_next.buffer, 0, VK_WHOLE_SIZE},
                                             {m_claims.buffer, 0, VK_WHOLE_SIZE},
                                             {m_proposals.buffer, 0, VK_WHOLE_SIZE},
                                             {m_pixel.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[5];
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i] = shader::write(shader::rt_path_guiding_update::kSet0[i], set);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    {
        // The claims start at 0 every frame; the trace's proposals and pixel words are complete.
        Pass pass(frame, nullptr, Pass::Transfer);
        pass.write(m_claims, BufferUse::CopyDst);
        pass.begin();
        vkCmdFillBuffer(cmd, m_claims.buffer, 0, VK_WHOLE_SIZE, 0u);
    }
    shared::GuidePush push{};
    push.camera = {camera.x, camera.y, camera.z, 0.0f};
    const uint32_t count = pixels * shared::kGuideTrainVertices;
    push.config = {sample, count, 0u, std::bit_cast<uint32_t>(m_prior)};
    const uint32_t groups = (count + shared::kGuideGroupSize - 1u) / shared::kGuideGroupSize;
    for (uint32_t step = 0; step < 3; ++step) {
        Pass pass(frame, nullptr, Pass::Compute);
        pass.read(m_states, BufferUse::Storage).read(m_pixel, BufferUse::Storage);
        pass.write(m_next, BufferUse::Storage).write(m_claims, BufferUse::Storage).write(m_proposals, BufferUse::Storage);
        pass.begin();
        push.config[2] = step;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[step]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layouts[step], 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layouts[step], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, 1, 1);
    }
    // What this frame learned becomes what the next reads (both buffers equal again).
    Pass pass(frame, nullptr, Pass::Transfer);
    pass.read(m_next, BufferUse::CopySrc).write(m_states, BufferUse::CopyDst);
    pass.begin();
    const VkBufferCopy region{0, 0, m_stateBytes};
    vkCmdCopyBuffer(cmd, m_next.buffer, m_states.buffer, 1, &region);
}

} // namespace ohao::render
