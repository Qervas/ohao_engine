#include "water_pass.hpp"
#include <array>
#include <vector>
#include <iostream>
#include <cstring>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WaterPass::~WaterPass() {
    cleanup();
}

bool WaterPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createSamplers()) {
        std::cerr << "WaterPass: sampler creation failed" << std::endl;
        return false;
    }
    if (!createRenderPass()) {
        std::cerr << "WaterPass: render pass creation failed" << std::endl;
        return false;
    }
    if (!createDescriptors()) {
        std::cerr << "WaterPass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipeline()) {
        std::cerr << "WaterPass: pipeline creation failed" << std::endl;
        return false;
    }
    // Grid mesh upload requires a command buffer; we defer to execute() on first run.
    // createGridMesh() just allocates GPU buffers and does a CPU-side upload via
    // staging buffers that are flushed immediately using a one-shot command buffer.

    std::cout << "WaterPass: OK" << std::endl;
    return true;
}

void WaterPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    destroyGridMesh();
    destroyFramebuffer();

    safeDestroy(m_dummyView);
    safeDestroy(m_dummyImage);
    safeFree(m_dummyMemory);

    safeDestroy(m_dummyCubeView);
    safeDestroy(m_dummyCubeImage);
    safeFree(m_dummyCubeMemory);

    safeDestroy(m_pipelineFFT);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_renderPass);
    safeDestroy(m_depthSampler);
    safeDestroy(m_linearSampler);
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void WaterPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled) return;
    if (m_framebuffer    == VK_NULL_HANDLE) return;
    if (m_pipeline       == VK_NULL_HANDLE) return;
    if (m_descriptorSet  == VK_NULL_HANDLE) return;
    if (m_hdrView        == VK_NULL_HANDLE) return;
    if (m_depthView      == VK_NULL_HANDLE) return;
    if (m_indexCount     == 0)             return;

    // Upload grid mesh on first execute (deferred from initialize to avoid
    // needing an external command buffer at init time).
    if (!m_resourcesUploaded) {
        if (!createGridMesh()) {
            std::cerr << "WaterPass: grid mesh upload failed, disabling" << std::endl;
            m_enabled = false;
            return;
        }
        m_resourcesUploaded = true;
    }

    // Refresh descriptors whenever external resources changed (e.g. after resize).
    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }

    // --- Transition HDR from SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL ---
    // The SkyPass / weather passes / particle pass leave the HDR image in
    // SHADER_READ_ONLY_OPTIMAL.  Our render pass uses initialLayout =
    // SHADER_READ_ONLY_OPTIMAL and the subpass dependencies handle the automatic
    // transition, so no explicit pre-barrier is needed here — the render pass
    // will insert it.  We just need to make sure we match the layout.

    // --- Begin render pass ---
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {m_width, m_height};
    rpInfo.clearValueCount   = 0;  // LOAD_OP_LOAD — no clear needed for color or depth

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport / scissor
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<float>(m_width);
    vp.height   = static_cast<float>(m_height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Select pipeline based on wave mode (FFT or Gerstner)
    bool useFFT = (m_waveSim != nullptr && m_waveSim->providesTextures()
                   && m_pipelineFFT != VK_NULL_HANDLE);
    VkPipeline activePipeline = useFFT ? m_pipelineFFT : m_pipeline;

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    // Bind descriptor set 0
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants (both stages)
    WaterPC pc{};
    pc.viewProj    = m_viewProj;
    pc.invViewProj = m_invViewProj;
    pc.cameraPos   = glm::vec4(m_cameraPos, m_waterLevel);
    pc.waterParams = glm::vec4(m_waterSize, m_time, m_waveAmplitude, m_foamIntensity);
    pc.sunParams   = glm::vec4(m_sunDir, m_sunIntensity);
    pc.shallowColor = glm::vec4(m_shallowColor, m_sssStrength);
    pc.deepColor    = glm::vec4(m_deepColor, m_rippleNormalStrength);
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WaterPC), &pc);

    // Bind vertex and index buffers
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Draw the grid
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void WaterPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    // Framebuffer is recreated by setHDROutput() called from DeferredRenderer::onResize().
    destroyFramebuffer();
}

