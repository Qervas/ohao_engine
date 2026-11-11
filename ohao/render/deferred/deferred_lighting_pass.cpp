#include "deferred_lighting_pass.hpp"
#include "render/shader_bindings/core_deferred_lighting.hpp"
#include "render/graph/pipeline.hpp"
#include "core/log.hpp"
#include <array>
#include <cstring>

namespace ohao {

DeferredLightingPass::~DeferredLightingPass() {
    cleanup();
}

bool DeferredLightingPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!m_allocator) return false;
    if (!createOutputImages()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    // Create G-Buffer sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_gbufferSampler) != VK_SUCCESS) {
        return false;
    }

    // Create dummy resources for fallback descriptor bindings
    if (!m_openpbrTables.create(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredLightingPass: failed to upload OpenPBR's energy tables");
        return false;
    }

    if (!createDummyResources()) {
        return false;
    }

    return true;
}

void DeferredLightingPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_gbufferSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_gbufferSampler, nullptr);
        m_gbufferSampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }

    destroyOutputImages();
    destroyDummyResources();
    m_openpbrTables.destroy();
}

void DeferredLightingPass::record(const render::Frame& frame) {
    render::Timed timed(frame, "lighting");
    const uint32_t frameIndex = frame.slot;
    if (!m_gbufferPass || !m_lit || !m_subsurfaceDiffuse) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        frameIndex >= MAX_FRAMES_IN_FLIGHT || m_descriptorSets[frameIndex] == VK_NULL_HANDLE) return;
    const VkCommandBuffer cmd = frame.cmd;

    // Inputs (light buffer, RT GI, RT direct light, env map) change between
    // frames, so this frame's set is rewritten from them here, at record time.
    writeDescriptorSet(frameIndex);

    gpu::Image* const shadowMap = boundShadowMap();
    {
        render::Pass pass(frame, nullptr, render::Pass::Raster);
        GBufferPass& g = *m_gbufferPass;
        pass.sampled(g.position()).sampled(g.normal()).sampled(g.albedo()).sampled(g.emissive());
        pass.sampled(g.specular()).sampled(g.openpbr()).sampled(g.layers());
        if (shadowMap) pass.sampled(*shadowMap);
        pass.sampled(m_dummyImage);
        if (m_lightBuffer) pass.read(*m_lightBuffer, render::BufferUse::Storage);
        if (m_clusterBuffer) pass.read(*m_clusterBuffer, render::BufferUse::Storage);
        if (m_cascadeBuffer) pass.read(*m_cascadeBuffer, render::BufferUse::Uniform);
        pass.read(m_dummyBuffer, render::BufferUse::Storage);
        for (gpu::Image* traced : {m_ssgi, m_giReflection, m_directDiffuse, m_directSpecular})
            if (traced) pass.sampled(*traced);
        if (m_materialBuffer) pass.read(*m_materialBuffer, render::BufferUse::Storage);
        // LEGACY(step 4a): the environment's maps are EnvPrefilter's and OpenPBR's tables an OpenPBRTablesBuffer:
        // not tracked yet.
        VkClearColorValue noSubsurface{};
        noSubsurface.uint32[0] = noSubsurface.uint32[1] = noSubsurface.uint32[2] = noSubsurface.uint32[3] = 0u;
        pass.color(m_lit, render::Load::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}})
            .color(m_subsurfaceDiffuse, render::Load::Clear, noSubsurface);
        pass.begin();

        // Bind pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        // Bind descriptor set (G-Buffer textures, lights, shadows, IBL)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_descriptorSets[frameIndex], 0, nullptr);

        // Push constants
        m_params.screenSize = glm::vec2(m_width, m_height);
        m_params.lightCount = m_lightCount;
        m_params.flags = 0;
        // IBL (bit 0): the prefiltered environment and its irradiance are both bound.
        if (m_prefilteredView != VK_NULL_HANDLE && m_irradianceView != VK_NULL_HANDLE)
            m_params.flags |= 1;
        if (m_directDiffuse)
            m_params.flags |= 32;  // RT direct light (bit 5)
        else if (shadowMap)
            m_params.flags |= 4;   // CSM fallback (bit 2)
        if (m_ssgi) m_params.flags |= 8;       // RT GI
        if (!m_clusterBuffer) m_params.flags |= 64;                  // every light (no froxel lists)

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LightingParams), &m_params);

        // Draw fullscreen triangle
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}

void DeferredLightingPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    destroyOutputImages();
    if (!createOutputImages())
        OHAO_LOG(Error, render, "DeferredLightingPass: failed to recreate its outputs at {}x{}", width, height);
}

