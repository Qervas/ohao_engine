#include "rt_gi_technique.hpp"
#include "render/scene/render_scene_types.hpp"
#include "core/log.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <vector>

namespace ohao {

namespace shared = gpu::shared;

// The trace reads a hit's instance row (its mesh and materials) straight from the instance table.
static_assert(sizeof(render::InstanceGpu) == shared::kInstanceGpuStride);

namespace {

using rtc::descriptorWrite;
using rtc::layoutBinding;

constexpr VkFormat kRadianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGuideFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kRefFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kFragment = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

std::array<float, 4> f4(glm::vec3 v, float w) { return {v.x, v.y, v.z, w}; }
std::array<float, 4> f4(float x, float y, float z, float w) { return {x, y, z, w}; }
std::array<uint32_t, 4> u4(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return {x, y, z, w}; }

// A light as the bounce sees it (see GILight in shaders/shared/layout.json): attenuated as the
// lighting pass attenuates it; a rect light as a point at its centre.
shared::GILight toGpu(const render::LightDesc& d) {
    shared::GILight g{};
    const glm::vec3 dir = glm::length(d.direction) > 0.0f ? glm::normalize(d.direction) : glm::vec3(0, -1, 0);
    g.radiance = f4(d.color * d.intensity, 0.0f);
    switch (d.kind) {
    case render::LightKind::Directional:
        g.positionAndType = f4(d.position, 0.0f);
        g.directionAndRange = f4(dir, 0.0f);
        break;
    case render::LightKind::Spot:
        g.positionAndType = f4(d.position, 2.0f);
        g.directionAndRange = f4(dir, d.range);
        g.cone = f4(std::cos(glm::radians(d.innerConeDeg)), std::cos(glm::radians(d.outerConeDeg)), 0.0f, 0.0f);
        break;
    case render::LightKind::AreaRect: {
        // Seen from afar, the path tracer's rectangle: radiance Le = I / (4 pi r^2) over its area A,
        // emitting toward edge1 x edge2 with intensity Le A cos(theta), a spot of cones 0 and 90
        // degrees (its linear ramp is cos(theta)). No range: the path tracer applies none.
        const glm::vec3 n = glm::cross(d.areaEdge1, d.areaEdge2);
        const float area = glm::length(n);
        const float r = std::max(d.radius, 0.01f);
        const float le = d.intensity / (4.0f * glm::pi<float>() * r * r);
        g.radiance = f4(d.color * (le * area), 0.0f);
        g.positionAndType = f4(d.position + 0.5f * (d.areaEdge1 + d.areaEdge2), 2.0f);
        g.directionAndRange = f4(area > 0.0f ? n / area : dir, 1.0e9f);
        g.cone = f4(1.0f, 0.0f, 0.0f, 0.0f);
        break;
    }
    default:   // Point
        g.positionAndType = f4(d.position, 1.0f);
        g.directionAndRange = f4(dir, d.range);
        break;
    }
    return g;
}

// A full memory dependency between compute dispatches (and from earlier frames' readers).
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, src, kCompute, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// Vose's alias table over the lights' power, in GILight.cone.zw (slot i keeps itself with
// probability z, else takes light w), and each light's probability in radiance.w. Deterministic:
// the small and large lists are filled and taken in index order.
void buildAliasTable(std::vector<shared::GILight>& lights, const std::vector<double>& power) {
    const size_t n = lights.size();
    if (n == 0) return;
    const double total = std::accumulate(power.begin(), power.end(), 0.0);
    std::vector<double> scaled(n);
    std::vector<uint32_t> small, large;
    for (size_t i = 0; i < n; ++i) {
        lights[i].radiance[3] = static_cast<float>(power[i] / total);
        scaled[i] = power[i] / total * static_cast<double>(n);
        (scaled[i] < 1.0 ? small : large).push_back(static_cast<uint32_t>(i));
    }
    size_t s = 0, l = 0;
    while (s < small.size() && l < large.size()) {
        const uint32_t a = small[s++], b = large[l];
        lights[a].cone[2] = static_cast<float>(scaled[a]);
        std::memcpy(&lights[a].cone[3], &b, sizeof(b));
        scaled[b] -= 1.0 - scaled[a];
        if (scaled[b] < 1.0) {
            ++l;
            small.push_back(b);
        }
    }
    // What is left keeps itself (rounding leaves some just under or over 1).
    for (; s < small.size(); ++s) {
        lights[small[s]].cone[2] = 1.0f;
        std::memcpy(&lights[small[s]].cone[3], &small[s], sizeof(uint32_t));
    }
    for (; l < large.size(); ++l) {
        lights[large[l]].cone[2] = 1.0f;
        std::memcpy(&lights[large[l]].cone[3], &large[l], sizeof(uint32_t));
    }
}

} // namespace

RTGITechnique::~RTGITechnique() {
    destroy();
}

bool RTGITechnique::init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;

