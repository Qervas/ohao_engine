#include "rt_gi_technique.hpp"
#include "render/shader_bindings/rt_gi_trace.hpp"
#include "render/shader_bindings/rt_gi_denoise.hpp"
#include "render/rt/rt_alpha_tables.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"
#include <span>
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

namespace {
// The denoiser's kernels (rt/gi_denoise.slang) each bind their own part of its set 0.
namespace D = shader::rt_gi_denoise;
constexpr std::array kTemporal{D::tNoisy, D::tGuide, D::tGuidePrev, D::tVelocity,
                               D::tFastPrev, D::tSlowPrev, D::tFastOut, D::tSlowOut};
constexpr std::array kAtrous{D::aInput, D::aGuide, D::aOutput};
constexpr std::array kUpsample{D::uInput, D::uGuide, D::uPosition, D::uNormal, D::uAlbedo, D::uOutput};
} // namespace

namespace shared = gpu::shared;

// The trace reads a hit's instance row (its mesh and materials) straight from the instance table.
static_assert(sizeof(render::InstanceGpu) == shared::kInstanceGpuStride);

namespace {


constexpr VkFormat kRadianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGuideFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kRefFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

bool RTGITechnique::init(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator, uint32_t width,
                         uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_allocator = &allocator;
    m_width = width;
    m_height = height;

    m_guideDummy = allocator.createImage({"rt.gi.dummy.guide", kGuideFormat, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT});
    m_refDummy = allocator.createImage({"rt.gi.dummy.mean", kRefFormat, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT});
    m_envDummy = allocator.createImage({"rt.gi.dummy.env", kRadianceFormat, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT});
    const bool dummies = m_guideDummy && m_refDummy && m_envDummy;
    if (!dummies || !createScreenImages()) {
        OHAO_LOG(Error, render, "RTGI: failed to create its images");
        return false;
    }
    if (!m_openpbrTables.create(device, physicalDevice)) {
        OHAO_LOG(Error, render, "RTGI: failed to upload OpenPBR's energy tables");
        return false;
    }
    if (!m_cache.create(device, *m_allocator, kCacheCapacity, kCacheMaxSamples)) {
        OHAO_LOG(Error, render, "RTGI: failed to create its radiance cache");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_lightCapacity[i] = shared::kGIWeighedLights;
        m_lights[i] = rtc::createHostBuffer(allocator, sizeof(shared::GILight) * m_lightCapacity[i],
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!m_lights[i].isValid()) {
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
    const auto half = [&](Image& img, const char* name, VkFormat format) {
        img = m_allocator->createImage({name, format, m_halfWidth, m_halfHeight, kUsage});
        return static_cast<bool>(img);
    };
    static constexpr const char* kPair[2][5] = {
        {"rt.gi.guide0", "rt.gi.fast0", "rt.gi.slow0", "rt.gi.spec.fast0", "rt.gi.spec.slow0"},
        {"rt.gi.guide1", "rt.gi.fast1", "rt.gi.slow1", "rt.gi.spec.fast1", "rt.gi.spec.slow1"}};
    bool ok = half(m_noisy, "rt.gi.noisy", kRadianceFormat) && half(m_atrous, "rt.gi.atrous", kRadianceFormat) &&
              half(m_specNoisy, "rt.gi.spec.noisy", kRadianceFormat) && half(m_specAtrous, "rt.gi.spec.atrous", kRadianceFormat);
    for (uint32_t i = 0; i < 2 && ok; ++i)
        ok = half(m_guide[i], kPair[i][0], kGuideFormat) && half(m_fast[i], kPair[i][1], kRadianceFormat) &&
             half(m_slow[i], kPair[i][2], kRadianceFormat) && half(m_specFast[i], kPair[i][3], kRadianceFormat) &&
             half(m_specSlow[i], kPair[i][4], kRadianceFormat);
    if (ok) {
        m_output = m_allocator->createImage({"rt.gi.output", kRadianceFormat, m_width, m_height, kUsage});
        m_specOutput = m_allocator->createImage({"rt.gi.spec.output", kRadianceFormat, m_width, m_height, kUsage});
        ok = m_output && m_specOutput;
    }
    m_historyValid = false;
    return ok;
}

void RTGITechnique::destroyScreenImages() {
    for (Image* img : {&m_noisy, &m_atrous, &m_output, &m_refMean, &m_specNoisy, &m_specAtrous, &m_specOutput,
                       &m_specRefMean})
        m_allocator->retire(*img);
    for (uint32_t i = 0; i < 2; ++i)
        for (Image* img : {&m_guide[i], &m_fast[i], &m_slow[i], &m_specFast[i], &m_specSlow[i]})
            m_allocator->retire(*img);
}

bool RTGITechnique::ensureReferenceImage() {
    if (!m_refMean)
        m_refMean = m_allocator->createImage({"rt.gi.mean", kRefFormat, m_width, m_height, VK_IMAGE_USAGE_STORAGE_BIT});
    if (!m_specRefMean)
        m_specRefMean =
            m_allocator->createImage({"rt.gi.spec.mean", kRefFormat, m_width, m_height, VK_IMAGE_USAGE_STORAGE_BIT});
    return m_refMean && m_specRefMean;
}

bool RTGITechnique::createDescriptorResources() {
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   // the pool's sizes
    const auto trace = shader::layoutBindings(shader::rt_gi_trace::kSet0, VK_SHADER_STAGE_COMPUTE_BIT);
    const auto temporal = shader::layoutBindings(kTemporal, VK_SHADER_STAGE_COMPUTE_BIT);
    const auto atrous = shader::layoutBindings(kAtrous, VK_SHADER_STAGE_COMPUTE_BIT);
    const auto upsample = shader::layoutBindings(kUpsample, VK_SHADER_STAGE_COMPUTE_BIT);
    for (auto [bindings, layout] : {std::pair{&trace, &m_traceSetLayout}, std::pair{&temporal, &m_temporalSetLayout},
                                    std::pair{&atrous, &m_atrousSetLayout}, std::pair{&upsample, &m_upsampleSetLayout}}) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(bindings->size());
        li.pBindings = bindings->data();
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, layout) != VK_SUCCESS) return false;
    }

