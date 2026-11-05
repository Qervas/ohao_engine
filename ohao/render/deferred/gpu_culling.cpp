#include "gpu_culling.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao {

namespace shared = gpu::shared;

// maxPushConstantsSize is at least 128 on every Vulkan device.
static_assert(sizeof(shared::CullPush) <= 128);
static_assert(sizeof(shared::DrawRecord) == 64);

namespace {

constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkDeviceSize kCommandBytes = 5 * sizeof(uint32_t);   // VkDrawIndexedIndirectCommand, or a task dispatch + its draw
static_assert(kCommandBytes == sizeof(VkDrawIndexedIndirectCommand));
static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) <= kCommandBytes);
constexpr VkDeviceSize kMeshletCommandBytes = sizeof(VkDrawIndirectCommand);   // the fallback's
static_assert(kMeshletCommandBytes == 16);

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                   VkAccessFlags dstAccess) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

uint32_t blocksFor(uint32_t items) { return (items + shared::kCullGroupSize - 1) / shared::kCullGroupSize; }

} // namespace

GpuCulling::~GpuCulling() {
    cleanup();
}

bool GpuCulling::initialize(VkDevice device, VkPhysicalDevice physicalDevice, bool meshShaders) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_meshShaderSupported = meshShaders;
    if (meshShaders) {
        m_drawMeshTasksIndirectCount = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(
            vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksIndirectCountEXT"));
        if (!m_drawMeshTasksIndirectCount) {
            OHAO_LOG(Warn, render, "GpuCulling: vkCmdDrawMeshTasksIndirectCountEXT missing; meshlets use the fallback");
            m_meshShaderSupported = false;
        }
    }
    m_taskMeshStages =
        m_meshShaderSupported ? VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT : 0;
    using rtc::layoutBinding;

    // cull/instances.slang: bindings 0-11 (5 the pyramid, the rest storage buffers)
    std::array<VkDescriptorSetLayoutBinding, 12> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = layoutBinding(i, i == 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_FRAMES_IN_FLIGHT}};
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
    // The entries share the set layout and push range, so their layouts are compatible: one
    // is kept for binding, the others are destroyed with their pipelines.
    struct Entry { const char* spirv; const char* name; VkPipeline* pipeline; };
    const Entry entries[] = {
        {"cull_instances.slang.classify.spv", "classify", &m_classify},
        {"cull_instances.slang.scanBlocks.spv", "scanBlocks", &m_scanBlocks},
        {"cull_instances.slang.scatter.spv", "scatter", &m_scatter},
        {"cull_instances.slang.classifyMeshlets.spv", "classifyMeshlets", &m_classifyMeshlets},
        {"cull_instances.slang.scatterMeshlets.spv", "scatterMeshlets", &m_scatterMeshlets},
    };
    bool ok = true;
    for (const Entry& e : entries) {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        ok = ok && rtc::createComputePipeline(m_device, e.spirv, e.name, m_setLayout, sizeof(shared::CullPush), layout,
                                              *e.pipeline);
        // The layouts are identical, so the first binds them all. Every pipeline keeps the layout it was made with until teardown: NVIDIA's
        //  Linux driver (615.78) crashed binding a pipeline whose layout was destroyed, maintenance4 or not.
        if (m_layout == VK_NULL_HANDLE) m_layout = layout;
        else if (layout) m_extraLayouts.push_back(layout);
    }
    if (!ok) {
        OHAO_LOG(Error, render, "GpuCulling: culling pipelines failed");
        return false;
    }

    // cull/hiz.slang: 0 the depth (sampled), 1 the level written, 2 the level read
    const VkDescriptorSetLayoutBinding hiz[] = {layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                                layoutBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                                layoutBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    li.bindingCount = 3;
    li.pBindings = hiz;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_hizSetLayout) != VK_SUCCESS) return false;
    VkPipelineLayout downLayout = VK_NULL_HANDLE;
    const bool hizOk = rtc::createComputePipeline(m_device, "cull_hiz.slang.fromDepth.spv", "fromDepth", m_hizSetLayout,
                                                  sizeof(shared::HiZPush), m_hizLayout, m_fromDepth) &&
                       rtc::createComputePipeline(m_device, "cull_hiz.slang.downsample.spv", "downsample",
                                                  m_hizSetLayout, sizeof(shared::HiZPush), downLayout, m_downsample);
    if (downLayout) m_extraLayouts.push_back(downLayout);
    if (!hizOk) {
        OHAO_LOG(Error, render, "GpuCulling: Hi-Z pipelines failed");
        return false;
    }
    return true;
}