    const bool dummies = rtc::createImage(device, physicalDevice, m_guideDummy, kGuideFormat, VK_IMAGE_USAGE_STORAGE_BIT, 1, 1) &&
                         rtc::createImage(device, physicalDevice, m_refDummy, kRefFormat, VK_IMAGE_USAGE_STORAGE_BIT, 1, 1) &&
                         rtc::createImage(device, physicalDevice, m_envDummy, kRadianceFormat, VK_IMAGE_USAGE_SAMPLED_BIT, 1, 1);
    if (!dummies || !createScreenImages()) {
        OHAO_LOG(Error, render, "RTGI: failed to create its images");
        return false;
    }
    if (!m_openpbrTables.create(device, physicalDevice)) {
        OHAO_LOG(Error, render, "RTGI: failed to upload OpenPBR's energy tables");
        return false;
    }
    if (!m_cache.create(device, physicalDevice, kCacheCapacity, kCacheMaxSamples)) {
        OHAO_LOG(Error, render, "RTGI: failed to create its radiance cache");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_lightCapacity[i] = shared::kGIWeighedLights;
        if (!rtc::createHostBuffer(device, physicalDevice, sizeof(shared::GILight) * m_lightCapacity[i],
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_lights[i])) {
            OHAO_LOG(Error, render, "RTGI: failed to create its light buffers");
            return false;
        }
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;          // longitude wraps
    si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &si, nullptr, &m_envSampler) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "RTGI: failed to create its environment sampler");
        return false;
    }
    if (!createDescriptorResources()) {
        OHAO_LOG(Error, render, "RTGI: failed to create descriptor resources");
        return false;
    }
    if (!createPipelines()) {
        OHAO_LOG(Error, render, "RTGI: failed to create its pipelines");
        return false;
    }
    OHAO_LOG(Info, render, "RTGI: initialized ({}x{}, traced at {}x{})", width, height, m_halfWidth, m_halfHeight);
    return true;
}

bool RTGITechnique::createScreenImages() {
    m_halfWidth = m_shift ? (m_width + 1) / 2 : m_width;
    m_halfHeight = m_shift ? (m_height + 1) / 2 : m_height;
    const auto half = [&](Image& img, VkFormat format) {
        return rtc::createImage(m_device, m_physicalDevice, img, format, kUsage, m_halfWidth, m_halfHeight);
    };
    bool ok = half(m_noisy, kRadianceFormat) && half(m_atrous, kRadianceFormat);
    for (uint32_t i = 0; i < 2 && ok; ++i)
        ok = half(m_guide[i], kGuideFormat) && half(m_fast[i], kRadianceFormat) && half(m_slow[i], kRadianceFormat);
    ok = ok && rtc::createImage(m_device, m_physicalDevice, m_output, kRadianceFormat, kUsage, m_width, m_height);
    m_halfReady = false;
    m_historyValid = false;
    return ok;
}