// ---------------------------------------------------------------------------
// Connection setters
// ---------------------------------------------------------------------------

void WaterPass::setHDROutput(VkImageView view, VkImage image) {
    m_hdrView    = view;
    m_hdrImage   = image;
    if (m_renderPass != VK_NULL_HANDLE && m_hdrView != VK_NULL_HANDLE
            && m_depthView != VK_NULL_HANDLE) {
        destroyFramebuffer();
        createFramebuffer();
    }
    m_descriptorDirty = true;
}

void WaterPass::setDepthBuffer(VkImageView view, VkSampler sampler) {
    m_depthView        = view;
    m_depthSamplerExt  = sampler;
    if (m_renderPass != VK_NULL_HANDLE && m_hdrView != VK_NULL_HANDLE
            && m_depthView != VK_NULL_HANDLE) {
        destroyFramebuffer();
        createFramebuffer();
    }
    m_descriptorDirty = true;
}

void WaterPass::setNormalMaps(VkImageView nm1, VkImageView nm2, VkSampler sampler) {
    m_normalMap1     = nm1;
    m_normalMap2     = nm2;
    m_normalSampler  = sampler;
    m_descriptorDirty = true;
}

void WaterPass::setIBL(VkImageView prefiltered, VkImageView brdfLUT, VkSampler sampler) {
    m_iblPrefiltered  = prefiltered;
    m_iblBrdfLUT      = brdfLUT;
    m_iblSampler      = sampler;
    m_descriptorDirty = true;
}

void WaterPass::setSceneColor(VkImageView view) {
    m_sceneColorView  = view;
    m_descriptorDirty = true;
}

void WaterPass::setSSROutput(VkImageView view) {
    m_ssrView         = view;
    m_descriptorDirty = true;
}

void WaterPass::setFoamTexture(VkImageView view, VkSampler sampler) {
    m_foamTexView     = view;
    m_foamSampler     = sampler;
    m_descriptorDirty = true;
}

void WaterPass::setRippleMap(VkImageView view) {
    m_rippleMapView   = view;
    m_descriptorDirty = true;
}

void WaterPass::setWaveSim(IWaveSim* sim) {
    m_waveSim = sim;
    if (sim && sim->providesTextures()) {
        m_fftDisplacementView = sim->getDisplacementView();
        m_fftNormalView       = sim->getNormalView();
    } else {
        m_fftDisplacementView = VK_NULL_HANDLE;
        m_fftNormalView       = VK_NULL_HANDLE;
    }
    m_descriptorDirty = true;
}

void WaterPass::setSunDirection(const glm::vec3& dir, float intensity) {
    m_sunDir       = dir;
    m_sunIntensity = glm::max(intensity, 0.0f);
}

void WaterPass::setWaterColors(const glm::vec3& shallow, const glm::vec3& deep) {
    m_shallowColor = shallow;
    m_deepColor    = deep;
}