void DeferredLightingPass::setShadowMap(gpu::Image* shadowMap, VkSampler shadowSampler) {
    m_shadowMap = shadowMap;
    m_shadowSampler = shadowSampler;
}

void DeferredLightingPass::setEnvironment(VkImageView prefiltered, VkImageView irradiance, VkSampler sampler,
                                          float intensity) {
    m_prefilteredView = prefiltered;
    m_irradianceView = irradiance;
    m_iblSampler = sampler;
    m_params.envIntensity = intensity;
}

void DeferredLightingPass::setSSGITexture(gpu::Image* ssgi, gpu::Image* reflection, VkSampler ssgiSampler) {
    const bool both = ssgi && *ssgi && reflection && *reflection;
    m_ssgi = both ? ssgi : nullptr;
    m_giReflection = both ? reflection : nullptr;
    m_ssgiSampler = ssgiSampler;
}

void DeferredLightingPass::setDirectLight(gpu::Image* diffuse, gpu::Image* specular) {
    // record() sets the flags from them
    const bool both = diffuse && *diffuse && specular && *specular;
    m_directDiffuse = both ? diffuse : nullptr;
    m_directSpecular = both ? specular : nullptr;
}

void DeferredLightingPass::setCameraData(const glm::vec3& position, const glm::mat4& view,
                                          const glm::mat4& invViewProj) {
    m_params.cameraPos = position;
    m_params.view = view;
    m_params.invViewProj = invViewProj;
}

void DeferredLightingPass::clearPerFrameInputs() {
    m_ssgi = nullptr;
    m_giReflection = nullptr;
    m_ssgiSampler = VK_NULL_HANDLE;
    m_directDiffuse = nullptr;
    m_directSpecular = nullptr;
}

void DeferredLightingPass::setGBufferPass(GBufferPass* gbufferPass) {
    m_gbufferPass = gbufferPass;
}

