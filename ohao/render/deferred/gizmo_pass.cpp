#include "gizmo_pass.hpp"
#include <array>
#include <cstring>
#include <iostream>

namespace ohao {

GizmoPass::~GizmoPass() {
    cleanup();
}

bool GizmoPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    // Generate initial geometry
    GizmoMeshes::generateTranslationGizmo(m_vertices, m_indices);
    m_geometryDirty = true;

    if (!createPipeline()) {
        std::cerr << "GizmoPass: Failed to create pipeline" << std::endl;
        return false;
    }
    if (!createVertexBuffers()) {
        std::cerr << "GizmoPass: Failed to create vertex buffers" << std::endl;
        return false;
    }

    return true;
}

void GizmoPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    destroyFramebuffer();
    destroyVertexBuffers();

    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
}

void GizmoPass::setGizmoMode(GizmoMode mode) {
    if (m_currentMode == mode) return;
    m_currentMode = mode;

    switch (mode) {
        case GizmoMode::TRANSLATE:
            GizmoMeshes::generateTranslationGizmo(m_vertices, m_indices);
            break;
        case GizmoMode::ROTATE:
            GizmoMeshes::generateRotationGizmo(m_vertices, m_indices);
            break;
        case GizmoMode::SCALE:
            GizmoMeshes::generateScaleGizmo(m_vertices, m_indices);
            break;
    }
    m_geometryDirty = true;
}

void GizmoPass::setGizmoTransform(const glm::mat4& model) {
    m_gizmoModel = model;
}

void GizmoPass::setViewProjection(const glm::mat4& viewProj) {
    m_viewProj = viewProj;
}

void GizmoPass::setTargetImage(VkImage image, VkImageView imageView) {
    if (m_targetImage == image && m_targetImageView == imageView) return;

    destroyFramebuffer();

    m_targetImage = image;
    m_targetImageView = imageView;

    if (imageView == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE) return;

    // Create framebuffer from the target image view
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_targetImageView;
    fbInfo.width = m_width;
    fbInfo.height = m_height;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) != VK_SUCCESS) {
        std::cerr << "GizmoPass: Failed to create framebuffer" << std::endl;
        m_framebuffer = VK_NULL_HANDLE;
    }
}

void GizmoPass::destroyFramebuffer() {
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
}

void GizmoPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled || m_indexCount == 0) return;
    if (m_framebuffer == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE) return;
    if (m_targetImage == VK_NULL_HANDLE) return;

    // Update geometry buffers if needed
    if (m_geometryDirty) {
        updateGizmoBuffers();
    }

    if (m_vertexBuffer == VK_NULL_HANDLE || m_indexBuffer == VK_NULL_HANDLE) return;

    // Transition target image to COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_targetImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Begin render pass (LOAD_OP_LOAD preserves existing content)
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = m_renderPass;
    rpInfo.framebuffer = m_framebuffer;
    rpInfo.renderArea.extent = {m_width, m_height};
    rpInfo.clearValueCount = 0;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Push constants
    GizmoPushConstants pc{};
    pc.viewProj = m_viewProj;
    pc.model = m_gizmoModel;
    pc.highlightColor = glm::vec4(GizmoMeshes::HIGHLIGHT_COLOR, 0.0f);

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(GizmoPushConstants), &pc);

    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);

    // Transition back to SHADER_READ_ONLY_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // finalLayout from render pass
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void GizmoPass::onResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    // Recreate framebuffer on resize (target image view will be reset)
    destroyFramebuffer();
    m_targetImage = VK_NULL_HANDLE;
    m_targetImageView = VK_NULL_HANDLE;
}

bool GizmoPass::createPipeline() {
    VkShaderModule vertShader = loadShaderModule("overlay_gizmo.vert.spv");
    VkShaderModule fragShader = loadShaderModule("overlay_gizmo.frag.spv");

    if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
        std::cerr << "GizmoPass: Failed to load shaders" << std::endl;
        if (vertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertShader, nullptr);
        if (fragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";

    // Vertex input: GizmoVertex = position(vec3) + color(vec3)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(GizmoVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
    attrDescs[0].binding = 0;
    attrDescs[0].location = 0;
    attrDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[0].offset = offsetof(GizmoVertex, position);
    attrDescs[1].binding = 0;
    attrDescs[1].location = 1;
    attrDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[1].offset = offsetof(GizmoVertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 2.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test disabled - gizmos always render on top
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(GizmoPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Create render pass with LOAD_OP_LOAD for compositing on top
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R8G8B8A8_SRGB;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}

bool GizmoPass::createVertexBuffers() {
    if (m_vertices.empty() || m_indices.empty()) return true;

    VkDeviceSize vertexBufferSize = sizeof(GizmoVertex) * m_vertices.size();
    VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_indices.size();

    // Create vertex buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = vertexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexMemory, 0);

    void* data;
    vkMapMemory(m_device, m_vertexMemory, 0, vertexBufferSize, 0, &data);
    std::memcpy(data, m_vertices.data(), vertexBufferSize);
    vkUnmapMemory(m_device, m_vertexMemory);

    // Create index buffer
    bufferInfo.size = indexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_indexBuffer) != VK_SUCCESS) {
        return false;
    }

    vkGetBufferMemoryRequirements(m_device, m_indexBuffer, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_indexMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_indexBuffer, m_indexMemory, 0);

    vkMapMemory(m_device, m_indexMemory, 0, indexBufferSize, 0, &data);
    std::memcpy(data, m_indices.data(), indexBufferSize);
    vkUnmapMemory(m_device, m_indexMemory);

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    m_geometryDirty = false;

    return true;
}

void GizmoPass::destroyVertexBuffers() {
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_vertexMemory, nullptr);
        m_vertexMemory = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    if (m_indexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_indexMemory, nullptr);
        m_indexMemory = VK_NULL_HANDLE;
    }
}

void GizmoPass::updateGizmoBuffers() {
    destroyVertexBuffers();
    createVertexBuffers();
}

} // namespace ohao