void WaterPass::setMatrices(const glm::mat4& viewProj,
                             const glm::mat4& invViewProj,
                             const glm::vec3& camPos) {
    m_viewProj    = viewProj;
    m_invViewProj = invViewProj;
    m_cameraPos   = camPos;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool WaterPass::createSamplers() {
    // Depth sampler: NEAREST, CLAMP — avoids filtering artefacts on depth values.
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &si, nullptr, &m_depthSampler) != VK_SUCCESS)
            return false;
    }

    // Linear repeat sampler: for normal maps and IBL (bilinear, repeat UVs).
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(m_device, &si, nullptr, &m_linearSampler) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool WaterPass::createRenderPass() {
    // Attachment 0: HDR color output.
    //   LOAD_OP_LOAD  — preserve deferred-lit + sky pixels.
    //   initialLayout = SHADER_READ_ONLY_OPTIMAL (left by previous pass).
    //   finalLayout   = SHADER_READ_ONLY_OPTIMAL (needed by post-processing).
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Attachment 1: GBuffer depth (read-only — depth test enabled, writes disabled).
    //   We use DEPTH_STENCIL_READ_ONLY_OPTIMAL so we can both depth-test the water
    //   mesh against existing scene geometry AND sample the depth in the fragment
    //   shader for shore foam.  No writes ever go to the depth buffer.
    VkAttachmentDescription depthAtt{};
    depthAtt.format         = VK_FORMAT_D32_SFLOAT;  // matches GBufferPass depth format
    depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Subpass dependency 0: previous pass (SHADER_READ) → us (COLOR_ATTACHMENT_WRITE)
    // Covers the implicit layout transition SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    deps[0].dependencyFlags = 0;

    // Subpass dependency 1: us (COLOR_ATTACHMENT_WRITE) → next pass (SHADER_READ)
    // Covers the implicit layout transition COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = 0;

    std::array<VkAttachmentDescription, 2> attachments = {colorAtt, depthAtt};

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool WaterPass::createFramebuffer() {
    if (m_hdrView == VK_NULL_HANDLE || m_depthView == VK_NULL_HANDLE) return false;
    if (m_renderPass == VK_NULL_HANDLE) return false;

    std::array<VkImageView, 2> attachments = {m_hdrView, m_depthView};

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_renderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbInfo.pAttachments    = attachments.data();
    fbInfo.width           = m_width;
    fbInfo.height          = m_height;
    fbInfo.layers          = 1;

    return vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

void WaterPass::destroyFramebuffer() {
    safeDestroy(m_framebuffer);
}

bool WaterPass::createDescriptors() {
    // 11 bindings — both pipelines share this layout:
    //   0-8: COMBINED_IMAGE_SAMPLER, FRAGMENT_BIT (same as before)
    //   9:   COMBINED_IMAGE_SAMPLER, VERTEX_BIT   — FFT displacement map
    //  10:   COMBINED_IMAGE_SAMPLER, VERTEX_BIT   — FFT normal map
    //
    //   0: sceneDepth   — GBuffer depth for shore foam
    //   1: normalMap1   — scrolling detail normal #1
    //   2: normalMap2   — scrolling detail normal #2
    //   3: envMap       — IBL prefiltered env cube
    //   4: brdfLUT      — IBL BRDF split-sum LUT
    //   5: sceneColor   — HDR scene color for refraction
    //   6: ssrOutput    — SSR screen-space reflections
    //   7: foamTex      — animated bubble/foam noise
    //   8: rippleMap    — GPU ripple height map (R16F)
    //   9: displacementMap — FFT IFFT result (vertex stage)
    //  10: fftNormalMap    — FFT normal + foam (vertex stage)
    std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
    for (uint32_t i = 0; i < 9; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (uint32_t i = 9; i < 11; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                    &m_descriptorLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 11;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;
    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

void WaterPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // Resolve which sampler to use for depth (prefer caller-supplied).
    VkSampler depthSampler = (m_depthSamplerExt != VK_NULL_HANDLE)
                             ? m_depthSamplerExt : m_depthSampler;

    // Resolve fallback views for bindings that have not been set.
    // The critical rule: ALL declared bindings MUST be written before use — otherwise
    // uninitialized bindings cause SEGV / VK_ERROR_DEVICE_LOST on NVIDIA hardware.

    // Create dummy resources lazily if needed (1×1 black 2D and cube images).
    if (m_dummyImage == VK_NULL_HANDLE) {
        // 1×1 R8G8B8A8_UNORM 2D image
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent        = {1, 1, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                              | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imgInfo, nullptr, &m_dummyImage) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(m_device, m_dummyImage, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize  = req.size;
            alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(m_device, &alloc, nullptr, &m_dummyMemory);
            vkBindImageMemory(m_device, m_dummyImage, m_dummyMemory, 0);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image                           = m_dummyImage;
            viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format                          = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel   = 0;
            viewInfo.subresourceRange.levelCount     = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount     = 1;
            vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyView);
        }
    }

    if (m_dummyCubeImage == VK_NULL_HANDLE) {
        // 1×1 cube map (6 faces)
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent        = {1, 1, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 6;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                              | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imgInfo, nullptr, &m_dummyCubeImage) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(m_device, m_dummyCubeImage, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize  = req.size;
            alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(m_device, &alloc, nullptr, &m_dummyCubeMemory);
            vkBindImageMemory(m_device, m_dummyCubeImage, m_dummyCubeMemory, 0);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image                           = m_dummyCubeImage;
            viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
            viewInfo.format                          = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel   = 0;
            viewInfo.subresourceRange.levelCount     = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount     = 6;
            vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyCubeView);
        }
    }

    // Resolve sampler for normal maps
    VkSampler nmSampler   = (m_normalSampler  != VK_NULL_HANDLE) ? m_normalSampler  : m_linearSampler;
    VkSampler iblSampler  = (m_iblSampler     != VK_NULL_HANDLE) ? m_iblSampler     : m_linearSampler;
    VkImageView nm1View   = (m_normalMap1     != VK_NULL_HANDLE) ? m_normalMap1     : m_dummyView;
    VkImageView nm2View   = (m_normalMap2     != VK_NULL_HANDLE) ? m_normalMap2     : m_dummyView;
    VkImageView envView   = (m_iblPrefiltered != VK_NULL_HANDLE) ? m_iblPrefiltered : m_dummyCubeView;
    VkImageView brdfView  = (m_iblBrdfLUT     != VK_NULL_HANDLE) ? m_iblBrdfLUT     : m_dummyView;
    VkImageView sceneView   = (m_sceneColorView != VK_NULL_HANDLE) ? m_sceneColorView : m_dummyView;
    VkImageView ssrView     = (m_ssrView        != VK_NULL_HANDLE) ? m_ssrView        : m_dummyView;
    VkImageView foamView    = (m_foamTexView    != VK_NULL_HANDLE) ? m_foamTexView    : m_dummyView;
    VkSampler   foamSampler = (m_foamSampler    != VK_NULL_HANDLE) ? m_foamSampler    : m_linearSampler;
    // rippleMap sampler: CLAMP to avoid wrapping artefacts at ocean edge
    VkImageView rippleView  = (m_rippleMapView  != VK_NULL_HANDLE) ? m_rippleMapView  : m_dummyView;

    // If any fallback view is still null (dummy creation failed), bail out.
    if (!nm1View || !nm2View || !envView || !brdfView || !m_depthView) return;
    if (!depthSampler || !nmSampler || !iblSampler) return;

    // Bindings 9+10: FFT displacement/normal maps, or dummy when in Gerstner mode.
    VkImageView fftDispView  = (m_fftDisplacementView != VK_NULL_HANDLE)
                               ? m_fftDisplacementView : m_dummyView;
    VkImageView fftNormView  = (m_fftNormalView       != VK_NULL_HANDLE)
                               ? m_fftNormalView       : m_dummyView;
    // Fallback guard: dummy might still be null if creation failed.
    if (!fftDispView) fftDispView = m_dummyView;
    if (!fftNormView) fftNormView = m_dummyView;

    // Write all 11 bindings.
    std::array<VkDescriptorImageInfo, 11> imageInfos{};
    imageInfos[0]  = {depthSampler,    m_depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    imageInfos[1]  = {nmSampler,       nm1View,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[2]  = {nmSampler,       nm2View,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[3]  = {iblSampler,      envView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[4]  = {iblSampler,      brdfView,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[5]  = {m_linearSampler, sceneView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[6]  = {m_linearSampler, ssrView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[7]  = {foamSampler,     foamView,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[8]  = {m_linearSampler, rippleView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[9]  = {m_linearSampler, fftDispView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imageInfos[10] = {m_linearSampler, fftNormView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    std::array<VkWriteDescriptorSet, 11> writes{};
    for (uint32_t i = 0; i < 11; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool WaterPass::createPipeline() {
    VkShaderModule vertShader    = loadShaderModule("water_water.vert.spv");
    VkShaderModule vertShaderFFT = loadShaderModule("water_water_fft.vert.spv");
    VkShaderModule fragShader    = loadShaderModule("water_water.frag.spv");
    if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
        if (vertShader)    vkDestroyShaderModule(m_device, vertShader,    nullptr);
        if (vertShaderFFT) vkDestroyShaderModule(m_device, vertShaderFFT, nullptr);
        if (fragShader)    vkDestroyShaderModule(m_device, fragShader,    nullptr);
        return false;
    }
    // FFT vert is optional — failure just disables FFT mode gracefully.
    bool haveFFT = (vertShaderFFT != VK_NULL_HANDLE);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName  = "main";

    // Vertex input: one binding, one attribute — vec2 XZ position.
    VkVertexInputBindingDescription vtxBinding{};
    vtxBinding.binding   = 0;
    vtxBinding.stride    = sizeof(float) * 2;
    vtxBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vtxAttr{};
    vtxAttr.location = 0;
    vtxAttr.binding  = 0;
    vtxAttr.format   = VK_FORMAT_R32G32_SFLOAT;
    vtxAttr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &vtxBinding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &vtxAttr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (LESS), depth write OFF.
    // The GBuffer depth already captures opaque scene geometry; we test against it
    // but do not write new depth values (water is semi-transparent).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable   = VK_TRUE;
    depthStencil.depthWriteEnable  = VK_FALSE;
    depthStencil.depthCompareOp    = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Alpha blending: SRC_ALPHA / ONE_MINUS_SRC_ALPHA for transparency.
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable         = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                 | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &blendAtt;

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    // Push constants: used in both vertex and fragment stages.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(WaterPC);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                                &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, vertShader, nullptr);
    if (result != VK_SUCCESS) {
        if (vertShaderFFT) vkDestroyShaderModule(m_device, vertShaderFFT, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Build FFT pipeline (same state, swaps in water_fft.vert)
    if (haveFFT) {
        stages[0].module = vertShaderFFT;  // swap vert, keep frag
        pipelineInfo.pStages = stages.data();
        VkResult fftResult = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                                       &pipelineInfo, nullptr, &m_pipelineFFT);
        vkDestroyShaderModule(m_device, vertShaderFFT, nullptr);
        if (fftResult != VK_SUCCESS) {
            // Non-fatal: Gerstner pipeline is still usable.
            std::cerr << "WaterPass: FFT pipeline creation failed (FFT mode disabled)" << std::endl;
            m_pipelineFFT = VK_NULL_HANDLE;
        }
    } else {
        if (vertShaderFFT) vkDestroyShaderModule(m_device, vertShaderFFT, nullptr);
    }

    vkDestroyShaderModule(m_device, fragShader, nullptr);
    return true;
}

bool WaterPass::createGridMesh() {
    // Generate a GRID_N × GRID_N quad grid.
    // Each vertex is a vec2 XZ position in normalized [-0.5, 0.5] space.
    // The vertex shader converts to world space using waterSize.
    //
    // Layout: (GRID_N+1)×(GRID_N+1) vertices, GRID_N×GRID_N×2 triangles.

    const uint32_t N     = GRID_N;
    const uint32_t verts = (N + 1) * (N + 1);
    const uint32_t tris  = N * N * 2;
    const uint32_t idxCount = tris * 3;

    std::vector<float>    vtxData(verts * 2);
    std::vector<uint32_t> idxData(idxCount);

    // Fill vertex positions
    for (uint32_t j = 0; j <= N; ++j) {
        for (uint32_t i = 0; i <= N; ++i) {
            uint32_t idx = (j * (N + 1) + i) * 2;
            vtxData[idx + 0] = (static_cast<float>(i) / static_cast<float>(N)) - 0.5f;
            vtxData[idx + 1] = (static_cast<float>(j) / static_cast<float>(N)) - 0.5f;
        }
    }

    // Fill index buffer (CCW winding looking from +Y down)
    uint32_t idx = 0;
    for (uint32_t j = 0; j < N; ++j) {
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t bl = j * (N + 1) + i;
            uint32_t br = bl + 1;
            uint32_t tl = bl + (N + 1);
            uint32_t tr = tl + 1;
            // Triangle 1: bottom-left, bottom-right, top-right
            idxData[idx++] = bl;
            idxData[idx++] = br;
            idxData[idx++] = tr;
            // Triangle 2: bottom-left, top-right, top-left
            idxData[idx++] = bl;
            idxData[idx++] = tr;
            idxData[idx++] = tl;
        }
    }
    m_indexCount = idxCount;

    VkDeviceSize vtxSize = vtxData.size() * sizeof(float);
    VkDeviceSize idxSize = idxData.size() * sizeof(uint32_t);

    // Create GPU-side vertex buffer
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = vtxSize;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_vertexBuffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_vertexMemory) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexMemory, 0);
    }

    // Create GPU-side index buffer
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = idxSize;
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_indexBuffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_indexBuffer, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_indexMemory) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(m_device, m_indexBuffer, m_indexMemory, 0);
    }

    // Staging: upload vertex data
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = vtxSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_stagingVtx) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_stagingVtx, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_stagingVtxMem) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(m_device, m_stagingVtx, m_stagingVtxMem, 0);

        void* mapped = nullptr;
        vkMapMemory(m_device, m_stagingVtxMem, 0, vtxSize, 0, &mapped);
        std::memcpy(mapped, vtxData.data(), static_cast<size_t>(vtxSize));
        vkUnmapMemory(m_device, m_stagingVtxMem);
    }

    // Staging: upload index data
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = idxSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_stagingIdx) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_stagingIdx, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_stagingIdxMem) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(m_device, m_stagingIdx, m_stagingIdxMem, 0);

        void* mapped = nullptr;
        vkMapMemory(m_device, m_stagingIdxMem, 0, idxSize, 0, &mapped);
        std::memcpy(mapped, idxData.data(), static_cast<size_t>(idxSize));
        vkUnmapMemory(m_device, m_stagingIdxMem);
    }

    // Execute copy via immediate command buffer.
    // We need a queue for this — find the graphics queue.
    // Since RenderPassBase does not expose the queue, we use vkDeviceWaitIdle +
    // a dedicated one-shot pattern using the device queue family 0.
    // The caller (DeferredRenderer) guarantees the device is idle at init time.
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, 0, 0, &queue);

    // Create a transient command pool for the upload.
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpInfo{};
        cpInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpInfo.queueFamilyIndex = 0;
        cpInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(m_device, &cpInfo, nullptr, &cmdPool) != VK_SUCCESS)
            return false;
    }

    VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool        = cmdPool;
        cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &cbAlloc, &uploadCmd) != VK_SUCCESS) {
            vkDestroyCommandPool(m_device, cmdPool, nullptr);
            return false;
        }
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(uploadCmd, &beginInfo);

    VkBufferCopy cpyVtx{};
    cpyVtx.size = vtxSize;
    vkCmdCopyBuffer(uploadCmd, m_stagingVtx, m_vertexBuffer, 1, &cpyVtx);

    VkBufferCopy cpyIdx{};
    cpyIdx.size = idxSize;
    vkCmdCopyBuffer(uploadCmd, m_stagingIdx, m_indexBuffer, 1, &cpyIdx);

    vkEndCommandBuffer(uploadCmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &uploadCmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // Free staging resources and command pool.
    vkDestroyCommandPool(m_device, cmdPool, nullptr);
    safeDestroy(m_stagingVtx);
    safeFree(m_stagingVtxMem);
    safeDestroy(m_stagingIdx);
    safeFree(m_stagingIdxMem);

    return true;
}

void WaterPass::destroyGridMesh() {
    // Staging (should already be freed after upload, but clean up if upload failed)
    safeDestroy(m_stagingVtx);
    safeFree(m_stagingVtxMem);
    safeDestroy(m_stagingIdx);
    safeFree(m_stagingIdxMem);

    safeDestroy(m_vertexBuffer);
    safeFree(m_vertexMemory);
    safeDestroy(m_indexBuffer);
    safeFree(m_indexMemory);
    m_indexCount = 0;
    m_resourcesUploaded = false;
}

} // namespace ohao
