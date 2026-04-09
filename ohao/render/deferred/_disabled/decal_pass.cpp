#include "decal_pass.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include <array>
#include <iostream>
#include <cstring>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DecalPass::~DecalPass() {
    cleanup();
}

bool DecalPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createDecalBuffer()) {
        std::cerr << "DecalPass: failed to create decal SSBO\n";
        return false;
    }
    if (!createUnitCubeMesh()) {
        std::cerr << "DecalPass: failed to create unit cube mesh\n";
        return false;
    }
    if (!createDescriptors()) {
        std::cerr << "DecalPass: failed to create descriptors\n";
        return false;
    }
    // RenderPass is created lazily in setGBufferAlbedo() once the format is known.
    // Pipeline is created in createPipeline(), called from setGBufferAlbedo().

    std::cout << "DecalPass: initialised (render pass deferred until albedo view set)\n";
    return true;
}

void DecalPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    destroyFramebuffer();
    safeDestroy(m_renderPass);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);

    safeDestroy(m_descriptorPool0);
    safeDestroy(m_descriptorLayout0);
    m_descriptorSet0 = VK_NULL_HANDLE;

    // Cube VB
    if (m_cubeVBMem != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_cubeVBMem, nullptr);
        m_cubeVBMem = VK_NULL_HANDLE;
    }
    safeDestroy(m_cubeVB);

    // Cube IB
    if (m_cubeIBMem != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_cubeIBMem, nullptr);
        m_cubeIBMem = VK_NULL_HANDLE;
    }
    safeDestroy(m_cubeIB);

    // Decal SSBO
    if (m_decalMapped != nullptr && m_decalMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(m_device, m_decalMemory);
        m_decalMapped = nullptr;
    }
    if (m_decalMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_decalMemory, nullptr);
        m_decalMemory = VK_NULL_HANDLE;
    }
    safeDestroy(m_decalBuffer);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void DecalPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    // Framebuffer is rebuilt externally via setGBufferAlbedo() after resize.
    destroyFramebuffer();
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void DecalPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_decals.empty()) return;
    if (m_framebuffer  == VK_NULL_HANDLE) return;
    if (m_renderPass   == VK_NULL_HANDLE) return;
    if (m_pipeline     == VK_NULL_HANDLE) return;
    if (m_descriptorSet0 == VK_NULL_HANDLE) return;

    // Upload decal data to GPU if changed
    if (m_decalsDirty) {
        uploadDecals();
        m_decalsDirty = false;
    }

    // Refresh descriptor set if depth view changed
    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }

    // --- Begin render pass (LOAD_OP_LOAD on GBuffer albedo) ---
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {m_width, m_height};
    rpInfo.clearValueCount   = 0;  // LOAD_OP_LOAD — nothing to clear

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport / scissor
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

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Bind set 0 (SSBO + depth sampler)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet0, 0, nullptr);

    // Bind set 1 (bindless textures) if available
    if (m_bindlessMgr != nullptr) {
        VkDescriptorSet bindlessSet = m_bindlessMgr->getDescriptorSet();
        if (bindlessSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 1, 1, &bindlessSet, 0, nullptr);
        }
    }

    // Push constants
    PushConstants pc{};
    pc.viewProj    = m_viewProj;
    pc.invViewProj = m_invViewProj;
    pc.screenSize  = m_screenSize;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // Bind unit cube vertex + index buffers
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_cubeVB, &offset);
    vkCmdBindIndexBuffer(cmd, m_cubeIB, 0, VK_INDEX_TYPE_UINT16);

    // One instanced draw: 36 indices × N decal instances
    vkCmdDrawIndexed(cmd, CUBE_INDEX_COUNT,
                     static_cast<uint32_t>(m_decals.size()),
                     0, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// Decal lifecycle
// ---------------------------------------------------------------------------

uint32_t DecalPass::addDecal(const DecalDesc& desc) {
    if (m_decals.size() >= MAX_DECALS) {
        std::cerr << "DecalPass: MAX_DECALS (" << MAX_DECALS << ") reached\n";
        return 0;
    }

    uint32_t handle = m_nextHandle++;
    if (m_nextHandle == 0) m_nextHandle = 1;  // wrap, skip 0

    DecalGPU gpu{};
    gpu.decalMatrix    = desc.decalMatrix;
    gpu.worldMatrix    = desc.worldMatrix;
    gpu.colorTint      = desc.colorTint;
    gpu.albedoIdx      = desc.albedoIdx;
    gpu.normalIdx      = desc.normalIdx;
    gpu.opacity        = desc.opacity;
    gpu.roughnessScale = desc.roughnessScale;

    m_handleToIndex[handle] = m_decals.size();
    m_decals.push_back(gpu);
    m_decalsDirty = true;

    return handle;
}

void DecalPass::removeDecal(uint32_t handle) {
    auto it = m_handleToIndex.find(handle);
    if (it == m_handleToIndex.end()) return;

    size_t idx = it->second;

    // Swap-erase to keep the vector dense
    if (idx + 1 < m_decals.size()) {
        m_decals[idx] = m_decals.back();

        // Patch the handle→index map for the element that was moved
        for (auto& [h, i] : m_handleToIndex) {
            if (i == m_decals.size() - 1 && h != handle) {
                i = idx;
                break;
            }
        }
    }
    m_decals.pop_back();
    m_handleToIndex.erase(it);
    m_decalsDirty = true;
}

void DecalPass::clearDecals() {
    m_decals.clear();
    m_handleToIndex.clear();
    m_decalsDirty = true;
}

// ---------------------------------------------------------------------------
// Resource connections
// ---------------------------------------------------------------------------

void DecalPass::setGBufferAlbedo(VkImageView albedoView, VkFormat albedoFormat) {
    bool formatChanged = (albedoFormat != m_albedoFormat);
    m_albedoView   = albedoView;
    m_albedoFormat = albedoFormat;

    // If the render pass format changed we need to rebuild everything
    if (formatChanged && m_renderPass != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        safeDestroy(m_pipeline);
        safeDestroy(m_pipelineLayout);
        safeDestroy(m_renderPass);
    }

    // Create render pass if not yet created
    if (m_renderPass == VK_NULL_HANDLE) {
        if (!createRenderPass()) {
            std::cerr << "DecalPass: failed to create render pass\n";
            return;
        }
        if (!createPipeline()) {
            std::cerr << "DecalPass: failed to create pipeline\n";
            return;
        }
    }

    // (Re)create framebuffer
    destroyFramebuffer();
    if (albedoView != VK_NULL_HANDLE) {
        createFramebuffer();
    }
}

void DecalPass::setDepthBuffer(VkImageView depthView, VkSampler depthSampler) {
    bool changed = (depthView != m_depthView || depthSampler != m_depthSampler);
    m_depthView    = depthView;
    m_depthSampler = depthSampler;
    if (changed) m_descriptorDirty = true;
}

void DecalPass::setMatrices(const glm::mat4& viewProj,
                             const glm::mat4& invViewProj,
                             const glm::vec2& screenSize) {
    m_viewProj    = viewProj;
    m_invViewProj = invViewProj;
    m_screenSize  = screenSize;
}

void DecalPass::setBindlessManager(BindlessTextureManager* mgr) {
    m_bindlessMgr = mgr;
}

// ---------------------------------------------------------------------------
// Private helpers — Vulkan resource creation
// ---------------------------------------------------------------------------

bool DecalPass::createRenderPass() {
    // Single colour attachment: GBuffer albedo.
    // LOAD_OP_LOAD: preserve existing geometry albedo; decals blend on top.
    // initialLayout = SHADER_READ_ONLY_OPTIMAL  (left by GBuffer pass)
    // finalLayout   = SHADER_READ_ONLY_OPTIMAL  (needed by deferred lighting)
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = m_albedoFormat;
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

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Dependency 0: SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL on subpass entry
    // Dependency 1: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL on subpass exit
    std::array<VkSubpassDependency, 2> deps{};

    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAtt;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool DecalPass::createFramebuffer() {
    if (m_albedoView == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE) return false;

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &m_albedoView;
    fbInfo.width           = m_width;
    fbInfo.height          = m_height;
    fbInfo.layers          = 1;

    return vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

void DecalPass::destroyFramebuffer() {
    safeDestroy(m_framebuffer);
}

bool DecalPass::createDescriptors() {
    // Set 0:
    //   binding 0 — DecalGPU SSBO  (vertex + fragment stages)
    //   binding 1 — depth buffer   (combined image sampler, fragment stage)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                    &m_descriptorLayout0) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                               &m_descriptorPool0) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool0;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout0;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet0) != VK_SUCCESS) {
        return false;
    }

    // Write the SSBO binding immediately (the buffer exists and won't change address)
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = m_decalBuffer;
    bufInfo.offset = 0;
    bufInfo.range  = static_cast<VkDeviceSize>(MAX_DECALS * sizeof(DecalGPU));

    VkWriteDescriptorSet ssboWrite{};
    ssboWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssboWrite.dstSet          = m_descriptorSet0;
    ssboWrite.dstBinding      = 0;
    ssboWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssboWrite.descriptorCount = 1;
    ssboWrite.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(m_device, 1, &ssboWrite, 0, nullptr);

    // Depth sampler binding is written lazily when setDepthBuffer() is called
    // (or inside updateDescriptors() / execute()).

    return true;
}

bool DecalPass::createPipeline() {
    // Load shaders: decal/decal.vert → decal_decal.vert.spv
    VkShaderModule vertShader = loadShaderModule("decal_decal.vert.spv");
    VkShaderModule fragShader = loadShaderModule("decal_decal.frag.spv");

    if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
        if (vertShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, vertShader, nullptr);
        if (fragShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, fragShader, nullptr);
        std::cerr << "DecalPass: failed to load shader modules\n";
        return false;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName  = "main";

    // Vertex input: location 0 = vec3 position (12-byte stride)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(float) * 3;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc.location = 0;
    attrDesc.binding  = 0;
    attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attrDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // Backface culling disabled: camera can be inside the decal OBB.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No depth attachment in this render pass.
    // Depth testing against the scene is done manually inside the fragment shader
    // by reading the GBuffer depth buffer via the combined image sampler.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Alpha blending: decal composites over existing GBuffer albedo.
    // Blend equation: result = srcAlpha * src + (1 - srcAlpha) * dst
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
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    // Push constant covers both vertex and fragment stages
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(PushConstants);

    // Pipeline layout:
    //   set 0 — our SSBO + depth sampler (m_descriptorLayout0)
    //   set 1 — bindless textures from BindlessTextureManager (if present)
    std::vector<VkDescriptorSetLayout> setLayouts;
    setLayouts.push_back(m_descriptorLayout0);
    if (m_bindlessMgr != nullptr) {
        setLayouts.push_back(m_bindlessMgr->getDescriptorSetLayout());
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts            = setLayouts.data();
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
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}

bool DecalPass::createUnitCubeMesh() {
    // Unit cube vertices: 8 corners of [-1,1]^3
    // Only position (vec3) — matches the vertex shader input.
    static const float cubeVerts[8 * 3] = {
        // x      y      z
        -1.0f, -1.0f, -1.0f,  // 0
         1.0f, -1.0f, -1.0f,  // 1
         1.0f,  1.0f, -1.0f,  // 2
        -1.0f,  1.0f, -1.0f,  // 3
        -1.0f, -1.0f,  1.0f,  // 4
         1.0f, -1.0f,  1.0f,  // 5
         1.0f,  1.0f,  1.0f,  // 6
        -1.0f,  1.0f,  1.0f,  // 7
    };

    // 6 faces × 2 triangles × 3 indices = 36 indices
    static const uint16_t cubeIndices[36] = {
        // -Z face
        0, 2, 1,  0, 3, 2,
        // +Z face
        4, 5, 6,  4, 6, 7,
        // -X face
        0, 7, 3,  0, 4, 7,
        // +X face
        1, 2, 6,  1, 6, 5,
        // -Y face
        0, 1, 5,  0, 5, 4,
        // +Y face
        3, 7, 6,  3, 6, 2,
    };

    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags props,
                             VkBuffer& outBuf, VkDeviceMemory& outMem) -> bool
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size        = size;
        bufInfo.usage       = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufInfo, nullptr, &outBuf) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_device, outBuf, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, props);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &outMem) != VK_SUCCESS)
            return false;

        return vkBindBufferMemory(m_device, outBuf, outMem, 0) == VK_SUCCESS;
    };

    VkDeviceSize vbSize = sizeof(cubeVerts);
    VkDeviceSize ibSize = sizeof(cubeIndices);

    if (!createBuffer(vbSize,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_cubeVB, m_cubeVBMem)) {
        return false;
    }
    if (!createBuffer(ibSize,
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_cubeIB, m_cubeIBMem)) {
        return false;
    }

    // Upload vertex data
    void* mapped = nullptr;
    vkMapMemory(m_device, m_cubeVBMem, 0, vbSize, 0, &mapped);
    std::memcpy(mapped, cubeVerts, static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, m_cubeVBMem);

    // Upload index data
    vkMapMemory(m_device, m_cubeIBMem, 0, ibSize, 0, &mapped);
    std::memcpy(mapped, cubeIndices, static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, m_cubeIBMem);

    return true;
}

