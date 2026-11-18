#include "rt_direct_technique.hpp"
#include "render/shader_bindings/rt_di_denoise.hpp"
#include "render/shader_bindings/rt_restir_di.hpp"
#include "render/rt/emissive_lights.hpp"
#include "render/rt/rt_alpha_tables.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"
#include <algorithm>

namespace ohao {

namespace shared = gpu::shared;

namespace {


constexpr VkFormat kReservoirFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMeanFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kGuideFormat = VK_FORMAT_R32G32_SFLOAT;
constexpr VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT;
constexpr VkImageUsageFlags kStorageSampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr uint32_t kFlagReference = 1u;
constexpr uint32_t kFlagHistory = 2u;

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

bool RTDirectTechnique::init(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator, uint32_t width,
                             uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_allocator = &allocator;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_meanDummy = allocator.createImage({"di.dummy.mean", kMeanFormat, 1, 1, kStorageSampled});
    if (!m_meanDummy || !createScreenImages()) {
        OHAO_LOG(Error, render, "RT direct light: failed to create its images");
        return false;
    }
    if (!m_openpbrTables.create(allocator)) {
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
    // The frame's: the light shade() writes and the denoiser's guides, in either mode. The rest follow the mode
    // (ensureModeImages).
    m_noisyDiffuse = m_allocator->createImage({"di.noisy.diffuse", kLightFormat, m_width, m_height, kStorage});
    m_noisySpecular = m_allocator->createImage({"di.noisy.specular", kLightFormat, m_width, m_height, kStorage});
    m_guide[0] = m_allocator->createImage({"di.guide0", kGuideFormat, m_width, m_height, kStorage});
    m_guide[1] = m_allocator->createImage({"di.guide1", kGuideFormat, m_width, m_height, kStorage});
    m_imageShift = kNoImages;
    m_historyValid = false;
    m_refFrames = 0;
    return m_noisyDiffuse && m_noisySpecular && m_guide[0] && m_guide[1];
}

bool RTDirectTechnique::ensureModeImages(bool reference, uint32_t shift) {
    const auto make = [&](Image& img, const char* name, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
        img = m_allocator->createImage({name, format, w, h, usage});
        return static_cast<bool>(img);
    };
    bool ok = true;
    // The trace image's reservoirs and surfaces, at the trace's size: made again when the size changes (a changed
    // shift leaves the history anyway).
    if (shift != m_imageShift) {
        for (Image* img : {&m_midA, &m_midB, &m_resA[0], &m_resA[1], &m_resB[0], &m_resB[1], &m_surfA[0], &m_surfA[1],
                           &m_surfB[0], &m_surfB[1]})
            m_allocator->retire(*img);
        const uint32_t w = (m_width + shift) >> shift, h = (m_height + shift) >> shift;
        static constexpr const char* kPair[2][4] = {{"di.reservoir.a0", "di.reservoir.b0", "di.surface.a0", "di.surface.b0"},
                                                    {"di.reservoir.a1", "di.reservoir.b1", "di.surface.a1", "di.surface.b1"}};
        ok = make(m_midA, "di.reservoir.mid.a", kReservoirFormat, w, h, kStorage) &&
             make(m_midB, "di.reservoir.mid.b", kReservoirFormat, w, h, kStorage);
        for (uint32_t i = 0; i < 2 && ok; ++i)
            ok = make(m_resA[i], kPair[i][0], kReservoirFormat, w, h, kStorage) &&
                 make(m_resB[i], kPair[i][1], kReservoirFormat, w, h, kStorage) &&
                 make(m_surfA[i], kPair[i][2], kReservoirFormat, w, h, kStorage) &&
                 make(m_surfB[i], kPair[i][3], kReservoirFormat, w, h, kStorage);
        m_imageShift = ok ? shift : kNoImages;
        m_historyValid = false;
    }
    // Realtime: the denoiser's accumulations, moments and outputs; reference: the running means. The other mode's
    // are given back.
    const auto retireDenoiser = [&]() {
        for (Image* img : {&m_histDiffuse[0], &m_histDiffuse[1], &m_histSpecular[0], &m_histSpecular[1], &m_moments[0],
                           &m_moments[1], &m_tmpDiffuse, &m_tmpSpecular, &m_outDiffuse, &m_outSpecular})
            m_allocator->retire(*img);
    };
    const auto retireMeans = [&]() {
        m_allocator->retire(m_meanDiffuse);
        m_allocator->retire(m_meanSpecular);
    };
    if (reference) {
        retireDenoiser();
        if (ok && !m_meanDiffuse) {
            m_refFrames = 0;
            ok = make(m_meanDiffuse, "di.mean.diffuse", kMeanFormat, m_width, m_height, kStorageSampled) &&
                 make(m_meanSpecular, "di.mean.specular", kMeanFormat, m_width, m_height, kStorageSampled);
            if (!ok) retireMeans();   // made again next frame
        }
    } else {
        retireMeans();
        if (ok && !m_outDiffuse) {
            static constexpr const char* kPair[2][3] = {{"di.history.diffuse0", "di.history.specular0", "di.moments0"},
                                                        {"di.history.diffuse1", "di.history.specular1", "di.moments1"}};
            ok = make(m_tmpDiffuse, "di.tmp.diffuse", kLightFormat, m_width, m_height, kStorage) &&
                 make(m_tmpSpecular, "di.tmp.specular", kLightFormat, m_width, m_height, kStorage) &&
                 make(m_outDiffuse, "di.out.diffuse", kLightFormat, m_width, m_height, kStorageSampled) &&
                 make(m_outSpecular, "di.out.specular", kLightFormat, m_width, m_height, kStorageSampled);
            for (uint32_t i = 0; i < 2 && ok; ++i)
                ok = make(m_histDiffuse[i], kPair[i][0], kLightFormat, m_width, m_height, kStorage) &&
                     make(m_histSpecular[i], kPair[i][1], kLightFormat, m_width, m_height, kStorage) &&
                     make(m_moments[i], kPair[i][2], kMeanFormat, m_width, m_height, kStorage);
            m_historyValid = false;
            if (!ok) retireDenoiser();   // made again next frame
        }
    }
    return ok;
}

void RTDirectTechnique::destroyScreenImages() {
    for (Image* img : {&m_midA, &m_midB, &m_noisyDiffuse, &m_noisySpecular, &m_tmpDiffuse, &m_tmpSpecular,
                       &m_outDiffuse, &m_outSpecular, &m_meanDiffuse, &m_meanSpecular})
        m_allocator->retire(*img);
    for (uint32_t i = 0; i < 2; ++i)
        for (Image* img : {&m_resA[i], &m_resB[i], &m_histDiffuse[i], &m_histSpecular[i], &m_surfA[i], &m_surfB[i],
                           &m_guide[i], &m_moments[i]})
            m_allocator->retire(*img);
    m_imageShift = kNoImages;
}

bool RTDirectTechnique::createDescriptorResources() {
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   // the pool's sizes
    constexpr VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // The resampling passes (rt/restir_di.slang and rt/lights.slang's bindings).
    namespace R = shader::rt_restir_di;
    std::vector<VkDescriptorSetLayoutBinding> sample;
    for (const shader::Binding& b : {R::scene, R::gPosition, R::gNormal, R::gAlbedo, R::gSpecular, R::gOpenPBR,
                                     R::gVelocity, R::openpbrTables, R::prevA, R::prevB, R::materials, R::lightData,
                                     R::midA, R::midB, R::outA, R::outB, R::noisyDiffuse, R::noisySpecular,
                                     R::meanDiffuse, R::meanSpecular, R::surfA, R::surfB, R::prevSurfA, R::prevSurfB,
                                     R::guide, R::emissiveLights, R::emissivePower, R::emissiveRanges, R::gLayers})
        sample.push_back(shader::layoutBinding(b));
    // The bindless textures, sized to the renderer's table and partially bound (a frame without textures writes none).
    sample.push_back({R::textures.binding, R::textures.type, m_textureCapacity, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    std::vector<VkDescriptorBindingFlags> flags(sample.size(), 0);
    flags.back() = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
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
    const auto denoise = shader::layoutBindings(shader::rt_di_denoise::kSet0, VK_SHADER_STAGE_COMPUTE_BIT);
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
    const VkAccelerationStructureKHR tlas = input.scene->tlas();
    asWrite.pAccelerationStructures = &tlas;
    const VkDescriptorImageInfo gbuf[6] = {
        {VK_NULL_HANDLE, input.positionBuffer->view, kReadOnly}, {VK_NULL_HANDLE, input.normalBuffer->view, kReadOnly},
        {VK_NULL_HANDLE, input.albedoBuffer->view, kReadOnly},   {VK_NULL_HANDLE, input.specularBuffer->view, kReadOnly},
        {VK_NULL_HANDLE, input.openpbrBuffer->view, kReadOnly},  {VK_NULL_HANDLE, input.velocityBuffer->view, kReadOnly},
    };
    const VkDescriptorBufferInfo tables{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo materials{input.scene->materialBuffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{input.scene->rtLightBuffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo emissiveLights{input.emissive->lights().buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo emissiveRanges{input.emissive->ranges(m_slot).buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo power{VK_NULL_HANDLE, input.emissive->power().view, kGeneral};
    const VkDescriptorImageInfo layers{VK_NULL_HANDLE, input.layersBuffer->view, kReadOnly};
    const VkImageView mean[2] = {reference ? m_meanDiffuse.view : m_meanDummy.view,
                                 reference ? m_meanSpecular.view : m_meanDummy.view};
    // 8-9 last frame's reservoirs, 13-16 this frame's, 17-20 the light, 21-24 the surfaces (this
    // frame's, last frame's), 25 the denoiser's guide.
    namespace R = shader::rt_restir_di;
    const std::pair<shader::Binding, VkImageView> storageViews[] = {
        {R::prevA, m_resA[prev].view},     {R::prevB, m_resB[prev].view},     {R::midA, m_midA.view},
        {R::midB, m_midB.view},            {R::outA, m_resA[m_cur].view},     {R::outB, m_resB[m_cur].view},
        {R::noisyDiffuse, m_noisyDiffuse.view}, {R::noisySpecular, m_noisySpecular.view},
        {R::meanDiffuse, mean[0]},         {R::meanSpecular, mean[1]},        {R::surfA, m_surfA[m_cur].view},
        {R::surfB, m_surfB[m_cur].view},   {R::prevSurfA, m_surfA[prev].view}, {R::prevSurfB, m_surfB[prev].view},
        {R::guide, m_guide[m_cur].view},
    };
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(16);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(24);
    VkWriteDescriptorSet w = shader::write(shader::rt_restir_di::scene, set);
    w.pNext = &asWrite;
    writes.push_back(w);
    const shader::Binding gbufBindings[6] = {R::gPosition, R::gNormal, R::gAlbedo, R::gSpecular, R::gOpenPBR, R::gVelocity};
    for (uint32_t i = 0; i < 6; ++i) {
        w = shader::write(gbufBindings[i], set);
        w.pImageInfo = &gbuf[i];
        writes.push_back(w);
    }
    for (auto [binding, info] : {std::pair{R::openpbrTables, &tables}, std::pair{R::materials, &materials},
                                 std::pair{R::lightData, &lights}, std::pair{R::emissiveLights, &emissiveLights},
                                 std::pair{R::emissiveRanges, &emissiveRanges}}) {
        w = shader::write(binding, set);
        w.pBufferInfo = info;
        writes.push_back(w);
    }
    w = shader::write(shader::rt_restir_di::emissivePower, set);
    w.pImageInfo = &power;
    writes.push_back(w);
    w = shader::write(shader::rt_restir_di::gLayers, set);
    w.pImageInfo = &layers;
    writes.push_back(w);
    for (const auto& [binding, view] : storageViews) images.push_back({VK_NULL_HANDLE, view, kGeneral});
    for (size_t i = 0; i < std::size(storageViews); ++i) {
        w = shader::write(storageViews[i].first, set);
        w.pImageInfo = &images[i];
        writes.push_back(w);
    }
    if (!m_textureInfos.empty() && m_slotTextureVersion[m_slot] != m_textureVersion) {
        w = shader::write(shader::rt_restir_di::textures, set);
        w.descriptorCount = static_cast<uint32_t>(m_textureInfos.size());
        w.pImageInfo = m_textureInfos.data();
        writes.push_back(w);
        m_slotTextureVersion[m_slot] = m_textureVersion;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void RTDirectTechnique::declareInputs(render::Pass& pass) {
    const DirectInput& in = m_input;
    for (gpu::Image* g : {in.positionBuffer, in.normalBuffer, in.albedoBuffer, in.specularBuffer, in.openpbrBuffer,
                          in.velocityBuffer, in.layersBuffer})
        pass.sampled(*g);
    in.alphaTables->declare(pass, m_slot, *in.scene);
    const render::RenderScene::Buffers scene = in.scene->buffers();
    pass.read(scene.rtLights, render::BufferUse::Storage);   // the materials are among the alpha tables
    pass.read(in.emissive->lights(), render::BufferUse::Storage).read(in.emissive->ranges(m_slot), render::BufferUse::Storage);
    pass.sampledGeneral(in.emissive->power());
    pass.read(m_openpbrTables.tracked(), render::BufferUse::Storage);
    // The bindless textures are immutable once uploaded and always SHADER_READ_ONLY: nothing to declare.
}

void RTDirectTechnique::sample(const render::Frame& frame, const DirectInput& input) {
    m_rendered = false;
    if (!m_initial || !input.scene || input.scene->tlas() == VK_NULL_HANDLE || !input.alphaTables ||
        input.alphaTables->set(input.frameSlot) == VK_NULL_HANDLE || input.textures == VK_NULL_HANDLE ||
        input.scene->rtLightBuffer() == VK_NULL_HANDLE || input.scene->materialBuffer() == VK_NULL_HANDLE ||
        !input.emissive || input.frameSlot >= MAX_FRAMES_IN_FLIGHT)
        return;
    for (const gpu::Image* g : {input.positionBuffer, input.normalBuffer, input.albedoBuffer, input.specularBuffer,
                                input.openpbrBuffer, input.velocityBuffer, input.layersBuffer})
        if (!g || !*g) return;
    const bool reference = input.quality == RenderQuality::Reference;
    const uint32_t shift = !reference && input.halfResolution ? 1u : 0u;
    if (!ensureModeImages(reference, shift)) return;
    m_quality = input.quality;
    m_layers = input.layers;
    m_slot = input.frameSlot;
    m_input = input;
    m_textures = input.textures;
    if (input.frameIndex == 0) resetHistory();   // a temporal reset: no history
    if (!reference) m_refFrames = 0;              // a realtime frame breaks the reference run
    if (shift != m_shift) m_historyValid = false;   // last frame's reservoirs are another image's
    m_shift = shift;
    m_traceWidth = (m_width + shift) >> shift;
    m_traceHeight = (m_height + shift) >> shift;
    m_cur ^= 1u;
    const uint32_t prev = m_cur ^ 1u;

    m_push = {};
    m_push.extent = {m_width, m_height, input.frameIndex, input.renderSeed};
    m_push.params = {std::clamp(input.candidates, 1u, shared::kDIMaxCandidates), std::min(input.spatial, shared::kDIMaxSpatial),
                     (reference ? kFlagReference : 0u) | (m_historyValid ? kFlagHistory : 0u), reference ? m_refFrames : 0u};
    m_push.cameraPos = {input.cameraPos.x, input.cameraPos.y, input.cameraPos.z, 0.0f};
    m_push.weather = {input.weather.x, input.weather.y, input.weather.z, 0.0f};
    m_push.trace = {m_traceWidth, m_traceHeight, m_shift, 0u};
    writeSampleSet(input);

    // Each trace pixel's surface and its reservoir after the temporal reuse of last frame's.
    render::Pass pass(frame, nullptr, render::Pass::Compute);
    declareInputs(pass);
    pass.storageRead(m_resA[prev]).storageRead(m_resB[prev]).storageRead(m_surfA[prev]).storageRead(m_surfB[prev]);
    for (Image* img : {&m_surfA[m_cur], &m_surfB[m_cur], &m_midA, &m_midB}) pass.storageWrite(*img, render::Old::Keep);
    pass.begin();
    dispatchSample(frame.cmd, m_initial, m_traceWidth, m_traceHeight);
    m_rendered = true;
}

void RTDirectTechnique::dispatchSample(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const VkDescriptorSet sets[] = {m_sampleSets[m_slot], m_input.alphaTables->set(m_slot), m_textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sampleLayout, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_sampleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push), &m_push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

void RTDirectTechnique::trace(const render::Frame& frame) {
    if (!m_rendered) return;
    // The spatial reuse of the neighbours' reservoirs and the final ray.
    render::Pass pass(frame, nullptr, render::Pass::Compute);
    declareInputs(pass);
    pass.storageRead(m_midA).storageRead(m_midB).storageRead(m_surfA[m_cur]).storageRead(m_surfB[m_cur]);
    pass.storageWrite(m_resA[m_cur], render::Old::Keep).storageWrite(m_resB[m_cur], render::Old::Keep);
    pass.begin();
    dispatchSample(frame.cmd, m_spatial, m_traceWidth, m_traceHeight);
}

void RTDirectTechnique::shade(const render::Frame& frame) {
    if (!m_rendered) return;
    const bool reference = m_quality == RenderQuality::Reference;
    render::Pass pass(frame, nullptr, render::Pass::Compute);
    declareInputs(pass);
    pass.storageRead(m_resA[m_cur]).storageRead(m_resB[m_cur]).storageRead(m_surfA[m_cur]).storageRead(m_surfB[m_cur]);
    for (Image* img : {&m_guide[m_cur], &m_noisyDiffuse, &m_noisySpecular}) pass.storageWrite(*img, render::Old::Keep);
    if (reference) pass.storageReadWrite(m_meanDiffuse).storageReadWrite(m_meanSpecular);   // the running means
    else pass.storageRead(m_meanDummy);                                                     // bound, never accessed
    pass.begin();
    dispatchSample(frame.cmd, m_layers ? m_shadeLayered : m_shade, m_width, m_height);
    if (reference) {
        ++m_refFrames;
        m_historyValid = false;
    }
}

void RTDirectTechnique::denoise(const render::Frame& frame) {
    if (!denoises()) return;
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t prev = m_cur ^ 1u;
    namespace D = shader::rt_di_denoise;
    using Views = std::array<std::pair<shader::Binding, VkImageView>, D::kSet0.size()>;
    // Writes every binding of `set` and dispatches `pipeline` with a-trous step `step`.
    const auto run = [&](VkPipeline pipeline, VkDescriptorSet set, const Views& views, uint32_t step) {
        std::array<VkDescriptorImageInfo, D::kSet0.size()> infos{};
        std::array<VkWriteDescriptorSet, D::kSet0.size()> writes{};
        uint32_t n = 0;
        for (const auto& [b, view] : views) {
            infos[n] = {VK_NULL_HANDLE, view, b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? kReadOnly : kGeneral};
            writes[n] = shader::write(b, set);
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
    const auto views = [&](Image& in0, Image& in1, Image& out0, Image& out1) {
        return Views{{{D::noisyDiffuse, m_noisyDiffuse.view},
                      {D::noisySpecular, m_noisySpecular.view},
                      {D::prevGuide, m_guide[prev].view},
                      {D::guide, m_guide[m_cur].view},
                      {D::gVelocity, m_input.velocityBuffer->view},
                      {D::histPrevDiffuse, m_histDiffuse[prev].view},
                      {D::histPrevSpecular, m_histSpecular[prev].view},
                      {D::inDiffuse, in0.view},
                      {D::inSpecular, in1.view},
                      {D::outDiffuse, out0.view},
                      {D::outSpecular, out1.view},
                      {D::prevMoments, m_moments[prev].view},
                      {D::moments, m_moments[m_cur].view}}};
    };
    // temporal: noisy + last frame's accumulation (guided by last frame's guides) -> this frame's (bound as the
    // a-trous input) and its moments.
    {
        render::Pass pass(frame, nullptr, render::Pass::Compute);
        pass.sampled(*m_input.velocityBuffer);
        for (Image* img : {&m_noisyDiffuse, &m_noisySpecular, &m_guide[prev], &m_guide[m_cur], &m_histDiffuse[prev],
                           &m_histSpecular[prev], &m_moments[prev]})
            pass.storageRead(*img);
        for (Image* img : {&m_histDiffuse[m_cur], &m_histSpecular[m_cur], &m_moments[m_cur]})
            pass.storageWrite(*img, render::Old::Keep);
        pass.begin();
        run(m_temporal, sets[0], views(m_histDiffuse[m_cur], m_histSpecular[m_cur], m_tmpDiffuse, m_tmpSpecular), 1u);
    }
    // a-trous, steps 1, 2 and 4: the accumulation -> out -> tmp -> out, the output lighting reads.
    const auto atrous = [&](VkDescriptorSet set, Image& in0, Image& in1, Image& out0, Image& out1, render::Old old,
                            uint32_t step) {
        render::Pass pass(frame, nullptr, render::Pass::Compute);
        pass.storageRead(in0).storageRead(in1).storageRead(m_guide[m_cur]).storageRead(m_moments[m_cur]);
        pass.storageWrite(out0, old).storageWrite(out1, old);
        pass.begin();
        run(m_atrous, set, views(in0, in1, out0, out1), step);
    };
    atrous(sets[1], m_histDiffuse[m_cur], m_histSpecular[m_cur], m_outDiffuse, m_outSpecular, render::Old::Discard, 1u);
    atrous(sets[2], m_outDiffuse, m_outSpecular, m_tmpDiffuse, m_tmpSpecular, render::Old::Keep, 2u);
    atrous(sets[3], m_tmpDiffuse, m_tmpSpecular, m_outDiffuse, m_outSpecular, render::Old::Keep, 4u);
    m_historyValid = true;
}

gpu::Image& RTDirectTechnique::diffuse() noexcept {
    return m_quality == RenderQuality::Reference ? m_meanDiffuse : m_outDiffuse;
}

gpu::Image& RTDirectTechnique::specular() noexcept {
    return m_quality == RenderQuality::Reference ? m_meanSpecular : m_outSpecular;
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
    m_allocator->retire(m_meanDummy);
    m_openpbrTables.destroy();
    m_sampleSets = {};
    m_denoiseSets = {};
    m_device = VK_NULL_HANDLE;
}

} // namespace ohao
