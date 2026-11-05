#include "rt_direct_technique.hpp"
#include "render/rt/emissive_lights.hpp"
#include "core/log.hpp"
#include <algorithm>

namespace ohao {

namespace shared = gpu::shared;

namespace {

using rtc::descriptorWrite;
using rtc::layoutBinding;

constexpr VkFormat kReservoirFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMeanFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kGuideFormat = VK_FORMAT_R32G32_SFLOAT;
constexpr VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT;
constexpr VkImageUsageFlags kStorageSampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kFragment = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkAccessFlags kReadWrite = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

constexpr uint32_t kFlagReference = 1u;
constexpr uint32_t kDenoiseBindings[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13};
constexpr uint32_t kFlagHistory = 2u;

// A full memory dependency between compute dispatches (and from earlier frames' readers).
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst = kCompute) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = kReadWrite;
    b.dstAccessMask = kReadWrite;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

} // namespace

RTDirectTechnique::~RTDirectTechnique() {
    destroy();
}

void RTDirectTechnique::setBindlessTextures(std::span<const VkImageView> views, std::span<const VkSampler> samplers) {
    const size_t n = std::min(views.size(), samplers.size());
    std::vector<VkDescriptorImageInfo> infos;
    if (n > 0) {
        // Every entry up to the capacity: past the table's views, view 0 (no index below the
        // capacity reads an unwritten descriptor; the shaders treat >= 4096 as no texture).
        infos.resize(m_textureCapacity);
        for (uint32_t i = 0; i < m_textureCapacity; ++i) {
            const size_t src = i < n ? i : 0;
            infos[i] = {samplers[src], views[src], kReadOnly};
        }
    }
    const bool same = infos.size() == m_textureInfos.size() &&
                      std::equal(infos.begin(), infos.end(), m_textureInfos.begin(), [](const auto& a, const auto& b) {
                          return a.sampler == b.sampler && a.imageView == b.imageView;
                      });
    if (same) return;
    m_textureInfos = std::move(infos);
    ++m_textureVersion;
}

bool RTDirectTechnique::init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    if (!rtc::createImage(device, physicalDevice, m_meanDummy, kMeanFormat, kStorageSampled, 1, 1) ||
        !createScreenImages()) {
        OHAO_LOG(Error, render, "RT direct light: failed to create its images");
        return false;
    }
    if (!m_openpbrTables.create(device, physicalDevice)) {
        OHAO_LOG(Error, render, "RT direct light: failed to upload OpenPBR's energy tables");
        return false;
    }
    if (!createDescriptorResources() || !createPipelines()) {
        OHAO_LOG(Error, render, "RT direct light: failed to create its pipelines");
        return false;
    }
    OHAO_LOG(Info, render, "RT direct light: initialized ({}x{})", m_width, m_height);
    return true;
}

bool RTDirectTechnique::createScreenImages() {
    const auto make = [&](Image& img, VkFormat format, VkImageUsageFlags usage) {
        return rtc::createImage(m_device, m_physicalDevice, img, format, usage, m_width, m_height);
    };
    bool ok = make(m_midA, kReservoirFormat, kStorage) && make(m_midB, kReservoirFormat, kStorage) &&
              make(m_noisyDiffuse, kLightFormat, kStorage) && make(m_noisySpecular, kLightFormat, kStorage) &&
              make(m_tmpDiffuse, kLightFormat, kStorage) && make(m_tmpSpecular, kLightFormat, kStorage) &&
              make(m_outDiffuse, kLightFormat, kStorageSampled) && make(m_outSpecular, kLightFormat, kStorageSampled);
    for (uint32_t i = 0; i < 2 && ok; ++i)
        ok = make(m_resA[i], kReservoirFormat, kStorage) && make(m_resB[i], kReservoirFormat, kStorage) &&
             make(m_histDiffuse[i], kLightFormat, kStorage) && make(m_histSpecular[i], kLightFormat, kStorage) &&
             make(m_surfA[i], kReservoirFormat, kStorage) && make(m_surfB[i], kReservoirFormat, kStorage) &&
             make(m_guide[i], kGuideFormat, kStorage) && make(m_moments[i], kMeanFormat, kStorage);
    m_imagesReady = false;
    m_historyValid = false;
    m_refFrames = 0;
    return ok;
}