bool DecalPass::createDecalBuffer() {
    VkDeviceSize bufSize = static_cast<VkDeviceSize>(MAX_DECALS * sizeof(DecalGPU));

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = bufSize;
    bufInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_decalBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, m_decalBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_decalMemory) != VK_SUCCESS)
        return false;

    if (vkBindBufferMemory(m_device, m_decalBuffer, m_decalMemory, 0) != VK_SUCCESS)
        return false;

    // Persistent map — we upload every time decals change
    if (vkMapMemory(m_device, m_decalMemory, 0, bufSize, 0, &m_decalMapped) != VK_SUCCESS)
        return false;

    // Zero-initialise
    std::memset(m_decalMapped, 0, static_cast<size_t>(bufSize));

    return true;
}

// ---------------------------------------------------------------------------
// Private helpers — runtime updates
// ---------------------------------------------------------------------------

void DecalPass::uploadDecals() {
    if (m_decalMapped == nullptr || m_decals.empty()) return;
    size_t byteCount = m_decals.size() * sizeof(DecalGPU);
    std::memcpy(m_decalMapped, m_decals.data(), byteCount);
}

void DecalPass::updateDescriptors() {
    if (m_descriptorSet0 == VK_NULL_HANDLE) return;
    if (m_depthView == VK_NULL_HANDLE || m_depthSampler == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = m_depthSampler;
    depthInfo.imageView   = m_depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet depthWrite{};
    depthWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    depthWrite.dstSet          = m_descriptorSet0;
    depthWrite.dstBinding      = 1;
    depthWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depthWrite.descriptorCount = 1;
    depthWrite.pImageInfo      = &depthInfo;

    vkUpdateDescriptorSets(m_device, 1, &depthWrite, 0, nullptr);
}

} // namespace ohao
