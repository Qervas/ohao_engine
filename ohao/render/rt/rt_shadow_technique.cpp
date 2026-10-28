#include "rt_shadow_technique.hpp"
#include "render/scene/render_scene_types.hpp"
#include "core/log.hpp"
#include <cstring>
#include <algorithm>
#include <tuple>
#include <utility>

namespace ohao {

namespace shared = gpu::shared;

namespace {

using rtc::descriptorWrite;
using rtc::layoutBinding;
using rtc::transition;

std::array<float, 4> f4(float x, float y, float z, float w) { return {x, y, z, w}; }
std::array<float, 4> f4(glm::vec3 v, float w) { return {v.x, v.y, v.z, w}; }
std::array<float, 4> f4(float v) { return {v, v, v, v}; }
std::array<uint32_t, 4> u4(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return {x, y, z, w}; }

// A light as the trace samples it (see ShadowLight in shaders/shared/layout.json).
shared::ShadowLight toGpu(const render::LightDesc& d) {
    shared::ShadowLight g{};
    switch (d.kind) {
    case render::LightKind::Directional: {   // radius is the angular radius in radians
        const glm::vec3 toLight = -glm::normalize(d.direction);
        g.positionAndType = f4(d.direction, 0.0f);
        g.edge1AndRadius = f4(0.0f, 0.0f, 0.0f, d.radius);
        g.edge2AndRange = f4(0.0f);
        g.centreAndIntensity = f4(toLight, d.intensity);
        break;
    }
    case render::LightKind::AreaRect: {      // position is a corner, as the path tracer samples it
        const glm::vec3 half = 0.5f * (d.areaEdge1 + d.areaEdge2);
        g.positionAndType = f4(d.position, 2.0f);
        g.edge1AndRadius = f4(d.areaEdge1, glm::length(half));
        g.edge2AndRange = f4(d.areaEdge2, 0.0f);
        g.centreAndIntensity = f4(d.position + half, d.intensity);
        break;
    }
    default: {                               // Point, Spot: a sphere of `radius`
        // A spot's direction (edge1 xyz) and outer-cone cosine (edge2 x): its importance is 0
        // outside the cone. A point light's edge2 x is -2: no cone.
        const bool spot = d.kind == render::LightKind::Spot;
        g.positionAndType = f4(d.position, 1.0f);
        g.edge1AndRadius = f4(spot ? glm::normalize(d.direction) : glm::vec3(0.0f), d.radius);
        g.edge2AndRange = f4(spot ? std::cos(glm::radians(d.outerConeDeg)) : -2.0f, 0.0f, 0.0f, d.range);
        g.centreAndIntensity = f4(d.position, d.intensity);
        break;
    }
    }
    return g;
}

constexpr VkFormat kMetaFormat = VK_FORMAT_R32G32_UINT;
constexpr VkFormat kVisFormat = VK_FORMAT_R32G32_UINT;   // eight 8-bit visibility channels
constexpr VkFormat kRefFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkImageUsageFlags kScreenUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

} // namespace

RTShadowTechnique::~RTShadowTechnique() {
    destroy();
}

bool RTShadowTechnique::init(VkDevice device, VkPhysicalDevice physicalDevice,
                              uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;

    if (!createScreenImages() || !createImage(m_refDummy, kRefFormat, VK_IMAGE_USAGE_STORAGE_BIT, 1, 1)) {
        OHAO_LOG(Error, render, "RTShadow: failed to create its images");
        return false;
    }
    if (!createSlots()) {
        OHAO_LOG(Error, render, "RTShadow: failed to create its light buffers");
        return false;
    }
    if (!createDescriptorResources()) {
        OHAO_LOG(Error, render, "RTShadow: failed to create descriptor resources");
        return false;
    }
    if (!createTracePipeline()) {
        OHAO_LOG(Error, render, "RTShadow: failed to create the trace pipeline");
        return false;
    }
    if (!createFilterPipeline()) {
        OHAO_LOG(Error, render, "RTShadow: failed to create the filter pipeline");
        return false;
    }

    OHAO_LOG(Info, render, "RTShadow: initialized ({}x{})", width, height);
    return true;
}

bool RTShadowTechnique::createImage(Image& img, VkFormat format, VkImageUsageFlags usage, uint32_t width,
                                    uint32_t height) {
    return rtc::createImage(m_device, m_physicalDevice, img, format, usage, width, height);
}

void RTShadowTechnique::destroyImage(Image& img) {
    rtc::destroyImage(m_device, img);
}

bool RTShadowTechnique::createScreenImages() {
    for (Image& m : m_meta)
        if (!createImage(m, kMetaFormat, kScreenUsage, m_width, m_height)) return false;
    for (Image& v : m_vis)
        if (!createImage(v, kVisFormat, kScreenUsage, m_width, m_height)) return false;
    m_historyValid = false;
    return true;
}

void RTShadowTechnique::destroyScreenImages() {
    for (Image& i : m_meta) destroyImage(i);
    for (Image& i : m_vis) destroyImage(i);
    destroyImage(m_refLo);
    destroyImage(m_refHi);
}

bool RTShadowTechnique::ensureReferenceImages() {
    if (m_refLo.image) return true;
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return createImage(m_refLo, kRefFormat, usage, m_width, m_height) &&
           createImage(m_refHi, kRefFormat, usage, m_width, m_height);
}

bool RTShadowTechnique::createSlots() {
    // Every pixel reads the lights: rtc::HostBuffer prefers device-local memory the host maps.
    for (Slot& s : m_slots)
        if (!rtc::createHostBuffer(m_device, m_physicalDevice, kCounterOffset + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.buf))
            return false;
    return true;
}

bool RTShadowTechnique::createDescriptorResources() {
    constexpr VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding trace[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, comp),
        layoutBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, comp),    // meta
        layoutBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),    // G-buffer position
        layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),    // G-buffer normal
        layoutBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, comp),   // lights
        layoutBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, comp),   // dropped-pixel counter
        layoutBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, comp),    // reference mean, lights 0-3
        layoutBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, comp),    // reference mean, lights 4-7
        layoutBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, comp),    // visibility out (reference)
    };
    const VkDescriptorSetLayoutBinding filter[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),   // meta, this frame
        layoutBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),   // meta, last frame
        layoutBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),   // velocity
        layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, comp),   // history: last frame's visibility
        layoutBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, comp),   // visibility out
    };
    for (auto [bindings, count, layout] :
         {std::tuple{trace, 9u, &m_traceSetLayout}, std::tuple{filter, 5u, &m_filterSetLayout}}) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = count;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, layout) != VK_SUCCESS) return false;
    }

    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (4 + 1) * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (2 + 4) * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2 * MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;

    for (auto [layout, sets] : {std::pair{m_traceSetLayout, &m_traceSets}, std::pair{m_filterSetLayout, &m_filterSets}}) {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
        layouts.fill(layout);
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = m_descriptorPool;
        setInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        setInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(m_device, &setInfo, sets->data()) != VK_SUCCESS) return false;
    }
    return true;
}