void GpuCulling::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    destroyBuffers();
    destroyHiZ();
    rtc::destroyHostBuffer(m_device, m_captureBuffer);
    m_captureBytes = 0;
    for (VkPipeline* p : {&m_classify, &m_scanBlocks, &m_scatter, &m_classifyMeshlets, &m_scatterMeshlets, &m_fromDepth,
                          &m_downsample})
        if (*p) vkDestroyPipeline(m_device, *p, nullptr), *p = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&m_layout, &m_hizLayout})
        if (*l) vkDestroyPipelineLayout(m_device, *l, nullptr), *l = VK_NULL_HANDLE;
    for (VkPipelineLayout l : m_extraLayouts) vkDestroyPipelineLayout(m_device, l, nullptr);
    m_extraLayouts.clear();
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_sets = {};
    for (VkDescriptorSetLayout* l : {&m_setLayout, &m_hizSetLayout})
        if (*l) vkDestroyDescriptorSetLayout(m_device, *l, nullptr), *l = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

bool GpuCulling::createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& out) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = usage;
    if (vkCreateBuffer(m_device, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, out.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(m_device, out.buffer, out.memory, 0) == VK_SUCCESS;
}

void GpuCulling::destroyBuffers() {
    for (Buffer* b : {&m_visibility, &m_local, &m_blockSums, &m_blockOffsets, &m_counts, &m_commands, &m_meshletLocal,
                      &m_meshletCommands}) {
        if (b->buffer) vkDestroyBuffer(m_device, b->buffer, nullptr);
        if (b->memory) vkFreeMemory(m_device, b->memory, nullptr);
        *b = Buffer{};
    }
    m_capacity = 0;
    m_entryCapacity = 0;
}

bool GpuCulling::ensureCapacity(uint32_t draws, uint32_t entries) {
    if (draws <= m_capacity && entries <= m_entryCapacity) return true;
    // A larger draw table than ever before (a scene load): frames in flight still use the
    // old buffers, so wait for them once.
    vkDeviceWaitIdle(m_device);
    const uint32_t oldDraws = m_capacity, oldEntries = m_entryCapacity;
    destroyBuffers();
    const auto grow = [](uint32_t cap, uint32_t need) {
        cap = std::max(cap, 1024u);
        while (cap < need) cap *= 2;
        return cap;
    };
    const uint32_t cap = grow(oldDraws, draws);
    // The per-meshlet buffers only matter to the fallback; a minimal set keeps the bindings valid.
    const uint32_t entryCap = grow(oldEntries, entries);
    const VkDeviceSize blocks = std::max(blocksFor(cap), blocksFor(entryCap));
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags kOut = kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const bool ok =
        createBuffer(VkDeviceSize{cap} * 4, kStorage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_visibility) &&
        createBuffer(VkDeviceSize{cap} * 16, kStorage, m_local) &&
        createBuffer(blocks * 16, kStorage, m_blockSums) && createBuffer(blocks * 16, kStorage, m_blockOffsets) &&
        createBuffer(VkDeviceSize{shared::kCullCounts} * 4, kOut | VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_counts) &&
        createBuffer(VkDeviceSize{shared::kCullLists} * cap * kCommandBytes, kOut, m_commands) &&
        createBuffer(VkDeviceSize{entryCap} * 8, kStorage, m_meshletLocal) &&
        createBuffer(VkDeviceSize{shared::kCullMeshletLists} * entryCap * kMeshletCommandBytes, kOut, m_meshletCommands);
    if (!ok) {
        OHAO_LOG(Error, render, "GpuCulling: buffers for {} draws, {} meshlets failed", cap, entryCap);
        destroyBuffers();
        return false;
    }
    m_capacity = cap;
    m_entryCapacity = entryCap;
    m_historyVersion = ~0ull;   // new buffers hold no history
    return true;
}

void GpuCulling::destroyHiZ() {
    for (VkImageView v : m_hizLevelViews)
        if (v) vkDestroyImageView(m_device, v, nullptr);
    m_hizLevelViews.clear();
    m_hizSizes.clear();
    rtc::destroyImage(m_device, m_hiz);
    if (m_hizPool) vkDestroyDescriptorPool(m_device, m_hizPool, nullptr);
    m_hizPool = VK_NULL_HANDLE;
    m_hizSets.clear();
    m_hizLevels = 0;
}

bool GpuCulling::resize(uint32_t width, uint32_t height, VkImageView depth) {
    if (m_device == VK_NULL_HANDLE) return false;
    destroyHiZ();
    m_width = width;
    m_height = height;
    m_depth = depth;
    // Level 0 is the frame halved, rounded up, then padded to powers of two: Vulkan sizes a
    // level by halving rounded down, so only power-of-two levels keep every texel's footprint
    // exact (level L texel t = pixels [t * 2^(L+1), (t + 1) * 2^(L+1))). The padding repeats
    // the frame's edge depth: no footprint over the frame loses a pixel.
    const auto pow2 = [](uint32_t v) {
        uint32_t p = 1;
        while (p < v) p *= 2;
        return p;
    };
    uint32_t w = pow2((width + 1) / 2), h = pow2((height + 1) / 2);
    for (;;) {
        m_hizSizes.push_back({w, h});
        if (w == 1 && h == 1) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    m_hizLevels = static_cast<uint32_t>(m_hizSizes.size());

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.extent = {m_hizSizes[0][0], m_hizSizes[0][1], 1};
    ii.mipLevels = m_hizLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ii, nullptr, &m_hiz.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, m_hiz.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &m_hiz.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_hiz.image, m_hiz.memory, 0);
    m_hiz.width = m_hizSizes[0][0];
    m_hiz.height = m_hizSizes[0][1];
    m_hiz.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // GENERAL at the first beginFrame
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_hiz.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_hizLevels, 0, 1};
    if (vkCreateImageView(m_device, &vi, nullptr, &m_hiz.view) != VK_SUCCESS) return false;
    m_hizLevelViews.resize(m_hizLevels, VK_NULL_HANDLE);
    for (uint32_t l = 0; l < m_hizLevels; ++l) {
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1};
        if (vkCreateImageView(m_device, &vi, nullptr, &m_hizLevelViews[l]) != VK_SUCCESS) return false;
    }

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_hizLevels},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * m_hizLevels}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = m_hizLevels;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_hizPool) != VK_SUCCESS) return false;
    m_hizSets.resize(m_hizLevels, VK_NULL_HANDLE);
    for (uint32_t l = 0; l < m_hizLevels; ++l) {
        VkDescriptorSetAllocateInfo sa{};
        sa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sa.descriptorPool = m_hizPool;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &m_hizSetLayout;
        if (vkAllocateDescriptorSets(m_device, &sa, &m_hizSets[l]) != VK_SUCCESS) return false;
        // Every binding written (the level-0 set's "level read" and the others' depth are
        // unused by their entry), so the sets are complete.
        const VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, m_hizLevelViews[l], VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, m_hizLevelViews[l == 0 ? 0 : l - 1], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3] = {rtc::descriptorWrite(m_hizSets[l], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                     rtc::descriptorWrite(m_hizSets[l], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                     rtc::descriptorWrite(m_hizSets[l], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
        w[0].pImageInfo = &depthInfo;
        w[1].pImageInfo = &dstInfo;
        w[2].pImageInfo = &srcInfo;
        vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
    }
    return true;
}

bool GpuCulling::beginFrame(VkCommandBuffer cmd, uint32_t slot, const render::RenderScene& scene) {
    m_set = VK_NULL_HANDLE;
    m_drawCount = 0;
    m_entryCount = 0;
    m_fallbackDrawn = false;
    if (m_hiz.image && m_hiz.layout == VK_IMAGE_LAYOUT_UNDEFINED) {   // every level, once
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_hiz.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_hizLevels, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, kCompute, 0, 0, nullptr, 0, nullptr, 1, &b);
        m_hiz.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const uint32_t n = scene.drawCount();
    const uint32_t entries = scene.meshletEntryCount();
    if (n == 0 || slot >= MAX_FRAMES_IN_FLIGHT || !m_hiz.image || scene.drawTable() == VK_NULL_HANDLE ||
        scene.instanceBuffer() == VK_NULL_HANDLE || scene.meshletArena() == VK_NULL_HANDLE)
        return false;
    // The per-meshlet buffers grow only for the fallback (task shaders never touch them).
    const bool fallback = !meshShadersActive() && m_settings.geometry != Geometry::Instances;
    if (!ensureCapacity(n, fallback ? entries : 0u)) return false;
    m_drawCount = n;
    m_entryCount = entries;

    // The history belongs to a draw table version: another list, or a temporal reset, starts
    // it again (the next early pass draws nothing).
    if (m_historyReset || m_historyVersion != scene.drawsVersion()) {
        memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, m_visibility.buffer, 0, VkDeviceSize{n} * 4, 0u);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        m_historyReset = false;
        m_historyVersion = scene.drawsVersion();
    }

    // The tables can be replaced (growth), so the slot's set is rewritten every frame; the
    // slot's previous use finished at its fence wait.
    m_set = m_sets[slot];
    const VkDescriptorBufferInfo infos[12] = {
        {scene.drawTable(), 0, VK_WHOLE_SIZE},       {scene.instanceBuffer(), 0, VK_WHOLE_SIZE},
        {m_visibility.buffer, 0, VK_WHOLE_SIZE},     {m_local.buffer, 0, VK_WHOLE_SIZE},
        {m_blockSums.buffer, 0, VK_WHOLE_SIZE},      {},
        {m_blockOffsets.buffer, 0, VK_WHOLE_SIZE},   {m_counts.buffer, 0, VK_WHOLE_SIZE},
        {m_commands.buffer, 0, VK_WHOLE_SIZE},       {scene.meshletArena(), 0, VK_WHOLE_SIZE},
        {m_meshletLocal.buffer, 0, VK_WHOLE_SIZE},   {m_meshletCommands.buffer, 0, VK_WHOLE_SIZE},
    };
    const VkDescriptorImageInfo hizInfo{VK_NULL_HANDLE, m_hiz.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 12> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = rtc::descriptorWrite(m_set, i, i == 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        if (i == 5) writes[i].pImageInfo = &hizInfo;
        else writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // A requested capture needs its host buffer before the frame records the copies.
    if (m_captureRequested) {
        const VkDeviceSize bytes = VkDeviceSize{shared::kCullCounts} * 4 + VkDeviceSize{n} * 4 +
                                   VkDeviceSize{shared::kCullLists} * n * kCommandBytes +
                                   VkDeviceSize{shared::kCullMeshletLists} * entries * kMeshletCommandBytes;
        if (bytes > m_captureBytes) {
            rtc::destroyHostBuffer(m_device, m_captureBuffer);
            m_captureBytes = 0;
            if (rtc::createHostBuffer(m_device, m_physicalDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_captureBuffer))
                m_captureBytes = bytes;
        }
    }
    return true;
}

uint32_t GpuCulling::meshletFlags(uint32_t phase) const noexcept {
    if (!m_settings.frustum) return 0u;
    uint32_t flags = shared::kCullFrustum | shared::kCullCone;
    if (m_settings.occlusion && phase == shared::kCullPhaseLate) flags |= shared::kCullOcclusion;
    return flags;
}

void GpuCulling::cull(VkCommandBuffer cmd, uint32_t phase, uint32_t firstList, uint32_t listCount,
                      const glm::mat4& viewProj, const glm::vec3& camera) {
    if (m_drawCount == 0 || m_set == VK_NULL_HANDLE) return;
    const uint32_t blocks = blocksFor(m_drawCount);
    // Earlier work on these buffers: the last phase's or frame's draws read the lists (the
    // task shaders read the commands too), and its culling wrote them (queue order; never a
    // wait).
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | m_taskMeshStages | kCompute |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    uint32_t flags = 0;
    if (m_settings.frustum) flags |= shared::kCullFrustum;
    if (m_settings.frustum && m_settings.occlusion && phase == shared::kCullPhaseLate) flags |= shared::kCullOcclusion;
    shared::CullPush pc{};
    std::memcpy(&pc.viewProj, &viewProj, sizeof(viewProj));
    pc.config = {m_drawCount, phase, flags, m_drawCount};
    pc.hiz = {m_width, m_height, m_hizLevels, blocks};
    pc.camera = {camera.x, camera.y, camera.z, 0.0f};
    pc.extra = {firstList, listCount, static_cast<uint32_t>(m_settings.geometry), 0u};

    const auto dispatch = [&](VkPipeline pipeline, uint32_t groups, const shared::CullPush& push) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, 1, 1);
    };
    const auto between = [&] {
        memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    dispatch(m_classify, blocks, pc);
    between();
    dispatch(m_scanBlocks, 1, pc);
    between();
    dispatch(m_scatter, blocks, pc);
    const bool gbufferPhase = phase == shared::kCullPhaseEarly || phase == shared::kCullPhaseLate;
    if (gbufferPhase && listCount == 4 && fallbackRuns()) {
        between();
        cullMeshlets(cmd, phase, pc);
    }
    memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | m_taskMeshStages | kCompute,
                  VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    if (phase == shared::kCullPhaseLate && m_captureRequested) recordCapture(cmd);
}

void GpuCulling::cullMeshlets(VkCommandBuffer cmd, uint32_t phase, const shared::CullPush& drawPush) {
    // Every meshlet entry against the phase's meshlet lists (the draw scan's `local`), then
    // the shared block scan into the fallback's two lists for the phase, then their commands.
    shared::CullPush pc = drawPush;
    const uint32_t blocks = blocksFor(m_entryCount);
    const uint32_t meshletList = phase == shared::kCullPhaseLate ? 2u : 0u;
    pc.config = {m_entryCount, phase, meshletFlags(phase), m_drawCount};
    pc.hiz[3] = blocks;
    pc.extra = {shared::kCullLists + meshletList, 2u, drawPush.extra[2], 0u};
    const auto dispatch = [&](VkPipeline pipeline, uint32_t groups) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, groups, 1, 1);
    };
    const auto between = [&] {
        memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    dispatch(m_classifyMeshlets, blocks);
    between();
    dispatch(m_scanBlocks, 1);
    between();
    dispatch(m_scatterMeshlets, blocks);
    m_fallbackDrawn = true;
}

void GpuCulling::buildHiZ(VkCommandBuffer cmd) {
    if (m_drawCount == 0 || m_hizLevels == 0) return;
    // The early pass's depth writes before the reads; the last cull's and task shaders'
    // pyramid reads before the writes.
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | kCompute |
                      m_taskMeshStages,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, kCompute,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    for (uint32_t l = 0; l < m_hizLevels; ++l) {
        shared::HiZPush pc{};
        const uint32_t srcW = l == 0 ? m_width : m_hizSizes[l - 1][0];
        const uint32_t srcH = l == 0 ? m_height : m_hizSizes[l - 1][1];
        pc.size = {m_hizSizes[l][0], m_hizSizes[l][1], srcW, srcH};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l == 0 ? m_fromDepth : m_downsample);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizLayout, 0, 1, &m_hizSets[l], 0, nullptr);
        vkCmdPushConstants(cmd, m_hizLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (pc.size[0] + 7) / 8, (pc.size[1] + 7) / 8, 1);
        memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT);
    }
}