void RTGITechnique::destroyScreenImages() {
    for (Image* img : {&m_noisy, &m_atrous, &m_output, &m_refMean}) rtc::destroyImage(m_device, *img);
    for (uint32_t i = 0; i < 2; ++i) {
        rtc::destroyImage(m_device, m_guide[i]);
        rtc::destroyImage(m_device, m_fast[i]);
        rtc::destroyImage(m_device, m_slow[i]);
    }
}

bool RTGITechnique::ensureReferenceImage() {
    return m_refMean.image ||
           rtc::createImage(m_device, m_physicalDevice, m_refMean, kRefFormat, VK_IMAGE_USAGE_STORAGE_BIT, m_width, m_height);
}

bool RTGITechnique::createDescriptorResources() {
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const std::vector<VkDescriptorSetLayoutBinding> trace = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
        layoutBinding(1, sampled), layoutBinding(2, sampled), layoutBinding(3, sampled),   // G-buffer
        layoutBinding(4, buffer),                                                          // instance table
        layoutBinding(5, buffer),                                                          // lights
        layoutBinding(6, sampled), layoutBinding(7, VK_DESCRIPTOR_TYPE_SAMPLER),           // environment
        layoutBinding(8, storage), layoutBinding(9, storage), layoutBinding(10, storage),  // radiance, guide, mean
        layoutBinding(11, buffer),                                                         // OpenPBR's tables
        layoutBinding(12, buffer), layoutBinding(13, buffer), layoutBinding(14, buffer),   // the radiance cache
    };
    const std::vector<VkDescriptorSetLayoutBinding> temporal = {
        layoutBinding(0, sampled), layoutBinding(1, sampled), layoutBinding(2, sampled),
        layoutBinding(3, sampled), layoutBinding(4, sampled), layoutBinding(5, sampled),
        layoutBinding(6, storage), layoutBinding(7, storage),
    };
    const std::vector<VkDescriptorSetLayoutBinding> atrous = {
        layoutBinding(8, sampled), layoutBinding(9, sampled), layoutBinding(10, storage),
    };
    const std::vector<VkDescriptorSetLayoutBinding> upsample = {
        layoutBinding(11, sampled), layoutBinding(12, sampled), layoutBinding(13, sampled),
        layoutBinding(14, sampled), layoutBinding(15, sampled), layoutBinding(16, storage),
    };
    for (auto [bindings, layout] : {std::pair{&trace, &m_traceSetLayout}, std::pair{&temporal, &m_temporalSetLayout},
                                    std::pair{&atrous, &m_atrousSetLayout}, std::pair{&upsample, &m_upsampleSetLayout}}) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(bindings->size());
        li.pBindings = bindings->data();
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, layout) != VK_SUCCESS) return false;
    }

    // Per frame slot: trace, temporal, 2 a-trous, upsample.
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * MAX_FRAMES_IN_FLIGHT},
        {sampled, (4 + 6 + 2 * 2 + 5) * MAX_FRAMES_IN_FLIGHT},
        {storage, (3 + 2 + 2 * 1 + 1) * MAX_FRAMES_IN_FLIGHT},
        {buffer, 6 * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1 * MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 5 * MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;

    const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        return vkAllocateDescriptorSets(m_device, &ai, out) == VK_SUCCESS;
    };
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        if (!allocate(m_traceSetLayout, &m_traceSets[s]) || !allocate(m_temporalSetLayout, &m_temporalSets[s]) ||
            !allocate(m_atrousSetLayout, &m_atrousSets[s][0]) || !allocate(m_atrousSetLayout, &m_atrousSets[s][1]) ||
            !allocate(m_upsampleSetLayout, &m_upsampleSets[s]))
            return false;
    }
    return true;
}