void DeferredLightingPass::writeDescriptorSet(uint32_t slot) {
    const VkDescriptorSet set = m_descriptorSets[slot];
    if (!m_gbufferPass || set == VK_NULL_HANDLE || m_gbufferSampler == VK_NULL_HANDLE) return;

    // Always write every binding, dummy resources for the unbound ones: Vulkan requires every
    // declared binding written before the set is used.
    VkImageView fallbackView = m_dummyImage.view;
    VkSampler fallbackSampler = m_gbufferSampler;

    namespace L = shader::core_deferred_lighting;
    std::array<VkDescriptorImageInfo, L::kSet0.size()> images{};
    std::array<VkDescriptorBufferInfo, L::kSet0.size()> buffers{};
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(L::kSet0.size());
    const auto image = [&](const shader::Binding& b, VkImageView view, VkSampler sampler,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkDescriptorImageInfo& i = images[writes.size()];
        i = {sampler, view != VK_NULL_HANDLE ? view : fallbackView, layout};
        VkWriteDescriptorSet w = shader::write(b, set);
        w.pImageInfo = &i;
        writes.push_back(w);
    };
    const auto buffer = [&](const shader::Binding& b, VkBuffer buf) {
        VkDescriptorBufferInfo& i = buffers[writes.size()];
        i = {buf != VK_NULL_HANDLE ? buf : m_dummyBuffer.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = shader::write(b, set);
        w.pBufferInfo = &i;
        writes.push_back(w);
    };

    // The G-buffer, always there after its init.
    const auto tracked = [](const GpuBuffer* b) { return b ? b->buffer : VkBuffer{VK_NULL_HANDLE}; };
    image(L::gBuffer0, m_gbufferPass->position().view, m_gbufferSampler);
    image(L::gBuffer1, m_gbufferPass->normal().view, m_gbufferSampler);
    image(L::gBuffer2, m_gbufferPass->albedo().view, m_gbufferSampler);
    image(L::gBufferEmissive, m_gbufferPass->emissive().view, m_gbufferSampler);
    image(L::gBufferSpecular, m_gbufferPass->specular().view, m_gbufferSampler);   // OpenPBR's inputs
    image(L::gBufferOpenPBR, m_gbufferPass->openpbr().view, m_gbufferSampler);
    image(L::gBufferLayers, m_gbufferPass->layers().view, m_gbufferSampler);       // OpenPBR's layers (uint)
    // The light table and the froxel lists; the dummy buffer without lights (the count is 0 then).
    if (tracked(m_lightBuffer) == VK_NULL_HANDLE) m_lightCount = 0;
    buffer(L::lights, tracked(m_lightBuffer));
    buffer(L::clusters, tracked(m_clusterBuffer));
    // The CSM fallback: its map (a depth image: sampled in the layout it is tested in) and its cascades.
    gpu::Image* const shadowMap = boundShadowMap();
    image(L::shadowMap, shadowMap ? shadowMap->view : VK_NULL_HANDLE,
          m_shadowSampler != VK_NULL_HANDLE ? m_shadowSampler : fallbackSampler,
          shadowMap ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    buffer(L::cascades, tracked(m_cascadeBuffer));
    // Image-based lighting: the environment's irradiance and GGX-prefiltered levels (flag bit 0 clear without).
    const VkSampler iblSampler = m_iblSampler != VK_NULL_HANDLE ? m_iblSampler : fallbackSampler;
    image(L::envIrradiance, m_irradianceView, iblSampler);
    image(L::envPrefiltered, m_prefilteredView, iblSampler);
    // RT GI's diffuse and reflection, RT direct light's diffuse and specular (or stand-ins: their flags clear).
    const VkSampler giSampler = m_ssgiSampler != VK_NULL_HANDLE ? m_ssgiSampler : fallbackSampler;
    const auto viewOf = [](const gpu::Image* i) { return i ? i->view : VkImageView{VK_NULL_HANDLE}; };
    image(L::ssgiTexture, viewOf(m_ssgi), giSampler);
    image(L::giReflection, viewOf(m_giReflection), giSampler);
    image(L::directDiffuse, viewOf(m_directDiffuse), m_gbufferSampler);
    image(L::directSpecular, viewOf(m_directSpecular), m_gbufferSampler);
    // OpenPBR's energy tables and the material table the layers index.
    buffer(L::openpbrTables, m_openpbrTables.buffer());
    buffer(L::layerMaterials, tracked(m_materialBuffer));

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool DeferredLightingPass::createOutputImages() {
    m_lit = m_allocator->createImage(
        {"lighting.lit", kLitFormat, m_width, m_height,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |   // final image when post-processing is off: readback copy / swapchain blit
             VK_IMAGE_USAGE_STORAGE_BIT});       // SSS blurs skin tiles in place
    // The subsurface pixels' diffuse irradiance and albedo, which the SSS pass spreads alone.
    m_subsurfaceDiffuse = m_allocator->createImage({"lighting.subsurfaceDiffuse", kSubsurfaceDiffuseFormat, m_width, m_height,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    return m_lit && m_subsurfaceDiffuse;
}

void DeferredLightingPass::destroyOutputImages() {
    if (!m_allocator) return;
    m_allocator->retire(m_lit);
    m_allocator->retire(m_subsurfaceDiffuse);
}

bool DeferredLightingPass::createDescriptors() {
    // Bindings (every one read by the fragment stage):
    // 0-4: G-Buffer (position, normal, albedo, velocity, depth)
    // 5: The light table (SSBO, LightGpu records)
    // 6: Shadow map array (sampler2DArray, 4 cascades)
    // 7: The environment's irradiance (EnvPrefilter)
    const auto bindings = shader::layoutBindings(shader::core_deferred_lighting::kSet0,
                                                 shader::core_deferred_lighting::kStages);
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool — 16 image samplers, 1 UBO (cascade data), 4 SSBOs (lights, froxels, OpenPBR's
    // tables, the materials)
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16 * MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // One set per frame in flight, all with the same layout
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_descriptorLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    return vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) == VK_SUCCESS;
}

bool DeferredLightingPass::createPipeline() {
    // Push constants
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(LightingParams);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    // A fullscreen triangle; unblended writes: the lit image, the subsurface pixels' diffuse light
    const std::array<VkFormat, 2> formats{kLitFormat, kSubsurfaceDiffuseFormat};
    render::GraphicsPipelineDesc desc{};
    desc.name = "lighting";
    desc.vertex = "postprocess_fullscreen.slang.spv";
    desc.fragment = "core_deferred_lighting.slang.spv";
    desc.layout = m_pipelineLayout;
    desc.colorFormats = formats;
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

bool DeferredLightingPass::createDummyResources() {
    // 1x1 RGBA8 image for fallback texture bindings
    m_dummyImage = m_allocator->createImage({"lighting.dummy", VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT});
    if (!m_dummyImage) return false;

    // 512 zeroed bytes for the SSBO and UBO fallbacks (CascadeData is 288 bytes).
    m_dummyBuffer = m_allocator->createBuffer(512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                              AllocationUsage::CpuToGpu, true);
    if (!m_dummyBuffer.isValid() || !m_dummyBuffer.getMappedData()) return false;
    memset(m_dummyBuffer.getMappedData(), 0, 512);
    m_allocator->flushBuffer(m_dummyBuffer);
    return true;
}

void DeferredLightingPass::destroyDummyResources() {
    if (!m_allocator) return;
    m_allocator->retire(m_dummyImage);
    m_allocator->retire(m_dummyBuffer);
}

} // namespace ohao
