#include "render/rt/manifold_casters.hpp"
#include "render/shader_bindings/rt_manifold_gather.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"
#include "render/scene/render_scene.hpp"

#include <bit>
#include <cstring>
#include <unordered_set>

namespace ohao::render {

namespace {

constexpr uint32_t kRequestBytes = 3 * 16;
constexpr uint32_t kSlots = 128;   // a cluster's triangle slots (rt/manifold_gather.slang; kMeshletMaxTriangles 124)
constexpr uint32_t kTriBytes = 9 * 16;    // a triangle slot: 9 float4
constexpr uint32_t kNodeBytes = 4 * 16;   // a node (group, cluster, a draw's record): 4 float4

VkMemoryBarrier memoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    return b;
}

} // namespace

bool ManifoldCasters::allocate(Buffer& b, VkDeviceSize size) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = rtc::findMemoryType(m_physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, b.buffer, nullptr);
        b = Buffer{};
        return false;
    }
    b.size = size;
    return vkBindBufferMemory(m_device, b.buffer, b.memory, 0) == VK_SUCCESS;
}

void ManifoldCasters::release(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = Buffer{};
}

bool ManifoldCasters::create(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physical = physicalDevice;
    VkDescriptorSetLayoutBinding bindings[9];
    for (uint32_t i = 0; i < 9; ++i) bindings[i] = shader::layoutBinding(shader::rt_manifold_gather::kSet0[i]);
    static_assert(shader::rt_manifold_gather::kSet0.size() == 9);
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 9;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &ai, m_sets.data()) != VK_SUCCESS) return false;
    const bool ok = rtc::createComputePipeline(device, "rt_manifold_gather.slang.clusters.spv", "clusters", m_setLayout, 0,
                                               m_layout, m_clusterPipe) &&
                    rtc::createComputePipeline(device, "rt_manifold_gather.slang.groups.spv", "groups", m_setLayout, 0,
                                               m_groupLayout, m_groupPipe);
    if (!ok) {
        OHAO_LOG(Error, render, "manifold casters: the gather pipelines could not be created");
        return false;
    }
    // The smallest resources, so every binding is valid before anything is gathered.
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        if (!rtc::createHostBuffer(device, physicalDevice, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_draws[s])) return false;
        std::memset(m_draws[s].mapped, 0, 64);
        m_drawsBytes[s] = 64;
    }
    return allocate(m_tris, kTriBytes) && allocate(m_nodes, kNodeBytes) && allocate(m_requests, kRequestBytes);
}

void ManifoldCasters::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    release(m_tris);
    release(m_nodes);
    release(m_requests);
    for (auto& b : m_draws) rtc::destroyHostBuffer(m_device, b);
    for (VkPipeline* p : {&m_clusterPipe, &m_groupPipe})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_layout, &m_groupLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