bool RTGITechnique::createPipelines() {
    if (!m_alphaTablesLayout || !m_texturesLayout) return false;
    const VkDescriptorSetLayout traceSets[] = {m_traceSetLayout, m_alphaTablesLayout, m_texturesLayout};
    return rtc::createComputePipeline(m_device, "rt_gi_trace.slang.spv", "trace", traceSets,
                                      sizeof(shared::GITracePush), m_traceLayout, m_tracePipeline) &&
           rtc::createComputePipeline(m_device, "rt_gi_denoise.slang.temporal.spv", "temporal", m_temporalSetLayout,
                                      sizeof(shared::GIDenoisePush), m_temporalLayout, m_temporalPipeline) &&
           rtc::createComputePipeline(m_device, "rt_gi_denoise.slang.atrous.spv", "atrous", m_atrousSetLayout,
                                      sizeof(shared::GIDenoisePush), m_atrousLayout, m_atrousPipeline) &&
           rtc::createComputePipeline(m_device, "rt_gi_denoise.slang.upsample.spv", "upsample", m_upsampleSetLayout,
                                      sizeof(shared::GIDenoisePush), m_upsampleLayout, m_upsamplePipeline);
}

void RTGITechnique::prepareHalfImages(VkCommandBuffer cmd) {
    if (m_halfReady) return;
    for (Image* img : {&m_noisy, &m_atrous, &m_guide[0], &m_guide[1], &m_fast[0], &m_fast[1], &m_slow[0], &m_slow[1]})
        rtc::transition(cmd, *img, kGeneral, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, kCompute,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, true);
    m_halfReady = true;
    m_historyValid = false;   // nothing written yet
}