void RTDirectTechnique::destroyScreenImages() {
    for (Image* img : {&m_midA, &m_midB, &m_noisyDiffuse, &m_noisySpecular, &m_tmpDiffuse, &m_tmpSpecular,
                       &m_outDiffuse, &m_outSpecular, &m_meanDiffuse, &m_meanSpecular})
        rtc::destroyImage(m_device, *img);
    for (uint32_t i = 0; i < 2; ++i)
        for (Image* img : {&m_resA[i], &m_resB[i], &m_histDiffuse[i], &m_histSpecular[i], &m_surfA[i], &m_surfB[i],
                           &m_guide[i], &m_moments[i]})
            rtc::destroyImage(m_device, *img);
}

bool RTDirectTechnique::ensureReferenceImages() {
    if (m_meanDiffuse.image) return true;
    m_refFrames = 0;
    return rtc::createImage(m_device, m_physicalDevice, m_meanDiffuse, kMeanFormat, kStorageSampled, m_width, m_height) &&
           rtc::createImage(m_device, m_physicalDevice, m_meanSpecular, kMeanFormat, kStorageSampled, m_width, m_height);
}

bool RTDirectTechnique::createDescriptorResources() {
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // The resampling passes (rt/restir_di.slang and rt/lights.slang's bindings).
    std::vector<VkDescriptorSetLayoutBinding> sample = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
        layoutBinding(1, sampled), layoutBinding(2, sampled), layoutBinding(3, sampled),
        layoutBinding(4, sampled), layoutBinding(5, sampled), layoutBinding(6, sampled),
        layoutBinding(7, buffer),
        layoutBinding(8, storage), layoutBinding(9, storage),
        layoutBinding(10, buffer), layoutBinding(11, buffer),
        VkDescriptorSetLayoutBinding{12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_textureCapacity,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    for (uint32_t b = 13; b <= 25; ++b) sample.push_back(layoutBinding(b, storage));
    sample.push_back(layoutBinding(42, buffer));
    sample.push_back(layoutBinding(43, sampled));
    sample.push_back(layoutBinding(44, buffer));
    sample.push_back(layoutBinding(45, sampled));   // the G-buffer's OpenPBR layers
    std::vector<VkDescriptorBindingFlags> flags(sample.size(), 0);
    flags[12] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;   // a frame without textures writes none
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = static_cast<uint32_t>(flags.size());
    flagsInfo.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &flagsInfo;
    li.bindingCount = static_cast<uint32_t>(sample.size());
    li.pBindings = sample.data();
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_sampleSetLayout) != VK_SUCCESS) return false;

    // The denoiser (rt/di_denoise.slang): the noisy light 0-1, the guides 2-3, the velocity 4, the
    // accumulations and outputs 6-11, the mean square luminances and frames 12-13.
    std::vector<VkDescriptorSetLayoutBinding> denoise;
    for (const uint32_t b : kDenoiseBindings) denoise.push_back(layoutBinding(b, b == 4 ? sampled : storage));
    li.pNext = nullptr;
    li.bindingCount = static_cast<uint32_t>(denoise.size());
    li.pBindings = denoise.data();
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_denoiseSetLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT},
        {sampled, (8 + 1 * 4) * MAX_FRAMES_IN_FLIGHT},
        {storage, (15 + 12 * 4) * MAX_FRAMES_IN_FLIGHT},
        {buffer, 5 * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_textureCapacity * MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 5 * MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 5;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        return vkAllocateDescriptorSets(m_device, &ai, out) == VK_SUCCESS;
    };
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        if (!allocate(m_sampleSetLayout, &m_sampleSets[s])) return false;
        for (VkDescriptorSet& d : m_denoiseSets[s])
            if (!allocate(m_denoiseSetLayout, &d)) return false;
        m_slotTextureVersion[s] = 0;
    }
    return true;
}

