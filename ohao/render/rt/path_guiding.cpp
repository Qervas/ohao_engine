#include "render/rt/path_guiding.hpp"

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

// The tracer's raygen reads and writes the buffers; the update passes are compute.
constexpr VkPipelineStageFlags kTrace = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
             VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

} // namespace

bool PathGuiding::allocate(Buffer& b, VkDeviceSize size, VkBufferUsageFlags extraUsage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage;
    if (vkCreateBuffer(m_device, &info, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, b.buffer, nullptr);
        b = Buffer{};
        return false;
    }
    b.size = size;
    return vkBindBufferMemory(m_device, b.buffer, b.memory, 0) == VK_SUCCESS;
}

void PathGuiding::release(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = Buffer{};
}

bool PathGuiding::create(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
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
    VkDescriptorSetLayoutBinding bindings[5];
    for (uint32_t i = 0; i < 5; ++i) bindings[i] = rtc::layoutBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(device, &setInfo, &m_set) != VK_SUCCESS) {
        destroy();
        return false;
    }
    writeSet();
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

void PathGuiding::writeSet() {
    const VkDescriptorBufferInfo infos[5] = {{m_states.buffer, 0, VK_WHOLE_SIZE}, {m_next.buffer, 0, VK_WHOLE_SIZE},
                                             {m_claims.buffer, 0, VK_WHOLE_SIZE},
                                             {m_proposals.buffer, 0, VK_WHOLE_SIZE},
                                             {m_pixel.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[5];
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i] = rtc::descriptorWrite(m_set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
}

bool PathGuiding::ensure(uint32_t pixels) {
    if (m_device == VK_NULL_HANDLE) return false;
    if (ready(pixels)) return true;
    // The placeholders or smaller buffers may be in flight: nothing is replaced under the GPU.
    vkDeviceWaitIdle(m_device);
    constexpr VkBufferUsageFlags kCopy = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!m_full) {
        release(m_states);
        release(m_next);
        release(m_claims);
        const VkDeviceSize cap = shared::kGuideCapacity;
        if (!allocate(m_states, cap * kStateBytes, kCopy) || !allocate(m_next, cap * kStateBytes, kCopy) ||
            !allocate(m_claims, cap * kClaimBytes, 0)) {
            OHAO_LOG(Error, render, "PathGuiding: the grid ({} states) could not be allocated; guiding stays off", cap);
            release(m_states);
            release(m_next);
            release(m_claims);
            allocate(m_states, kStateBytes, kCopy);
            allocate(m_next, kStateBytes, kCopy);
            allocate(m_claims, kClaimBytes, 0);
            writeSet();
            return false;
        }
        m_full = true;
        m_clearPending = true;
    }
    if (m_pixels < pixels) {
        release(m_proposals);
        release(m_pixel);
        if (!allocate(m_proposals, VkDeviceSize(pixels) * shared::kGuideTrainVertices * kProposalBytes, 0) ||
            !allocate(m_pixel, VkDeviceSize(pixels) * kPixelBytes, 0)) {
            OHAO_LOG(Error, render, "PathGuiding: buffers for {} pixels could not be allocated; guiding stays off", pixels);
            release(m_proposals);
            release(m_pixel);
            allocate(m_proposals, kProposalBytes, 0);
            allocate(m_pixel, kPixelBytes, 0);
            m_pixels = 0;
            writeSet();
            return false;
        }
        m_pixels = pixels;
    }
    writeSet();
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
    for (Buffer* b : {&m_states, &m_next, &m_claims, &m_proposals, &m_pixel}) release(*b);
    *this = PathGuiding{};
}

void PathGuiding::recordBeginFrame(VkCommandBuffer cmd) {
    if (!m_full) return;
    if (!m_clearPending) {
        // The last frame's copy into the read buffer, before this frame's trace reads it.
        barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTrace, VK_ACCESS_SHADER_READ_BIT);
        return;
    }
    m_clearPending = false;
    barrier(cmd, kTrace | kCompute | kTransfer, kRW | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, m_states.buffer, 0, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, m_next.buffer, 0, VK_WHOLE_SIZE, 0u);
    barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTrace | kCompute, kRW);
}

void PathGuiding::recordLearn(VkCommandBuffer cmd, uint32_t pixels, uint32_t frame, const glm::vec3& camera) {
    if (!ready(pixels)) return;
    // The claims start at 0 every frame; the trace's proposals and pixel words are complete.
    vkCmdFillBuffer(cmd, m_claims.buffer, 0, VK_WHOLE_SIZE, 0u);
    barrier(cmd, kTrace | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, kRW);
    shared::GuidePush push{};
    push.camera = {camera.x, camera.y, camera.z, 0.0f};
    const uint32_t count = pixels * shared::kGuideTrainVertices;
    push.config = {frame, count, 0u, std::bit_cast<uint32_t>(m_prior)};
    const uint32_t groups = (count + shared::kGuideGroupSize - 1u) / shared::kGuideGroupSize;
    for (uint32_t pass = 0; pass < 3; ++pass) {
        if (pass > 0) barrier(cmd, kCompute, kRW, kCompute, kRW);
        push.config[2] = pass;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[pass]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layouts[pass], 0, 1, &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layouts[pass], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, 1, 1);
    }
    // What this frame learned becomes what the next reads (both buffers equal again). The read
    // buffer's readers this frame (the trace, the update pass) are done before the copy.
    barrier(cmd, kCompute | kTrace, kRW, kTransfer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferCopy region{0, 0, m_next.size};
    vkCmdCopyBuffer(cmd, m_next.buffer, m_states.buffer, 1, &region);
}

} // namespace ohao::render