void RTGITechnique::render(VkCommandBuffer cmd, const GIInput& input) {
    m_rendered = false;
    if (!m_tracePipeline || input.tlas == VK_NULL_HANDLE || input.instances == VK_NULL_HANDLE) return;
    if (input.alphaTables == VK_NULL_HANDLE || input.textures == VK_NULL_HANDLE) return;
    if (input.frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    const bool reference = input.quality == RenderQuality::Reference;
    if (!reference && input.velocityBuffer == VK_NULL_HANDLE) return;
    if (reference && !ensureReferenceImage()) return;
    m_quality = input.quality;
    m_slot = input.frameSlot;
    m_velocityView = input.velocityBuffer;
    m_positionView = input.positionBuffer;
    m_normalView = input.normalBuffer;
    m_albedoView = input.albedoBuffer;
    m_cameraPos = input.cameraPos;
    if (input.frameIndex == 0) resetHistory();   // a temporal reset: no history

    // The bounce's lights: every light that gives any, flashes out (an effect's light, raster only),
    // in scene order, each with its share of power (the luminance of colour times intensity) and the
    // alias table the trace draws candidates by.
    std::vector<shared::GILight> bounce;
    std::vector<double> power;
    for (const render::LightDesc& d : input.lights) {
        if (d.kind == render::LightKind::Flash) continue;
        const shared::GILight g = toGpu(d);
        const double p = 0.2126 * g.radiance[0] + 0.7152 * g.radiance[1] + 0.0722 * g.radiance[2];
        if (!(p > 0.0)) continue;
        bounce.push_back(g);
        power.push_back(p);
    }
    buildAliasTable(bounce, power);
    const uint32_t lightCount = static_cast<uint32_t>(bounce.size());
    if (lightCount > m_lightCapacity[m_slot]) {
        // This slot's previous frame has finished: its buffer can go.
        uint32_t capacity = m_lightCapacity[m_slot];
        while (capacity < lightCount) capacity *= 2;
        rtc::destroyHostBuffer(m_device, m_lights[m_slot]);
        if (!rtc::createHostBuffer(m_device, m_physicalDevice, sizeof(shared::GILight) * capacity,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_lights[m_slot])) {
            OHAO_LOG(Error, render, "RTGI: failed to grow its light buffer to {} lights", capacity);
            m_lightCapacity[m_slot] = 0;
            return;
        }
        m_lightCapacity[m_slot] = capacity;
    }
    if (lightCount > 0)
        std::memcpy(m_lights[m_slot].mapped, bounce.data(), sizeof(shared::GILight) * lightCount);

    // Earlier frames' readers of these images (the denoiser, lighting) finish first.
    barrier(cmd, kCompute | kFragment);
    if (reference) {
        m_historyValid = false;
        m_traceExtent = {m_width, m_height};
        m_rays = shared::kGIReferenceRays;
        rtc::transition(cmd, m_output, kGeneral, kFragment, VK_ACCESS_SHADER_READ_BIT, kCompute,
                        VK_ACCESS_SHADER_WRITE_BIT, true);
        // The running mean reads last frame's while the reference frames run unbroken; a new
        // image holds none of them. A run that starts again starts the cache again too: the same
        // reference frames after any history.
        if (m_refMean.layout == VK_IMAGE_LAYOUT_UNDEFINED) m_refFrames = 0;
        if (m_refFrames == 0) m_cache.clear();
        rtc::transition(cmd, m_refMean, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        kCompute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, m_refFrames == 0);
    } else {
        m_refFrames = 0;   // a realtime frame breaks the reference run
        m_cur ^= 1u;
        prepareHalfImages(cmd);
        m_traceExtent = {m_halfWidth, m_halfHeight};
        m_rays = m_realtimeRays;
    }
    for (Image* d : {&m_guideDummy, &m_refDummy})
        if (d->layout != kGeneral)
            rtc::transition(cmd, *d, kGeneral, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, kCompute, 0, true);
    if (m_envDummy.layout != kReadOnly)
        rtc::transition(cmd, m_envDummy, kReadOnly, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, kCompute, 0, true);

    const VkDescriptorSet set = m_traceSets[m_slot];
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    const VkAccelerationStructureKHR tlas = input.tlas;
    asWrite.pAccelerationStructures = &tlas;
    const VkDescriptorImageInfo gbuf[3] = {{VK_NULL_HANDLE, input.positionBuffer, kReadOnly},
                                           {VK_NULL_HANDLE, input.normalBuffer, kReadOnly},
                                           {VK_NULL_HANDLE, input.albedoBuffer, kReadOnly}};
    const VkDescriptorBufferInfo instancesInfo{input.instances, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lightsInfo{m_lights[m_slot].buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo envInfo{VK_NULL_HANDLE, input.envMap ? input.envMap : m_envDummy.view, kReadOnly};
    const VkDescriptorImageInfo samplerInfo{m_envSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, reference ? m_output.view : m_noisy.view, kGeneral};
    const VkDescriptorImageInfo guideInfo{VK_NULL_HANDLE, reference ? m_guideDummy.view : m_guide[m_cur].view, kGeneral};
    const VkDescriptorImageInfo meanInfo{VK_NULL_HANDLE, reference ? m_refMean.view : m_refDummy.view, kGeneral};
    const VkDescriptorBufferInfo tablesInfo{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo cacheInfo[3] = {{m_cache.keys(), 0, VK_WHOLE_SIZE},
                                                 {m_cache.accum(), 0, VK_WHOLE_SIZE},
                                                 {m_cache.resolved(), 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[15] = {
        descriptorWrite(set, 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
        descriptorWrite(set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), descriptorWrite(set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        descriptorWrite(set, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        descriptorWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), descriptorWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        descriptorWrite(set, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), descriptorWrite(set, 7, VK_DESCRIPTOR_TYPE_SAMPLER),
        descriptorWrite(set, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), descriptorWrite(set, 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        descriptorWrite(set, 10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        descriptorWrite(set, 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        descriptorWrite(set, 12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), descriptorWrite(set, 13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        descriptorWrite(set, 14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    };
    writes[0].pNext = &asWrite;
    for (uint32_t i = 0; i < 3; ++i) writes[1 + i].pImageInfo = &gbuf[i];
    writes[4].pBufferInfo = &instancesInfo;
    writes[5].pBufferInfo = &lightsInfo;
    writes[6].pImageInfo = &envInfo;
    writes[7].pImageInfo = &samplerInfo;
    writes[8].pImageInfo = &outInfo;
    writes[9].pImageInfo = &guideInfo;
    writes[10].pImageInfo = &meanInfo;
    writes[11].pBufferInfo = &tablesInfo;
    for (uint32_t i = 0; i < 3; ++i) writes[12 + i].pBufferInfo = &cacheInfo[i];
    vkUpdateDescriptorSets(m_device, 15, writes, 0, nullptr);

    // The cache: last frame's work ordered before this frame's, emptied if a reset asked.
    m_cache.recordBeginFrame(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
    const VkDescriptorSet traceSets[] = {set, input.alphaTables, input.textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_traceLayout, 0, 3, traceSets, 0, nullptr);
    shared::GITracePush pc{};
    pc.extent = u4(m_traceExtent.width, m_traceExtent.height, m_width, m_height);
    pc.seed = u4(input.renderSeed, input.frameIndex, m_rays, lightCount);
    pc.flags = u4(reference ? 1u : 0u, (input.envMap ? 1u : 0u) | (input.emissionIsDirect ? 2u : 0u),
                  reference ? m_refFrames : 0u, m_shift);
    pc.cameraPos = f4(input.cameraPos, input.envIntensity);
    vkCmdPushConstants(cmd, m_traceLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (m_traceExtent.width + 7) / 8, (m_traceExtent.height + 7) / 8, 1);
    m_cache.recordResolve(cmd);   // this frame's samples blended in for the next frame's rays
    m_rendered = true;
    if (reference) ++m_refFrames;

    if (reference) {
        rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kFragment,
                        VK_ACCESS_SHADER_READ_BIT, false);
    } else {
        barrier(cmd, kCompute);   // the denoiser reads the trace
    }
}

void RTGITechnique::denoise(VkCommandBuffer cmd) {
    if (!denoises()) return;
    const uint32_t prev = m_cur ^ 1u;
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    shared::GIDenoisePush pc{};
    pc.extent = u4(m_halfWidth, m_halfHeight, m_width, m_height);
    pc.params = u4(m_historyValid ? 1u : 0u, 1u, m_shift, 0u);
    pc.cameraPos = f4(m_cameraPos, 0.0f);

    // Writes `views` (binding first + i; the last one storage) into `set` and dispatches.
    const auto run = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, uint32_t first,
                         std::initializer_list<std::pair<VkImageView, VkImageLayout>> views, uint32_t groupsX,
                         uint32_t groupsY) {
        std::array<VkDescriptorImageInfo, 8> infos{};
        std::array<VkWriteDescriptorSet, 8> writes{};
        uint32_t n = 0;
        for (const auto& [view, layoutOf] : views) {
            const bool last = n + 1 == views.size();
            infos[n] = {VK_NULL_HANDLE, view, layoutOf};
            writes[n] = descriptorWrite(set, first + n, last ? storage : sampled);
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkUpdateDescriptorSets(m_device, n, writes.data(), 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
    };

    // Temporal: two outputs, so its set is written by hand.
    {
        const VkDescriptorSet set = m_temporalSets[m_slot];
        const VkDescriptorImageInfo infos[8] = {
            {VK_NULL_HANDLE, m_noisy.view, kGeneral},       {VK_NULL_HANDLE, m_guide[m_cur].view, kGeneral},
            {VK_NULL_HANDLE, m_guide[prev].view, kGeneral}, {VK_NULL_HANDLE, m_velocityView, kReadOnly},
            {VK_NULL_HANDLE, m_fast[prev].view, kGeneral},  {VK_NULL_HANDLE, m_slow[prev].view, kGeneral},
            {VK_NULL_HANDLE, m_fast[m_cur].view, kGeneral}, {VK_NULL_HANDLE, m_slow[m_cur].view, kGeneral},
        };
        VkWriteDescriptorSet writes[8];
        for (uint32_t i = 0; i < 8; ++i) {
            writes[i] = descriptorWrite(set, i, i < 6 ? sampled : storage);
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 8, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_temporalLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        const uint32_t tile = shared::kGITile;
        vkCmdDispatch(cmd, (m_halfWidth + tile - 1) / tile, (m_halfHeight + tile - 1) / tile, 1);
    }
    barrier(cmd, kCompute);

    // Two a-trous iterations over the long accumulation: steps 1 then 2. The second writes
    // over the trace's radiance, which nothing reads any more.
    const uint32_t gx = (m_halfWidth + 7) / 8, gy = (m_halfHeight + 7) / 8;
    run(m_atrousPipeline, m_atrousLayout, m_atrousSets[m_slot][0], 8,
        {{m_slow[m_cur].view, kGeneral}, {m_guide[m_cur].view, kGeneral}, {m_atrous.view, kGeneral}}, gx, gy);
    barrier(cmd, kCompute);
    pc.params[1] = 2u;
    run(m_atrousPipeline, m_atrousLayout, m_atrousSets[m_slot][1], 8,
        {{m_atrous.view, kGeneral}, {m_guide[m_cur].view, kGeneral}, {m_noisy.view, kGeneral}}, gx, gy);
    barrier(cmd, kCompute);

    // The joint-bilateral upsample into the output lighting reads.
    rtc::transition(cmd, m_output, kGeneral, kFragment, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT,
                    true);
    run(m_upsamplePipeline, m_upsampleLayout, m_upsampleSets[m_slot], 11,
        {{m_noisy.view, kGeneral}, {m_guide[m_cur].view, kGeneral}, {m_positionView, kReadOnly},
         {m_normalView, kReadOnly}, {m_albedoView, kReadOnly}, {m_output.view, kGeneral}},
        (m_width + 7) / 8, (m_height + 7) / 8);
    rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kFragment, VK_ACCESS_SHADER_READ_BIT,
                    false);
    m_historyValid = true;
}

void RTGITechnique::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    destroyScreenImages();
    createScreenImages();   // the reference mean comes back on first use
}

void RTGITechnique::setRealtime(bool fullResolution, uint32_t rays) {
    m_realtimeRays = std::clamp(rays, 1u, shared::kGIReferenceRays);
    const uint32_t shift = fullResolution ? 0u : 1u;
    if (shift == m_shift) return;
    m_shift = shift;
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);   // frames in flight read the realtime images
    destroyScreenImages();
    createScreenImages();
}

GIOutput RTGITechnique::getOutput() const {
    return {m_output.image, m_output.view};
}

void RTGITechnique::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    m_cache.destroy();
    for (VkPipeline* p : {&m_tracePipeline, &m_temporalPipeline, &m_atrousPipeline, &m_upsamplePipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_traceLayout, &m_temporalLayout, &m_atrousLayout, &m_upsampleLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    for (VkDescriptorSetLayout* l : {&m_traceSetLayout, &m_temporalSetLayout, &m_atrousSetLayout, &m_upsampleSetLayout})
        if (*l) { vkDestroyDescriptorSetLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (m_envSampler) { vkDestroySampler(m_device, m_envSampler, nullptr); m_envSampler = VK_NULL_HANDLE; }
    destroyScreenImages();
    for (Image* d : {&m_guideDummy, &m_refDummy, &m_envDummy}) rtc::destroyImage(m_device, *d);
    for (rtc::HostBuffer& b : m_lights) rtc::destroyHostBuffer(m_device, b);
    m_openpbrTables.destroy();
    m_traceSets = {};
    m_temporalSets = {};
    m_upsampleSets = {};
    m_atrousSets = {};
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
