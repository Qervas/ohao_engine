#include "csm_pass.hpp"
#include "../../engine/scene/scene.hpp"
#include "../../engine/actor/actor.hpp"
#include "../../engine/component/transform_component.hpp"
#include "../../renderer/components/mesh_component.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>

namespace ohao {

CSMPass::~CSMPass() {
    cleanup();
}

bool CSMPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    if (!createShadowMap()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCascadeBuffer()) return false;
    if (!createPipeline()) return false;

    // Initialize cascade splits
    calculateCascadeSplits();

    return true;
}

void CSMPass::cleanup() {
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
    if (m_cascadeBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_cascadeBuffer, nullptr);
        m_cascadeBuffer = VK_NULL_HANDLE;
    }
    if (m_cascadeBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_cascadeBufferMemory, nullptr);
        m_cascadeBufferMemory = VK_NULL_HANDLE;
    }
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    for (auto& fb : m_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    for (auto& view : m_cascadeViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (m_shadowMapArrayView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_shadowMapArrayView, nullptr);
        m_shadowMapArrayView = VK_NULL_HANDLE;
    }
    if (m_shadowMap != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_shadowMap, nullptr);
        m_shadowMap = VK_NULL_HANDLE;
    }
    if (m_shadowMapMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_shadowMapMemory, nullptr);
        m_shadowMapMemory = VK_NULL_HANDLE;
    }
}

void CSMPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_scene) return;

    // Update cascade matrices based on current camera
    updateCascadeMatrices();

    // Update cascade buffer
    memcpy(m_cascadeBufferMapped, &m_cascadeData, sizeof(CascadeData));

    // Render each cascade
    for (uint32_t cascade = 0; cascade < CASCADE_COUNT; ++cascade) {
        VkClearValue clearValue{};
        clearValue.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_framebuffers[cascade];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Set viewport and scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(SHADOW_MAP_SIZE);
        viewport.height = static_cast<float>(SHADOW_MAP_SIZE);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Bind pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        // Bind vertex and index buffers if available
        if (m_vertexBuffer != VK_NULL_HANDLE && m_indexBuffer != VK_NULL_HANDLE) {
            VkBuffer vertexBuffers[] = {m_vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        // Iterate through scene objects and render to shadow map
        if (m_meshBufferMap) {
            const auto& actors = m_scene->getAllActors();
            for (const auto& [actorId, actor] : actors) {
                if (!actor) continue;

                // Get mesh component (shadow casters only)
                auto meshComp = actor->getComponent<MeshComponent>();
                if (!meshComp || !meshComp->isVisible()) continue;

                // Get transform
                auto transformComp = actor->getComponent<TransformComponent>();
                glm::mat4 modelMatrix = transformComp ? transformComp->getWorldMatrix() : glm::mat4(1.0f);

                // Find buffer info for this actor
                auto it = m_meshBufferMap->find(actorId);
                if (it == m_meshBufferMap->end()) continue;

                const MeshBufferInfo& bufferInfo = it->second;

                // Set push constants
                ShadowPushConstant push{};
                push.model = modelMatrix;
                push.cascadeIndex = cascade;
                vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(ShadowPushConstant), &push);

                // Draw indexed
                vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1,
                                bufferInfo.indexOffset, bufferInfo.vertexOffset, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
    }

    // Transition shadow map for shader read
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_shadowMap;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = CASCADE_COUNT;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void CSMPass::setCameraData(const glm::mat4& view, const glm::mat4& proj,
                            float nearPlane, float farPlane) {
    m_cameraView = view;
    m_cameraProj = proj;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
    calculateCascadeSplits();
}

bool CSMPass::createShadowMap() {
    // Create shadow map array (one layer per cascade)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = CASCADE_COUNT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_shadowMap) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_shadowMap, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_shadowMapMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_shadowMap, m_shadowMapMemory, 0);

    // Create array view (for shader sampling all cascades)
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowMap;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = CASCADE_COUNT;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_shadowMapArrayView) != VK_SUCCESS) {
        return false;
    }

    // Create per-cascade views (for rendering)
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange.baseArrayLayer = i;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_cascadeViews[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // Create shadow sampler with comparison
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // Outside = no shadow
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    return vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler) == VK_SUCCESS;
}

bool CSMPass::createRenderPass() {
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    return vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool CSMPass::createFramebuffers() {
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_cascadeViews[i];
        framebufferInfo.width = SHADOW_MAP_SIZE;
        framebufferInfo.height = SHADOW_MAP_SIZE;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool CSMPass::createCascadeBuffer() {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(CascadeData);
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_cascadeBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_cascadeBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_cascadeBufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_cascadeBuffer, m_cascadeBufferMemory, 0);
    vkMapMemory(m_device, m_cascadeBufferMemory, 0, sizeof(CascadeData), 0, &m_cascadeBufferMapped);

    return true;
}

bool CSMPass::createPipeline() {
    // Load shaders
    VkShaderModule vertShader = loadShaderModule("shadow_shadow_csm.vert.spv");
    VkShaderModule geomShader = loadShaderModule("shadow_shadow_csm.geom.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shaderStages[1].module = geomShader;
    shaderStages[1].pName = "main";

    // Vertex input (position only for shadow pass)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(float) * 3; // Position only
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDesc{};
    attributeDesc.binding = 0;
    attributeDesc.location = 0;
    attributeDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDesc.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    vertexInputInfo.pVertexAttributeDescriptions = &attributeDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_TRUE; // Clamp depth to [0, 1]
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // Front-face culling for shadow
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 0; // No color attachments

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Push constants for model matrix and cascade index
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ShadowPushConstant);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, geomShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
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
    pipelineInfo.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, geomShader, nullptr);

    return result == VK_SUCCESS;
}

void CSMPass::calculateCascadeSplits() {
    float range = m_farPlane - m_nearPlane;
    float ratio = m_farPlane / m_nearPlane;

    m_cascadeSplits[0] = m_nearPlane;

    for (uint32_t i = 1; i <= CASCADE_COUNT; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(CASCADE_COUNT);

        // Logarithmic split
        float log = m_nearPlane * std::pow(ratio, p);

        // Linear split
        float linear = m_nearPlane + range * p;

        // Blend between log and linear
        float d = m_splitLambda * log + (1.0f - m_splitLambda) * linear;

        m_cascadeSplits[i] = d;
    }

    // Store split depths (view space) for shader
    m_cascadeData.splitDepths = glm::vec4(
        m_cascadeSplits[1],
        m_cascadeSplits[2],
        m_cascadeSplits[3],
        m_cascadeSplits[4]
    );
}

void CSMPass::updateCascadeMatrices() {
    glm::mat4 invCameraView = glm::inverse(m_cameraView);

    for (uint32_t cascade = 0; cascade < CASCADE_COUNT; ++cascade) {
        m_cascadeData.viewProj[cascade] = calculateLightViewProj(cascade);
    }
}

glm::mat4 CSMPass::calculateLightViewProj(uint32_t cascade) {
    float nearSplit = m_cascadeSplits[cascade];
    float farSplit = m_cascadeSplits[cascade + 1];

    // Get frustum corners in world space
    glm::mat4 invViewProj = glm::inverse(m_cameraProj * m_cameraView);

    std::array<glm::vec3, 8> frustumCorners;
    uint32_t idx = 0;

    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                glm::vec4 pt = invViewProj * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    z == 0 ? 0.0f : 1.0f, // NDC depth [0, 1] in Vulkan
                    1.0f
                );
                frustumCorners[idx++] = glm::vec3(pt) / pt.w;
            }
        }
    }

    // Adjust frustum corners to cascade split range
    for (uint32_t i = 0; i < 4; ++i) {
        glm::vec3 dist = frustumCorners[i + 4] - frustumCorners[i];
        frustumCorners[i + 4] = frustumCorners[i] + dist * (farSplit - m_nearPlane) / (m_farPlane - m_nearPlane);
        frustumCorners[i] = frustumCorners[i] + dist * (nearSplit - m_nearPlane) / (m_farPlane - m_nearPlane);
    }

    // Calculate frustum center
    glm::vec3 center(0.0f);
    for (const auto& corner : frustumCorners) {
        center += corner;
    }
    center /= 8.0f;

    // Calculate bounding sphere radius for stable shadow edges
    float radius = 0.0f;
    for (const auto& corner : frustumCorners) {
        float dist = glm::length(corner - center);
        radius = std::max(radius, dist);
    }

    // Round to texel size for stable shadows
    float texelsPerUnit = static_cast<float>(SHADOW_MAP_SIZE) / (radius * 2.0f);
    glm::mat4 scaleBias = glm::scale(glm::mat4(1.0f), glm::vec3(texelsPerUnit));

    // Light view matrix
    glm::vec3 up = std::abs(glm::dot(m_lightDirection, glm::vec3(0, 1, 0))) > 0.99f ?
                   glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::mat4 lightView = glm::lookAt(center - m_lightDirection * radius, center, up);

    // Snap to texel grid
    lightView = scaleBias * lightView;
    glm::vec4 shadowOrigin = lightView * glm::vec4(0, 0, 0, 1);
    shadowOrigin.x = std::round(shadowOrigin.x);
    shadowOrigin.y = std::round(shadowOrigin.y);
    glm::vec4 roundedOrigin = shadowOrigin - lightView * glm::vec4(0, 0, 0, 1);
    lightView[3][0] += roundedOrigin.x;
    lightView[3][1] += roundedOrigin.y;
    lightView = glm::inverse(scaleBias) * lightView;

    // Orthographic projection
    glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, radius * 2.0f);

    return lightProj * lightView;
}

} // namespace ohao