void ManifoldCasters::prepare(uint32_t frameSlot, const RenderScene& scene, float cut) {
    if (!m_device || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    std::vector<ManifoldDrawInput> inputs;
    std::vector<glm::vec4> spheres;   // each draw's world bounding sphere (its mesh's box through its transform)
    for (const RenderScene::Draw& d : scene.draws()) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? d.aabbMax.x : d.aabbMin.x, (c & 2) ? d.aabbMax.y : d.aabbMin.y,
                                   (c & 4) ? d.aabbMax.z : d.aabbMin.z);
            const glm::vec3 w = d.world ? glm::vec3(*d.world * glm::vec4(corner, 1.0f)) : corner;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
        spheres.emplace_back(0.5f * (lo + hi), 0.5f * glm::length(hi - lo));
        ManifoldDrawInput in;
        in.firstIndex = d.firstIndex;
        in.triangles = d.indexCount / 3;
        in.vertexOffset = d.vertexOffset;
        in.slot = d.slot;
        in.material = scene.materialIndexFor(d.slot, d.materialSlot);
        in.skinned = d.skinned;
        in.emitter = d.emitter;
        in.viewModel = d.viewModel;
        in.firstMeshlet = d.firstMeshlet;
        in.meshlets = d.meshletCount;
        in.hasMeshlets = d.meshletCount > 0;
        inputs.push_back(in);
    }
    const ManifoldSelection sel =
        selectManifoldDraws(inputs, [&](uint32_t m) { return scene.materialSmoothness(m); }, cut);

    uint32_t D = static_cast<uint32_t>(sel.draws.size());
    const uint32_t K = gpu::shared::kManifoldCluster;
    std::vector<uint32_t> words(4 + 12 * D + ((sel.materialBits.size() + 3) / 4) * 4, 0u);
    uint32_t T = 0, C = 0, G = 0;
    // Step 2b walks a whole instance, so only the first draw of each (instance, material) runs it (word 7): a merged
    // model's runs of one glass are several draws of one instance, and each would add the instance's light again.
    std::unordered_set<uint64_t> glassOwners;
    for (uint32_t k = 0; k < D; ++k) {
        const ManifoldDrawInput& d = inputs[sel.draws[k]];
        const uint32_t clusters = d.meshlets, groups = (clusters + K - 1) / K;
        uint32_t* r = &words[4 + 12 * k];
        r[0] = d.firstMeshlet;
        r[1] = d.meshlets;
        r[2] = static_cast<uint32_t>(d.vertexOffset);
        r[3] = d.slot;
        r[4] = C;
        r[5] = G;
        r[6] = d.material;
        r[7] = glassOwners.insert((uint64_t{d.slot} << 32) | d.material).second ? 1u : 0u;
        const glm::vec4& sp = spheres[sel.draws[k]];
        for (int i = 0; i < 4; ++i) r[8 + i] = std::bit_cast<uint32_t>(sp[i]);
        C += clusters;
        G += groups;
    }
    T = C * kSlots;
    words[0] = D;
    words[1] = T;
    words[2] = C;
    words[3] = G;
    std::copy(sel.materialBits.begin(), sel.materialBits.end(), words.begin() + 4 + 12 * D);

    // Device buffers only grow (frames in flight read the old ones: the device idles once).
    const VkDeviceSize trisBytes = std::max<VkDeviceSize>(VkDeviceSize{T} * kTriBytes, kTriBytes);
    const VkDeviceSize nodesBytes = std::max<VkDeviceSize>(VkDeviceSize{C * 9 + G + D} * kNodeBytes, kNodeBytes);   // 8 sub-clusters a cluster
    // A failed growth keeps the pass off from then on (no idle and retry every frame).
    if (m_growthFailed) T = 0;
    else if (trisBytes > m_tris.size || nodesBytes > m_nodes.size) {
        vkDeviceWaitIdle(m_device);
        if (trisBytes > m_tris.size) {
            release(m_tris);
            if (!allocate(m_tris, trisBytes * 2)) T = 0;
        }
        if (nodesBytes > m_nodes.size) {
            release(m_nodes);
            if (!allocate(m_nodes, nodesBytes * 2)) T = 0;
        }
        if (T == 0) {
            OHAO_LOG(Error, render, "manifold casters: {} triangles could not be allocated; the pass stays off", words[1]);
            m_growthFailed = true;
            release(m_tris);   // whichever grew, or never failed, before the smallest pair
            release(m_nodes);
            allocate(m_tris, kTriBytes);
            allocate(m_nodes, kNodeBytes);
        }
    }
    if (T == 0) {
        words.assign(4, 0u);
        C = G = D = 0;
    }
    VkDeviceSize bytes = words.size() * sizeof(uint32_t);
    if (bytes > m_drawsBytes[frameSlot]) {
        rtc::destroyHostBuffer(m_device, m_draws[frameSlot]);
        const VkDeviceSize cap = std::max(bytes, m_drawsBytes[frameSlot] * 2);
        if (rtc::createHostBuffer(m_device, m_physical, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_draws[frameSlot])) {
            m_drawsBytes[frameSlot] = cap;
        } else {
            // The smallest buffer again, holding no draw: every binding stays valid and the pass does nothing.
            OHAO_LOG(Error, render, "manifold casters: a draws buffer of {} bytes could not be created; the pass stays off",
                     cap);
            rtc::destroyHostBuffer(m_device, m_draws[frameSlot]);
            m_drawsBytes[frameSlot] = 0;
            if (rtc::createHostBuffer(m_device, m_physical, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_draws[frameSlot]))
                m_drawsBytes[frameSlot] = 64;
            words.assign(4, 0u);
            bytes = words.size() * sizeof(uint32_t);
            T = C = G = D = 0;
        }
    }
    if (m_draws[frameSlot].mapped && bytes <= m_drawsBytes[frameSlot])
        std::memcpy(m_draws[frameSlot].mapped, words.data(), bytes);
    else
        T = C = G = D = 0;
    m_triangles = T;
    m_clusters = C;
    m_groups = G;
    m_drawCount = D;
}