void GpuCulling::drawList(VkCommandBuffer cmd, uint32_t list) const {
    if (m_drawCount == 0 || list >= shared::kCullLists) return;
    vkCmdDrawIndexedIndirectCount(cmd, m_commands.buffer, VkDeviceSize{list} * m_drawCount * kCommandBytes,
                                  m_counts.buffer, VkDeviceSize{list} * 4, m_drawCount,
                                  static_cast<uint32_t>(kCommandBytes));
}

void GpuCulling::drawMeshTasks(VkCommandBuffer cmd, uint32_t list) const {
    if (m_drawCount == 0 || list >= shared::kCullLists || !meshShadersActive()) return;
    m_drawMeshTasksIndirectCount(cmd, m_commands.buffer, VkDeviceSize{list} * m_drawCount * kCommandBytes,
                                 m_counts.buffer, VkDeviceSize{list} * 4, m_drawCount,
                                 static_cast<uint32_t>(kCommandBytes));
}

void GpuCulling::drawMeshletFallback(VkCommandBuffer cmd, uint32_t meshletList) const {
    if (!m_fallbackDrawn || m_entryCount == 0 || meshletList >= shared::kCullMeshletLists) return;
    vkCmdDrawIndirectCount(cmd, m_meshletCommands.buffer, VkDeviceSize{meshletList} * m_entryCount * kMeshletCommandBytes,
                           m_counts.buffer, VkDeviceSize{shared::kCullLists + meshletList} * 4, m_entryCount,
                           static_cast<uint32_t>(kMeshletCommandBytes));
}