bool RTDirectTechnique::createPipelines() {
    if (!m_alphaTablesLayout || !m_texturesLayout) return false;
    const VkDescriptorSetLayout sets[] = {m_sampleSetLayout, m_alphaTablesLayout, m_texturesLayout};
    // The second pipeline of each pair makes its own layout, identical to the first's (so the first's
    // binds both), kept until teardown: NVIDIA's Linux driver (615.78) crashed binding a pipeline whose layout was
    // destroyed, maintenance4 or not.
    VkPipelineLayout spatialLayout = VK_NULL_HANDLE, shadeLayout = VK_NULL_HANDLE, atrousLayout = VK_NULL_HANDLE,
                     layeredLayout = VK_NULL_HANDLE;
    const bool ok = rtc::createComputePipeline(m_device, "rt_restir_di.slang.initial.spv", "initial", sets,
                                               sizeof(shared::DIPush), m_sampleLayout, m_initial) &&
                    rtc::createComputePipeline(m_device, "rt_restir_di.slang.spatial.spv", "spatial", sets,
                                               sizeof(shared::DIPush), spatialLayout, m_spatial) &&
                    rtc::createComputePipeline(m_device, "rt_restir_di.slang.shade.spv", "shade", sets,
                                               sizeof(shared::DIPush), shadeLayout, m_shade) &&
                    rtc::createComputePipeline(m_device, "rt_restir_di.slang.shadeLayered.spv", "shadeLayered", sets,
                                               sizeof(shared::DIPush), layeredLayout, m_shadeLayered) &&
                    rtc::createComputePipeline(m_device, "rt_di_denoise.slang.temporal.spv", "temporal",
                                               m_denoiseSetLayout, sizeof(shared::DIPush), m_denoiseLayout, m_temporal) &&
                    rtc::createComputePipeline(m_device, "rt_di_denoise.slang.atrous.spv", "atrous", m_denoiseSetLayout,
                                               sizeof(shared::DIPush), atrousLayout, m_atrous);
    for (VkPipelineLayout l : {spatialLayout, shadeLayout, atrousLayout, layeredLayout})
        if (l) m_extraLayouts.push_back(l);
    return ok;
}