bool ManifoldCasters::ensureRequests(uint32_t pixels) {
    if (m_device == VK_NULL_HANDLE) return false;
    if (m_requestsFull && m_requestPixels >= pixels) return true;
    vkDeviceWaitIdle(m_device);
    release(m_requests);
    const VkDeviceSize bytes = VkDeviceSize{pixels} * gpu::shared::kManifoldDepths * kRequestBytes;
    if (!allocate(m_requests, bytes)) {
        OHAO_LOG(Error, render, "manifold casters: requests for {} pixels ({} MB) could not be allocated; the pass stays off",
                 pixels, bytes >> 20);
        allocate(m_requests, kRequestBytes);
        m_requestsFull = false;
        m_requestPixels = 0;
        return false;
    }
    m_requestsFull = true;
    m_requestPixels = pixels;
    m_clearRequests = true;
    return true;
}

void ManifoldCasters::record(VkCommandBuffer cmd, uint32_t frameSlot, VkBuffer vertices, VkBuffer meshlets,
                             VkBuffer meshletVertices, VkBuffer meshletTriangles, VkBuffer instances, VkBuffer materials) {
    if (!m_device || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    constexpr VkPipelineStageFlags kTrace = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    // Last frame's trace read the triangles and requests; this frame rewrites them.
    VkMemoryBarrier before = memoryBarrier(kRW, kRW | VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, kTrace, kCompute | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    if (m_clearRequests && m_requestsFull) {
        // A request's meta word 0 is "none": a fresh buffer holds none (raygen writes every slot each frame after).
        vkCmdFillBuffer(cmd, m_requests.buffer, 0, VK_WHOLE_SIZE, 0u);
        m_clearRequests = false;
    }
    if (m_triangles > 0) {
        const VkDescriptorSet set = m_sets[frameSlot];
        const VkDescriptorBufferInfo infos[9] = {
            {vertices, 0, VK_WHOLE_SIZE},         {meshlets, 0, VK_WHOLE_SIZE},  {meshletVertices, 0, VK_WHOLE_SIZE},
            {meshletTriangles, 0, VK_WHOLE_SIZE}, {instances, 0, VK_WHOLE_SIZE}, {m_draws[frameSlot].buffer, 0, VK_WHOLE_SIZE},
            {m_tris.buffer, 0, VK_WHOLE_SIZE},    {m_nodes.buffer, 0, VK_WHOLE_SIZE}, {materials, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet w[9];
        for (uint32_t i = 0; i < 9; ++i) {
            w[i] = shader::write(shader::rt_manifold_gather::kSet0[i], set);
            w[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 9, w, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_clusterPipe);
        vkCmdDispatch(cmd, m_clusters, 1, 1);
        VkMemoryBarrier step = memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, kCompute, kCompute, 0, 1, &step, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_groupPipe);
        vkCmdDispatch(cmd, m_groups, 1, 1);
    }
    VkMemoryBarrier after = memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kRW);
    vkCmdPipelineBarrier(cmd, kCompute | VK_PIPELINE_STAGE_TRANSFER_BIT, kTrace, 0, 1, &after, 0, nullptr, 0, nullptr);
}

} // namespace ohao::render