bool RTShadowTechnique::createTracePipeline() {
    if (!m_alphaTablesLayout || !m_texturesLayout) return false;
    const VkDescriptorSetLayout sets[] = {m_traceSetLayout, m_alphaTablesLayout, m_texturesLayout};
    return rtc::createComputePipeline(m_device, "rt_shadow_trace.slang.spv", "trace", sets,
                                      sizeof(shared::ShadowTracePush), m_tracePipelineLayout, m_tracePipeline);
}

bool RTShadowTechnique::createFilterPipeline() {
    return rtc::createComputePipeline(m_device, "rt_shadow_filter.slang.spv", "filter", m_filterSetLayout,
                                      sizeof(shared::ShadowFilterPush), m_filterPipelineLayout, m_filterPipeline);
}

void RTShadowTechnique::collectSlot(uint32_t slot) {
    Slot& s = m_slots[slot];
    if (!s.pending) return;
    uint32_t pixels = 0;
    std::memcpy(&pixels, static_cast<const uint8_t*>(s.buf.mapped) + kCounterOffset, sizeof(pixels));
    if (!m_latestDropped || s.frame >= m_latestDropped->frame) m_latestDropped = Dropped{s.frame, pixels};
    s.pending = false;
}

void RTShadowTechnique::render(VkCommandBuffer cmd, const ShadowInput& input) {
    m_rendered = false;
    if (!m_tracePipeline || input.tlas == VK_NULL_HANDLE) return;
    if (input.alphaTables == VK_NULL_HANDLE || input.textures == VK_NULL_HANDLE) return;
    if (input.lights.empty()) return;   // no light casts shadows: nothing reads the visibility
    if (input.frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    const bool reference = input.quality == RenderQuality::Reference;
    m_quality = input.quality;
    m_lightCount = static_cast<uint32_t>(std::min<std::size_t>(input.lights.size(), shared::kShadowMaxLights));
    m_frameIndex = input.frameIndex;
    m_renderSeed = input.renderSeed;
    m_slot = input.frameSlot;
    m_velocityView = input.velocityBuffer;
    if (reference && !ensureReferenceImages()) return;
    if (!reference && m_velocityView == VK_NULL_HANDLE) return;

    // This slot's frame has finished (the renderer waited on its fence): read its counter,
    // then this frame's lights and a zeroed counter.
    collectSlot(m_slot);
    Slot& slot = m_slots[m_slot];
    auto* lights = static_cast<shared::ShadowLight*>(slot.buf.mapped);
    for (uint32_t i = 0; i < m_lightCount; ++i) lights[i] = toGpu(input.lights[i]);
    std::memset(static_cast<uint8_t*>(slot.buf.mapped) + kCounterOffset, 0, sizeof(uint32_t));
    slot.pending = !reference;
    slot.frame = input.frameNumber;

    // Realtime: this frame writes the other half of each pair, and breaks a reference run;
    // reference keeps none of the realtime history.
    if (input.frameIndex == 0) m_refFrames = 0;   // a temporal reset
    if (!reference) {
        m_cur ^= 1u;
        m_refFrames = 0;
    } else {
        m_historyValid = false;
    }
    Image& meta = m_meta[m_cur];
    Image& vis = m_vis[m_cur];   // written by this frame's trace (reference) or filter (realtime)
    for (Image* img : {&meta, &vis})
        transition(cmd, *img, VK_IMAGE_LAYOUT_GENERAL, kReaders, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true);
    if (m_refDummy.layout != VK_IMAGE_LAYOUT_GENERAL)
        transition(cmd, m_refDummy, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, true);
    if (reference) {
        // The running means read last frame's while the reference frames run unbroken; new
        // images hold none of them.
        if (m_refLo.layout == VK_IMAGE_LAYOUT_UNDEFINED) m_refFrames = 0;
        for (Image* ref : {&m_refLo, &m_refHi})
            transition(cmd, *ref, VK_IMAGE_LAYOUT_GENERAL, kReaders, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, m_refFrames == 0);
    }

    const VkDescriptorSet set = m_traceSets[m_slot];
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    const VkAccelerationStructureKHR tlas = input.tlas;
    asWrite.pAccelerationStructures = &tlas;
    const VkDescriptorImageInfo metaInfo{VK_NULL_HANDLE, meta.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo posInfo{VK_NULL_HANDLE, input.positionBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo normalInfo{VK_NULL_HANDLE, input.normalBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightsInfo{slot.buf.buffer, 0, kLightBytes};
    const VkDescriptorBufferInfo counterInfo{slot.buf.buffer, kCounterOffset, 16};
    const VkDescriptorImageInfo refLoInfo{VK_NULL_HANDLE, reference ? m_refLo.view : m_refDummy.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo refHiInfo{VK_NULL_HANDLE, reference ? m_refHi.view : m_refDummy.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo visInfo{VK_NULL_HANDLE, vis.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[9] = {
        descriptorWrite(set, 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR), descriptorWrite(set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        descriptorWrite(set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),              descriptorWrite(set, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        descriptorWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),             descriptorWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        descriptorWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),              descriptorWrite(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        descriptorWrite(set, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
    };
    writes[0].pNext = &asWrite;
    writes[1].pImageInfo = &metaInfo;
    writes[2].pImageInfo = &posInfo;
    writes[3].pImageInfo = &normalInfo;
    writes[4].pBufferInfo = &lightsInfo;
    writes[5].pBufferInfo = &counterInfo;
    writes[6].pImageInfo = &refLoInfo;
    writes[7].pImageInfo = &refHiInfo;
    writes[8].pImageInfo = &visInfo;
    vkUpdateDescriptorSets(m_device, 9, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
    const VkDescriptorSet sets[] = {set, input.alphaTables, input.textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipelineLayout, 0, 3, sets, 0, nullptr);
    shared::ShadowTracePush pc{};
    pc.extent = u4(input.width, input.height, m_lightCount,
                           std::min(input.maxLightsPerPixel, shared::kShadowLightsPerPixel));
    pc.seed = u4(input.renderSeed, input.frameIndex, reference ? 1u : 0u,
                         reference ? shared::kShadowReferenceRays : 1u);
    pc.cameraPos = f4(input.cameraPos, 0.0f);
    pc.history = u4(reference ? m_refFrames : 0u, 0u, 0u, 0u);
    vkCmdPushConstants(cmd, m_tracePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (input.width + 7) / 8, (input.height + 7) / 8, 1);
    m_rendered = true;
    if (reference) ++m_refFrames;

    if (reference) {
        // The means stay in GENERAL for the next frame's trace; lighting reads the packed copy.
        transition(cmd, vis, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    } else {
        transition(cmd, meta, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    }
}

void RTShadowTechnique::filter(VkCommandBuffer cmd) {
    if (!filters()) return;
    const uint32_t prev = m_cur ^ 1u;
    // History: none on the first frame after a reset, or before this pair holds a frame.
    if (m_frameIndex == 0) m_historyValid = false;

    // Last frame's halves are read as sampled images; one never written gets a layout only.
    // This frame's visibility is in GENERAL since render().
    for (Image* h : {&m_meta[prev], &m_vis[prev]})
        if (h->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            transition(cmd, *h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, true);

    const VkDescriptorSet set = m_filterSets[m_slot];
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo infos[5] = {
        {VK_NULL_HANDLE, m_meta[m_cur].view, ro},
        {VK_NULL_HANDLE, m_meta[prev].view, ro},
        {VK_NULL_HANDLE, m_velocityView, ro},
        {VK_NULL_HANDLE, m_vis[prev].view, ro},
        {VK_NULL_HANDLE, m_vis[m_cur].view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[5];
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i] = descriptorWrite(set, i, i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_filterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_filterPipelineLayout, 0, 1, &set, 0, nullptr);
    shared::ShadowFilterPush pc{};
    pc.extent = u4(m_width, m_height, m_lightCount, m_historyValid ? m_historyFrames : 0u);
    pc.seed = u4(m_renderSeed, m_frameIndex, 0u, 0u);
    vkCmdPushConstants(cmd, m_filterPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    const uint32_t tile = shared::kShadowFilterTile;
    vkCmdDispatch(cmd, (m_width + tile - 1) / tile, (m_height + tile - 1) / tile, 1);

    transition(cmd, m_vis[m_cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, false);
    m_historyFrames = m_historyValid ? m_historyFrames + 1 : 1;
    m_historyValid = true;
}

void RTShadowTechnique::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    destroyScreenImages();
    createScreenImages();   // the reference means come back on first use
}

ShadowOutput RTShadowTechnique::getOutput() const {
    return {m_vis[m_cur].view};
}

void RTShadowTechnique::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    if (m_tracePipeline) { vkDestroyPipeline(m_device, m_tracePipeline, nullptr); m_tracePipeline = VK_NULL_HANDLE; }
    if (m_filterPipeline) { vkDestroyPipeline(m_device, m_filterPipeline, nullptr); m_filterPipeline = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_tracePipelineLayout, &m_filterPipelineLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    for (VkDescriptorSetLayout* l : {&m_traceSetLayout, &m_filterSetLayout})
        if (*l) { vkDestroyDescriptorSetLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    destroyScreenImages();
    destroyImage(m_refDummy);
    for (Slot& s : m_slots) {
        rtc::destroyHostBuffer(m_device, s.buf);
        s = Slot{};
    }
    m_traceSets = {};
    m_filterSets = {};
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