void RTDirectTechnique::writeSampleSet(const DirectInput& input) {
    const VkDescriptorSet set = m_sampleSets[m_slot];
    const uint32_t prev = m_cur ^ 1u;
    const bool reference = m_quality == RenderQuality::Reference;
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &input.tlas;
    const VkDescriptorImageInfo gbuf[6] = {
        {VK_NULL_HANDLE, input.positionBuffer, kReadOnly}, {VK_NULL_HANDLE, input.normalBuffer, kReadOnly},
        {VK_NULL_HANDLE, input.albedoBuffer, kReadOnly},   {VK_NULL_HANDLE, input.specularBuffer, kReadOnly},
        {VK_NULL_HANDLE, input.openpbrBuffer, kReadOnly},  {VK_NULL_HANDLE, input.velocityBuffer, kReadOnly},
    };
    const VkDescriptorBufferInfo tables{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo materials{input.materials, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{input.lightData, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo emissiveLights{input.emissive->lightBuffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo emissiveRanges{input.emissive->rangeBuffer(m_slot), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo power{VK_NULL_HANDLE, input.emissive->powerView(), kGeneral};
    const VkDescriptorImageInfo layers{VK_NULL_HANDLE, input.layersBuffer, kReadOnly};
    const VkImageView mean[2] = {reference ? m_meanDiffuse.view : m_meanDummy.view,
                                 reference ? m_meanSpecular.view : m_meanDummy.view};
    // 8-9 last frame's reservoirs, 13-16 this frame's, 17-20 the light, 21-24 the surfaces (this
    // frame's, last frame's), 25 the denoiser's guide.
    const std::pair<uint32_t, VkImageView> storageViews[] = {
        {8, m_resA[prev].view},   {9, m_resB[prev].view},   {13, m_midA.view},           {14, m_midB.view},
        {15, m_resA[m_cur].view}, {16, m_resB[m_cur].view}, {17, m_noisyDiffuse.view},   {18, m_noisySpecular.view},
        {19, mean[0]},            {20, mean[1]},            {21, m_surfA[m_cur].view},   {22, m_surfB[m_cur].view},
        {23, m_surfA[prev].view}, {24, m_surfB[prev].view}, {25, m_guide[m_cur].view},
    };
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(16);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(24);
    VkWriteDescriptorSet w = descriptorWrite(set, 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    w.pNext = &asWrite;
    writes.push_back(w);
    for (uint32_t i = 0; i < 6; ++i) {
        w = descriptorWrite(set, 1 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w.pImageInfo = &gbuf[i];
        writes.push_back(w);
    }
    for (auto [binding, info] : {std::pair{7u, &tables}, std::pair{10u, &materials}, std::pair{11u, &lights},
                                 std::pair{42u, &emissiveLights}, std::pair{44u, &emissiveRanges}}) {
        w = descriptorWrite(set, binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w.pBufferInfo = info;
        writes.push_back(w);
    }
    w = descriptorWrite(set, 43, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    w.pImageInfo = &power;
    writes.push_back(w);
    w = descriptorWrite(set, 45, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    w.pImageInfo = &layers;
    writes.push_back(w);
    for (const auto& [binding, view] : storageViews) images.push_back({VK_NULL_HANDLE, view, kGeneral});
    for (size_t i = 0; i < std::size(storageViews); ++i) {
        w = descriptorWrite(set, storageViews[i].first, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        w.pImageInfo = &images[i];
        writes.push_back(w);
    }
    if (!m_textureInfos.empty() && m_slotTextureVersion[m_slot] != m_textureVersion) {
        w = descriptorWrite(set, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        w.descriptorCount = static_cast<uint32_t>(m_textureInfos.size());
        w.pImageInfo = m_textureInfos.data();
        writes.push_back(w);
        m_slotTextureVersion[m_slot] = m_textureVersion;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void RTDirectTechnique::sample(VkCommandBuffer cmd, const DirectInput& input) {
    m_rendered = false;
    if (!m_initial || input.tlas == VK_NULL_HANDLE || input.alphaTables == VK_NULL_HANDLE ||
        input.textures == VK_NULL_HANDLE || input.lightData == VK_NULL_HANDLE || input.materials == VK_NULL_HANDLE ||
        !input.emissive || input.frameSlot >= MAX_FRAMES_IN_FLIGHT || input.velocityBuffer == VK_NULL_HANDLE ||
        input.layersBuffer == VK_NULL_HANDLE)
        return;
    const bool reference = input.quality == RenderQuality::Reference;
    if (reference && !ensureReferenceImages()) return;
    m_quality = input.quality;
    m_layers = input.layers;
    m_slot = input.frameSlot;
    m_alphaTables = input.alphaTables;
    m_textures = input.textures;
    m_velocityView = input.velocityBuffer;
    if (input.frameIndex == 0) resetHistory();   // a temporal reset: no history
    if (!reference) m_refFrames = 0;              // a realtime frame breaks the reference run
    const uint32_t shift = !reference && input.halfResolution ? 1u : 0u;
    if (shift != m_shift) m_historyValid = false;   // last frame's reservoirs are another image's
    m_shift = shift;
    m_traceWidth = (m_width + shift) >> shift;
    m_traceHeight = (m_height + shift) >> shift;
    m_cur ^= 1u;

    // Earlier frames' readers of these images (the passes, lighting) finish first; new images
    // start in GENERAL.
    barrier(cmd, kCompute | kFragment);
    if (!m_imagesReady) {
        for (Image* img : {&m_midA, &m_midB, &m_noisyDiffuse, &m_noisySpecular, &m_tmpDiffuse, &m_tmpSpecular,
                           &m_resA[0], &m_resA[1], &m_resB[0], &m_resB[1], &m_histDiffuse[0], &m_histDiffuse[1],
                           &m_histSpecular[0], &m_histSpecular[1], &m_surfA[0], &m_surfA[1], &m_surfB[0],
                           &m_surfB[1], &m_guide[0], &m_guide[1], &m_moments[0], &m_moments[1],
                           &m_meanDummy})
            rtc::transition(cmd, *img, kGeneral, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, kCompute, kReadWrite, true);
        m_imagesReady = true;
        m_historyValid = false;
    }
    if (reference) {
        for (Image* img : {&m_meanDiffuse, &m_meanSpecular})
            rtc::transition(cmd, *img, kGeneral, kFragment, VK_ACCESS_SHADER_READ_BIT, kCompute, kReadWrite,
                            img->layout == VK_IMAGE_LAYOUT_UNDEFINED);
    }

    m_push = {};
    m_push.extent = {m_width, m_height, input.frameIndex, input.renderSeed};
    m_push.params = {std::clamp(input.candidates, 1u, shared::kDIMaxCandidates), std::min(input.spatial, shared::kDIMaxSpatial),
                     (reference ? kFlagReference : 0u) | (m_historyValid ? kFlagHistory : 0u), reference ? m_refFrames : 0u};
    m_push.cameraPos = {input.cameraPos.x, input.cameraPos.y, input.cameraPos.z, 0.0f};
    m_push.weather = {input.weather.x, input.weather.y, input.weather.z, 0.0f};
    m_push.trace = {m_traceWidth, m_traceHeight, m_shift, 0u};
    writeSampleSet(input);

    dispatchSample(cmd, m_initial, m_traceWidth, m_traceHeight);
    m_rendered = true;
}

void RTDirectTechnique::dispatchSample(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const VkDescriptorSet sets[] = {m_sampleSets[m_slot], m_alphaTables, m_textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sampleLayout, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_sampleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push), &m_push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    barrier(cmd, kCompute);
}

void RTDirectTechnique::trace(VkCommandBuffer cmd) {
    if (!m_rendered) return;
    dispatchSample(cmd, m_spatial, m_traceWidth, m_traceHeight);
}

void RTDirectTechnique::shade(VkCommandBuffer cmd) {
    if (!m_rendered) return;
    const bool reference = m_quality == RenderQuality::Reference;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layers ? m_shadeLayered : m_shade);
    const VkDescriptorSet sets[] = {m_sampleSets[m_slot], m_alphaTables, m_textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sampleLayout, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_sampleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push), &m_push);
    vkCmdDispatch(cmd, (m_width + 7) / 8, (m_height + 7) / 8, 1);
    if (reference) {
        ++m_refFrames;
        for (Image* img : {&m_meanDiffuse, &m_meanSpecular})
            rtc::transition(cmd, *img, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kFragment,
                            VK_ACCESS_SHADER_READ_BIT, false);
        m_historyValid = false;
    } else {
        barrier(cmd, kCompute);   // the denoiser reads the light and the guide
    }
}

void RTDirectTechnique::denoise(VkCommandBuffer cmd) {
    if (!denoises()) return;
    const uint32_t prev = m_cur ^ 1u;
    // Writes every binding of `set` (views[b] is binding b, kDenoiseBindings') and dispatches
    // `pipeline` with a-trous step `step`.
    const auto run = [&](VkPipeline pipeline, VkDescriptorSet set, const std::array<VkImageView, 15>& views, uint32_t step) {
        std::array<VkDescriptorImageInfo, 15> infos{};
        std::array<VkWriteDescriptorSet, 15> writes{};
        uint32_t n = 0;
        for (const uint32_t b : kDenoiseBindings) {
            infos[n] = {VK_NULL_HANDLE, views[b], b == 4 ? kReadOnly : kGeneral};
            writes[n] = descriptorWrite(set, b, b == 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkUpdateDescriptorSets(m_device, n, writes.data(), 0, nullptr);
        gpu::shared::DIPush push = m_push;
        push.params[3] = step;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoiseLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_denoiseLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (m_width + 7) / 8, (m_height + 7) / 8, 1);
    };
    const auto& sets = m_denoiseSets[m_slot];
    // The views every pass binds: the noisy light, the guides, the velocity, the accumulations.
    const auto views = [&](VkImageView in0, VkImageView in1, VkImageView out0, VkImageView out1) {
        return std::array<VkImageView, 15>{m_noisyDiffuse.view,      m_noisySpecular.view,      m_guide[prev].view,
                                           m_guide[m_cur].view,      m_velocityView,            VK_NULL_HANDLE,
                                           m_histDiffuse[prev].view, m_histSpecular[prev].view, in0,
                                           in1,                      out0,                      out1,
                                           m_moments[prev].view,     m_moments[m_cur].view};
    };
    // temporal: noisy + last frame's accumulation (guided by last frame's guides) -> this frame's.
    run(m_temporal, sets[0],
        views(m_histDiffuse[m_cur].view, m_histSpecular[m_cur].view, m_tmpDiffuse.view, m_tmpSpecular.view), 1u);
    barrier(cmd, kCompute);
    // a-trous, steps 1, 2 and 4: the accumulation -> out -> tmp -> out, the output lighting reads.
    for (Image* img : {&m_outDiffuse, &m_outSpecular})
        rtc::transition(cmd, *img, kGeneral, kFragment, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT, true);
    run(m_atrous, sets[1],
        views(m_histDiffuse[m_cur].view, m_histSpecular[m_cur].view, m_outDiffuse.view, m_outSpecular.view), 1u);
    barrier(cmd, kCompute);
    run(m_atrous, sets[2], views(m_outDiffuse.view, m_outSpecular.view, m_tmpDiffuse.view, m_tmpSpecular.view), 2u);
    barrier(cmd, kCompute);
    run(m_atrous, sets[3], views(m_tmpDiffuse.view, m_tmpSpecular.view, m_outDiffuse.view, m_outSpecular.view), 4u);
    for (Image* img : {&m_outDiffuse, &m_outSpecular})
        rtc::transition(cmd, *img, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kFragment, VK_ACCESS_SHADER_READ_BIT,
                        false);
    m_historyValid = true;
}

VkImageView RTDirectTechnique::diffuse() const noexcept {
    return m_quality == RenderQuality::Reference ? m_meanDiffuse.view : m_outDiffuse.view;
}

VkImageView RTDirectTechnique::specular() const noexcept {
    return m_quality == RenderQuality::Reference ? m_meanSpecular.view : m_outSpecular.view;
}

void RTDirectTechnique::resize(uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    if (!m_device) return;
    destroyScreenImages();
    createScreenImages();
}

void RTDirectTechnique::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    for (VkPipeline* p : {&m_initial, &m_spatial, &m_shade, &m_shadeLayered, &m_temporal, &m_atrous})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_sampleLayout, &m_denoiseLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    for (VkPipelineLayout l : m_extraLayouts) vkDestroyPipelineLayout(m_device, l, nullptr);
    m_extraLayouts.clear();
    if (m_pool) { vkDestroyDescriptorPool(m_device, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
    for (VkDescriptorSetLayout* l : {&m_sampleSetLayout, &m_denoiseSetLayout})
        if (*l) { vkDestroyDescriptorSetLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    destroyScreenImages();
    rtc::destroyImage(m_device, m_meanDummy);
    m_openpbrTables.destroy();
    m_sampleSets = {};
    m_denoiseSets = {};
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