void GpuCulling::recordCapture(VkCommandBuffer cmd) {
    m_captured = false;
    const uint32_t n = m_drawCount;
    const uint32_t e = m_fallbackDrawn ? m_entryCount : 0u;
    const VkDeviceSize countBytes = VkDeviceSize{shared::kCullCounts} * 4, visBytes = VkDeviceSize{n} * 4,
                       cmdBytes = VkDeviceSize{shared::kCullLists} * n * kCommandBytes,
                       meshletBytes = VkDeviceSize{shared::kCullMeshletLists} * e * kMeshletCommandBytes;
    if (!m_captureBuffer.buffer || countBytes + visBytes + cmdBytes + meshletBytes > m_captureBytes) return;
    memoryBarrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy counts{0, 0, countBytes}, vis{0, countBytes, visBytes}, cmds{0, countBytes + visBytes, cmdBytes},
        meshletCmds{0, countBytes + visBytes + cmdBytes, meshletBytes};
    vkCmdCopyBuffer(cmd, m_counts.buffer, m_captureBuffer.buffer, 1, &counts);
    vkCmdCopyBuffer(cmd, m_visibility.buffer, m_captureBuffer.buffer, 1, &vis);
    vkCmdCopyBuffer(cmd, m_commands.buffer, m_captureBuffer.buffer, 1, &cmds);
    if (meshletBytes) vkCmdCopyBuffer(cmd, m_meshletCommands.buffer, m_captureBuffer.buffer, 1, &meshletCmds);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_HOST_READ_BIT);
    m_capturedDraws = n;
    m_capturedEntries = e;
    m_captured = true;
}

GpuCulling::Capture GpuCulling::capture() const {
    Capture c;
    if (!m_captured || !m_captureBuffer.mapped) return c;
    const uint32_t n = m_capturedDraws, e = m_capturedEntries;
    const auto* words = static_cast<const uint32_t*>(m_captureBuffer.mapped);
    c.drawCount = n;
    c.meshletEntries = e;
    c.counts.assign(words, words + shared::kCullCounts);
    words += shared::kCullCounts;
    c.visibility.assign(words, words + n);
    words += n;
    c.commands.assign(words, words + std::size_t{shared::kCullLists} * n * 5);
    words += std::size_t{shared::kCullLists} * n * 5;
    c.meshletCommands.assign(words, words + std::size_t{shared::kCullMeshletLists} * e * 4);
    return c;
}

} // namespace ohao