    // Per frame slot: trace, and per chain temporal, kAtrousSteps a-trous, upsample.
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * MAX_FRAMES_IN_FLIGHT},
        {sampled, (5 + 2 * (6 + kAtrousSteps * 2 + 5)) * MAX_FRAMES_IN_FLIGHT},
        {storage, (5 + 2 * (2 + kAtrousSteps * 1 + 1)) * MAX_FRAMES_IN_FLIGHT},
        {buffer, 6 * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1 * MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = (1 + 2 * (2 + kAtrousSteps)) * MAX_FRAMES_IN_FLIGHT;
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
        if (!allocate(m_traceSetLayout, &m_traceSets[s])) return false;
        for (uint32_t c = 0; c < 2; ++c) {
            if (!allocate(m_temporalSetLayout, &m_temporalSets[s][c]) || !allocate(m_upsampleSetLayout, &m_upsampleSets[s][c]))
                return false;
            for (VkDescriptorSet& set : m_atrousSets[s][c])
                if (!allocate(m_atrousSetLayout, &set)) return false;
        }
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

void RTGITechnique::render(const render::Frame& frame, const GIInput& input) {
    const VkCommandBuffer cmd = frame.cmd;
    m_rendered = false;
    if (!m_tracePipeline || !input.scene || input.scene->tlas() == VK_NULL_HANDLE ||
        input.scene->instanceBuffer() == VK_NULL_HANDLE)
        return;
    if (!input.alphaTables || input.alphaTables->set(input.frameSlot) == VK_NULL_HANDLE || input.textures == VK_NULL_HANDLE)
        return;
    if (input.frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    if (!input.positionBuffer || !input.normalBuffer || !input.albedoBuffer || !input.layersBuffer) return;
    const bool reference = input.quality == RenderQuality::Reference;
    if (!reference && !input.velocityBuffer) return;
    if (reference && !ensureReferenceImage()) return;
    m_quality = input.quality;
    m_slot = input.frameSlot;
    m_velocity = input.velocityBuffer;
    m_position = input.positionBuffer;
    m_normal = input.normalBuffer;
    m_albedo = input.albedoBuffer;
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
        m_allocator->destroyBuffer(m_lights[m_slot]);
        m_lights[m_slot] =
            rtc::createHostBuffer(*m_allocator, sizeof(shared::GILight) * capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!m_lights[m_slot].isValid()) {
            OHAO_LOG(Error, render, "RTGI: failed to grow its light buffer to {} lights", capacity);
            m_lightCapacity[m_slot] = 0;
            return;
        }
        m_lightCapacity[m_slot] = capacity;
    }
    if (lightCount > 0) {
        std::memcpy(m_lights[m_slot].getMappedData(), bounce.data(), sizeof(shared::GILight) * lightCount);
        m_allocator->flushBuffer(m_lights[m_slot], 0, sizeof(shared::GILight) * lightCount);
    }

    render::Pass pass(frame, nullptr, render::Pass::Compute);
    pass.sampled(*input.positionBuffer).sampled(*input.normalBuffer).sampled(*input.albedoBuffer).sampled(*input.layersBuffer);
    input.alphaTables->declare(pass, m_slot, *input.scene);
    pass.read(m_lights[m_slot], render::BufferUse::Storage);
    // LEGACY(step 4a): the environment map is EnvUpload's and OpenPBR's tables an OpenPBRTablesBuffer, not tracked
    // yet.
    if (!input.envMap) pass.sampled(m_envDummy);
    if (reference) {
        m_historyValid = false;
        m_traceExtent = {m_width, m_height};
        m_rays = shared::kGIReferenceRays;
        pass.storageWrite(m_output).storageWrite(m_specOutput);
        // The running mean reads last frame's while the reference frames run unbroken; a new
        // image holds none of them. A run that starts again starts the cache again too: the same
        // reference frames after any history.
        if (m_refMean.state(0, 0).layout == VK_IMAGE_LAYOUT_UNDEFINED) m_refFrames = 0;
        if (m_refFrames == 0) m_cache.clear();
        for (Image* img : {&m_refMean, &m_specRefMean}) {
            if (m_refFrames == 0) pass.storageWrite(*img);
            else pass.storageReadWrite(*img);
        }
        pass.storageRead(m_guideDummy);   // bound where the realtime trace writes its guide: never accessed
    } else {
        m_refFrames = 0;   // a realtime frame breaks the reference run
        m_cur ^= 1u;
        m_traceExtent = {m_halfWidth, m_halfHeight};
        m_rays = m_realtimeRays;
        pass.storageWrite(m_noisy, render::Old::Keep).storageWrite(m_specNoisy, render::Old::Keep);
        pass.storageWrite(m_guide[m_cur], render::Old::Keep);
        pass.storageRead(m_refDummy);     // bound where the reference trace keeps its means: never accessed
    }

    const VkDescriptorSet set = m_traceSets[m_slot];
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    const VkAccelerationStructureKHR tlas = input.scene->tlas();
    asWrite.pAccelerationStructures = &tlas;
    const VkDescriptorImageInfo gbuf[3] = {{VK_NULL_HANDLE, input.positionBuffer->view, kReadOnly},
                                           {VK_NULL_HANDLE, input.normalBuffer->view, kReadOnly},
                                           {VK_NULL_HANDLE, input.albedoBuffer->view, kReadOnly}};
    const VkDescriptorBufferInfo instancesInfo{input.scene->instanceBuffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lightsInfo{m_lights[m_slot].buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo envInfo{VK_NULL_HANDLE, input.envMap ? input.envMap : m_envDummy.view, kReadOnly};
    const VkDescriptorImageInfo samplerInfo{m_envSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, reference ? m_output.view : m_noisy.view, kGeneral};
    const VkDescriptorImageInfo guideInfo{VK_NULL_HANDLE, reference ? m_guideDummy.view : m_guide[m_cur].view, kGeneral};
    const VkDescriptorImageInfo meanInfo{VK_NULL_HANDLE, reference ? m_refMean.view : m_refDummy.view, kGeneral};
    const VkDescriptorBufferInfo tablesInfo{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo specInfo{VK_NULL_HANDLE, reference ? m_specOutput.view : m_specNoisy.view, kGeneral};
    const VkDescriptorImageInfo specMeanInfo{VK_NULL_HANDLE, reference ? m_specRefMean.view : m_refDummy.view, kGeneral};
    const VkDescriptorBufferInfo cacheInfo[3] = {{m_cache.keys(), 0, VK_WHOLE_SIZE},
                                                 {m_cache.accum(), 0, VK_WHOLE_SIZE},
                                                 {m_cache.resolved(), 0, VK_WHOLE_SIZE}};
    const VkDescriptorImageInfo layersInfo{VK_NULL_HANDLE, input.layersBuffer->view, kReadOnly};
    VkWriteDescriptorSet writes[18] = {
        shader::write(shader::rt_gi_trace::scene, set),
        shader::write(shader::rt_gi_trace::gPosition, set), shader::write(shader::rt_gi_trace::gNormal, set),
        shader::write(shader::rt_gi_trace::gAlbedo, set),
        shader::write(shader::rt_gi_trace::instances, set), shader::write(shader::rt_gi_trace::lights, set),
        shader::write(shader::rt_gi_trace::envMap, set), shader::write(shader::rt_gi_trace::envSampler, set),
        shader::write(shader::rt_gi_trace::radianceOut, set), shader::write(shader::rt_gi_trace::guideOut, set),
        shader::write(shader::rt_gi_trace::refMean, set),
        shader::write(shader::rt_gi_trace::openpbrTables, set),
        shader::write(shader::rt_gi_trace::cacheKeys, set), shader::write(shader::rt_gi_trace::cacheAccum, set),
        shader::write(shader::rt_gi_trace::cacheResolved, set),
        shader::write(shader::rt_gi_trace::specOut, set), shader::write(shader::rt_gi_trace::specRefMean, set),
        shader::write(shader::rt_gi_trace::gLayers, set),
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
    writes[15].pImageInfo = &specInfo;
    writes[16].pImageInfo = &specMeanInfo;
    writes[17].pImageInfo = &layersInfo;
    vkUpdateDescriptorSets(m_device, 18, writes, 0, nullptr);

    // The cache: emptied if a reset asked, then read and fed by the trace.
    m_cache.recordBeginFrame(frame);
    m_cache.fedBy(pass);

    pass.begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
    const VkDescriptorSet traceSets[] = {set, input.alphaTables->set(m_slot), input.textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_traceLayout, 0, 3, traceSets, 0, nullptr);
    shared::GITracePush pc{};
    pc.extent = u4(m_traceExtent.width, m_traceExtent.height, m_width, m_height);
    pc.seed = u4(input.renderSeed, input.frameIndex, m_rays, lightCount);
    pc.flags = u4(reference ? 1u : 0u, (input.envMap ? 1u : 0u) | (input.emissionIsDirect ? 2u : 0u),
                  reference ? m_refFrames : 0u, m_shift);
    pc.cameraPos = f4(input.cameraPos, input.envIntensity);
    vkCmdPushConstants(cmd, m_traceLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (m_traceExtent.width + 7) / 8, (m_traceExtent.height + 7) / 8, 1);
    pass.end();
    m_cache.recordResolve(frame);   // this frame's samples blended in for the next frame's rays
    m_rendered = true;
    if (reference) ++m_refFrames;
}

void RTGITechnique::denoise(const render::Frame& frame) {
    if (!denoises()) return;
    denoiseChain(frame, 0);
    denoiseChain(frame, 1);
    m_historyValid = true;
}

void RTGITechnique::denoiseChain(const render::Frame& frame, uint32_t chain) {
    const VkCommandBuffer cmd = frame.cmd;
    const bool spec = chain == 1u;
    Image& noisy = spec ? m_specNoisy : m_noisy;
    Image& atrous = spec ? m_specAtrous : m_atrous;
    Image& output = spec ? m_specOutput : m_output;
    auto& fast = spec ? m_specFast : m_fast;
    auto& slow = spec ? m_specSlow : m_slow;
    const uint32_t prev = m_cur ^ 1u;
    shared::GIDenoisePush pc{};
    pc.extent = u4(m_halfWidth, m_halfHeight, m_width, m_height);
    pc.params = u4(m_historyValid ? 1u : 0u, 1u, m_shift, spec ? 1u : 0u);
    pc.cameraPos = f4(m_cameraPos, 0.0f);

    // Writes `views` into `set` at the kernel's bindings, in order, and dispatches.
    const auto run = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                         std::span<const shader::Binding> bindings,
                         std::initializer_list<std::pair<VkImageView, VkImageLayout>> views, uint32_t groupsX,
                         uint32_t groupsY) {
        std::array<VkDescriptorImageInfo, 8> infos{};
        std::array<VkWriteDescriptorSet, 8> writes{};
        uint32_t n = 0;
        for (const auto& [view, layoutOf] : views) {
            infos[n] = {VK_NULL_HANDLE, view, layoutOf};
            writes[n] = shader::write(bindings[n], set);
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
        render::Pass pass(frame, nullptr, render::Pass::Compute);
        pass.sampledGeneral(noisy).sampledGeneral(m_guide[m_cur]).sampledGeneral(m_guide[prev]).sampled(*m_velocity);
        pass.sampledGeneral(fast[prev]).sampledGeneral(slow[prev]);
        pass.storageWrite(fast[m_cur], render::Old::Keep).storageWrite(slow[m_cur], render::Old::Keep);
        pass.begin();
        const VkDescriptorSet set = m_temporalSets[m_slot][chain];
        const VkDescriptorImageInfo infos[8] = {
            {VK_NULL_HANDLE, noisy.view, kGeneral},         {VK_NULL_HANDLE, m_guide[m_cur].view, kGeneral},
            {VK_NULL_HANDLE, m_guide[prev].view, kGeneral}, {VK_NULL_HANDLE, m_velocity->view, kReadOnly},
            {VK_NULL_HANDLE, fast[prev].view, kGeneral},    {VK_NULL_HANDLE, slow[prev].view, kGeneral},
            {VK_NULL_HANDLE, fast[m_cur].view, kGeneral},   {VK_NULL_HANDLE, slow[m_cur].view, kGeneral},
        };
        VkWriteDescriptorSet writes[8];
        for (uint32_t i = 0; i < 8; ++i) {
            writes[i] = shader::write(kTemporal[i], set);
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 8, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_temporalLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        const uint32_t tile = shared::kGITile;
        vkCmdDispatch(cmd, (m_halfWidth + tile - 1) / tile, (m_halfHeight + tile - 1) / tile, 1);
    }

    // The a-trous iterations over the long accumulation, steps 1, 2, 4, 8 (the reflection's 1
    // and 2: its spread is the surface's roughness), between the a-trous image and the trace's
    // radiance (which nothing reads any more); the last writes the latter.
    const uint32_t gx = (m_halfWidth + 7) / 8, gy = (m_halfHeight + 7) / 8;
    const uint32_t steps = spec ? 2u : kAtrousSteps;
    for (uint32_t i = 0; i < steps; ++i) {
        Image& in = i == 0 ? slow[m_cur] : (i % 2 == 1 ? atrous : noisy);
        Image& out = i % 2 == 0 ? atrous : noisy;
        pc.params[1] = 1u << i;
        render::Pass pass(frame, nullptr, render::Pass::Compute);
        pass.sampledGeneral(in).sampledGeneral(m_guide[m_cur]).storageWrite(out, render::Old::Keep);
        pass.begin();
        run(m_atrousPipeline, m_atrousLayout, m_atrousSets[m_slot][chain][i], kAtrous,
            {{in.view, kGeneral}, {m_guide[m_cur].view, kGeneral}, {out.view, kGeneral}}, gx, gy);
    }
    static_assert(kAtrousSteps % 2 == 0, "the last a-trous step writes the trace's radiance image");

    // The joint-bilateral upsample into the output lighting reads.
    render::Pass pass(frame, nullptr, render::Pass::Compute);
    pass.sampledGeneral(noisy).sampledGeneral(m_guide[m_cur]);
    pass.sampled(*m_position).sampled(*m_normal).sampled(*m_albedo).storageWrite(output);
    pass.begin();
    run(m_upsamplePipeline, m_upsampleLayout, m_upsampleSets[m_slot][chain], kUpsample,
        {{noisy.view, kGeneral}, {m_guide[m_cur].view, kGeneral}, {m_position->view, kReadOnly},
         {m_normal->view, kReadOnly}, {m_albedo->view, kReadOnly}, {output.view, kGeneral}},
        (m_width + 7) / 8, (m_height + 7) / 8);
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
    destroyScreenImages();
    createScreenImages();
}

GIOutput RTGITechnique::getOutput() {
    return {&m_output, &m_specOutput};
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
    for (Image* d : {&m_guideDummy, &m_refDummy, &m_envDummy}) m_allocator->retire(*d);
    for (GpuBuffer& b : m_lights) m_allocator->retire(b);
    m_openpbrTables.destroy();
    m_traceSets = {};
    m_temporalSets = {};
    m_upsampleSets = {};
    m_atrousSets = {};
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
